/* rc-client.c - Minimal Rocket.Chat REST + realtime client (libsoup 3) */

#include "rc-client.h"

#include <string.h>
#include <libsoup/soup.h>

#define RECONNECT_SECONDS 5
#define USER_AGENT "rocket-client/0.2"

struct _RcClient {
  SoupSession  *session;
  GCancellable *cancellable;

  GPtrArray    *candidates;   /* base URLs to try for login, in order */
  char         *base_url;     /* active base URL, no trailing slash */

  char         *user_id;
  char         *token;

  /* certificate pinning */
  char                 *trust_file;
  GHashTable           *pins;           /* "host:port" -> "AB:CD:…" */
  GTlsCertificate      *rejected_cert;
  GTlsCertificateFlags  rejected_errors;
  char                 *rejected_host;

  /* realtime */
  RcRealtimeFunc          rt_func;
  gpointer                rt_data;
  gboolean                rt_wanted;
  gboolean                rt_ready;
  GCancellable           *rt_cancellable;
  SoupWebsocketConnection *ws;
  guint                   rt_reconnect_id;
  guint                   rt_next_id;
  char                   *rt_room;       /* watched room */
  GPtrArray              *rt_room_subs;  /* sub ids for rt_room */
};

G_DEFINE_QUARK (rc-client-error-quark, rc_client_error)

gboolean
rc_error_is_cancelled (const GError *error)
{
  return error != NULL && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* ------------------------------------------------------------------------ */
/* lifetime                                                                  */

RcClient *
rc_client_new (void)
{
  RcClient *self = g_rc_box_new0 (RcClient);

  self->session = soup_session_new_with_options ("user-agent", USER_AGENT,
                                                 "timeout", 60,
                                                 NULL);
  self->cancellable = g_cancellable_new ();
  self->candidates = g_ptr_array_new_with_free_func (g_free);
  self->pins = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->rt_room_subs = g_ptr_array_new_with_free_func (g_free);
  return self;
}

RcClient *
rc_client_ref (RcClient *self)
{
  return g_rc_box_acquire (self);
}

static void
rc_client_clear (RcClient *self)
{
  rc_client_shutdown (self);
  g_clear_object (&self->session);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->candidates, g_ptr_array_unref);
  g_clear_pointer (&self->base_url, g_free);
  g_clear_pointer (&self->user_id, g_free);
  g_clear_pointer (&self->token, g_free);
  g_clear_pointer (&self->trust_file, g_free);
  g_clear_pointer (&self->pins, g_hash_table_unref);
  g_clear_object (&self->rejected_cert);
  g_clear_pointer (&self->rejected_host, g_free);
  g_clear_pointer (&self->rt_room, g_free);
  g_clear_pointer (&self->rt_room_subs, g_ptr_array_unref);
}

void
rc_client_unref (RcClient *self)
{
  g_rc_box_release_full (self, (GDestroyNotify) rc_client_clear);
}

void
rc_client_shutdown (RcClient *self)
{
  rc_client_realtime_stop (self);
  if (self->cancellable)
    {
      g_cancellable_cancel (self->cancellable);
      g_object_unref (self->cancellable);
      self->cancellable = g_cancellable_new ();
    }
}

/* ------------------------------------------------------------------------ */
/* server / credentials                                                      */

static char *
normalize_base (const char *url, GError **error)
{
  g_autoptr(GUri) uri = g_uri_parse (url, SOUP_HTTP_URI_FLAGS, NULL);
  g_autofree char *path = NULL;
  g_autofree char *s = NULL;
  const char *scheme;

  if (uri == NULL || g_uri_get_host (uri) == NULL || *g_uri_get_host (uri) == '\0')
    {
      g_set_error (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_INVALID_URL,
                   "“%s” is not a valid server address", url);
      return NULL;
    }

  scheme = g_uri_get_scheme (uri);
  if (scheme == NULL)
    {
      g_set_error (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_INVALID_URL,
                   "“%s” is not a valid server address", url);
      return NULL;
    }
  if (g_ascii_strcasecmp (scheme, "http") != 0 && g_ascii_strcasecmp (scheme, "https") != 0)
    {
      g_set_error (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_INVALID_URL,
                   "Unsupported scheme “%s” (use http or https)", scheme);
      return NULL;
    }

  path = g_strdup (g_uri_get_path (uri));
  while (*path && path[strlen (path) - 1] == '/')
    path[strlen (path) - 1] = '\0';

  /* rebuild without query/fragment; keep only a non-default port */
  {
    int port = g_uri_get_port (uri);
    g_autoptr(GUri) clean = NULL;

    if ((port == 443 && g_ascii_strcasecmp (scheme, "https") == 0) ||
        (port == 80 && g_ascii_strcasecmp (scheme, "http") == 0))
      port = -1;
    clean = g_uri_build (G_URI_FLAGS_NONE, scheme, NULL, g_uri_get_host (uri),
                         port, path, NULL, NULL);
    s = g_uri_to_string (clean);
  }
  return g_steal_pointer (&s);
}

gboolean
rc_client_set_server (RcClient    *self,
                      const char  *input,
                      RcConnMode   mode,
                      GError     **error)
{
  g_autofree char *trimmed = g_strstrip (g_strdup (input ? input : ""));
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func (g_free);

  if (*trimmed == '\0')
    {
      g_set_error_literal (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_INVALID_URL,
                           "Please enter a server address");
      return FALSE;
    }

  if (strstr (trimmed, "://") != NULL)
    {
      /* user typed an explicit scheme – honour it */
      char *u = normalize_base (trimmed, error);
      if (!u)
        return FALSE;
      g_ptr_array_add (urls, u);
    }
  else
    {
      if (mode == RC_CONN_AUTO || mode == RC_CONN_HTTPS)
        {
          g_autofree char *raw = g_strconcat ("https://", trimmed, NULL);
          char *u = normalize_base (raw, error);
          if (!u)
            return FALSE;
          g_ptr_array_add (urls, u);
        }
      if (mode == RC_CONN_AUTO || mode == RC_CONN_HTTP)
        {
          g_autofree char *raw = g_strconcat ("http://", trimmed, NULL);
          char *u = normalize_base (raw, error);
          if (!u)
            return FALSE;
          g_ptr_array_add (urls, u);
        }
    }

  g_ptr_array_unref (self->candidates);
  self->candidates = g_steal_pointer (&urls);
  g_free (self->base_url);
  self->base_url = g_strdup (g_ptr_array_index (self->candidates, 0));
  return TRUE;
}

const char *
rc_client_get_base_url (RcClient *self)
{
  return self->base_url;
}

gboolean
rc_client_is_secure (RcClient *self)
{
  return self->base_url && g_str_has_prefix (self->base_url, "https://");
}

void
rc_client_set_credentials (RcClient   *self,
                           const char *user_id,
                           const char *token)
{
  g_free (self->user_id);
  g_free (self->token);
  self->user_id = g_strdup (user_id);
  self->token = g_strdup (token);
}

const char *
rc_client_get_user_id (RcClient *self)
{
  return self->user_id;
}

const char *
rc_client_get_token (RcClient *self)
{
  return self->token;
}

/* ------------------------------------------------------------------------ */
/* certificate trust                                                         */

char *
rc_cert_fingerprint (GTlsCertificate *cert)
{
  g_autoptr(GByteArray) der = NULL;
  g_autofree char *hex = NULL;
  GString *out;

  g_object_get (cert, "certificate", &der, NULL);
  if (der == NULL)
    return g_strdup ("");
  hex = g_compute_checksum_for_data (G_CHECKSUM_SHA256, der->data, der->len);
  out = g_string_new (NULL);
  for (gsize i = 0; hex[i]; i += 2)
    {
      if (i)
        g_string_append_c (out, ':');
      g_string_append_c (out, g_ascii_toupper (hex[i]));
      g_string_append_c (out, g_ascii_toupper (hex[i + 1]));
    }
  return g_string_free (out, FALSE);
}

char *
rc_cert_describe_errors (GTlsCertificateFlags errors)
{
  GString *s = g_string_new (NULL);

#define ADD(flag, text) if (errors & (flag)) g_string_append_printf (s, "%s• %s", s->len ? "\n" : "", text)
  ADD (G_TLS_CERTIFICATE_UNKNOWN_CA, "Not signed by a trusted authority (self-signed or private CA)");
  ADD (G_TLS_CERTIFICATE_BAD_IDENTITY, "Issued for a different host name / IP address");
  ADD (G_TLS_CERTIFICATE_NOT_ACTIVATED, "Not valid yet");
  ADD (G_TLS_CERTIFICATE_EXPIRED, "Expired");
  ADD (G_TLS_CERTIFICATE_REVOKED, "Revoked");
  ADD (G_TLS_CERTIFICATE_INSECURE, "Uses an insecure algorithm");
  ADD (G_TLS_CERTIFICATE_GENERIC_ERROR, "Could not be validated");
#undef ADD
  if (s->len == 0)
    g_string_append (s, "• Not trusted");
  return g_string_free (s, FALSE);
}

static char *
host_key (GUri *uri)
{
  int port = g_uri_get_port (uri);

  if (port <= 0)
    port = g_ascii_strcasecmp (g_uri_get_scheme (uri), "https") == 0 ||
           g_ascii_strcasecmp (g_uri_get_scheme (uri), "wss") == 0 ? 443 : 80;
  return g_strdup_printf ("%s:%d", g_uri_get_host (uri), port);
}

void
rc_client_set_trust_file (RcClient   *self,
                          const char *path)
{
  g_autoptr(GKeyFile) kf = g_key_file_new ();
  g_auto(GStrv) keys = NULL;

  g_free (self->trust_file);
  self->trust_file = g_strdup (path);
  g_hash_table_remove_all (self->pins);

  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
    return;
  keys = g_key_file_get_keys (kf, "pins", NULL, NULL);
  for (guint i = 0; keys && keys[i]; i++)
    g_hash_table_insert (self->pins, g_strdup (keys[i]),
                         g_key_file_get_string (kf, "pins", keys[i], NULL));
}

GTlsCertificate *
rc_client_get_rejected_certificate (RcClient             *self,
                                    const char          **host,
                                    GTlsCertificateFlags *errors)
{
  if (host)
    *host = self->rejected_host;
  if (errors)
    *errors = self->rejected_errors;
  return self->rejected_cert;
}

void
rc_client_trust_rejected_certificate (RcClient *self)
{
  g_autoptr(GKeyFile) kf = g_key_file_new ();
  g_autofree char *fp = NULL;
  g_autofree char *dir = NULL;

  if (self->rejected_cert == NULL || self->rejected_host == NULL)
    return;

  fp = rc_cert_fingerprint (self->rejected_cert);
  g_hash_table_insert (self->pins, g_strdup (self->rejected_host), g_strdup (fp));

  if (self->trust_file)
    {
      g_key_file_load_from_file (kf, self->trust_file, G_KEY_FILE_KEEP_COMMENTS, NULL);
      g_key_file_set_string (kf, "pins", self->rejected_host, fp);
      dir = g_path_get_dirname (self->trust_file);
      g_mkdir_with_parents (dir, 0700);
      g_key_file_save_to_file (kf, self->trust_file, NULL);
    }
  g_clear_object (&self->rejected_cert);
  g_clear_pointer (&self->rejected_host, g_free);
}

static gboolean
on_accept_certificate (SoupMessage          *msg,
                       GTlsCertificate      *cert,
                       GTlsCertificateFlags  errors,
                       gpointer              data)
{
  RcClient *self = data;
  g_autofree char *key = host_key (soup_message_get_uri (msg));
  g_autofree char *fp = rc_cert_fingerprint (cert);
  const char *pinned = g_hash_table_lookup (self->pins, key);

  if (pinned && g_str_equal (pinned, fp))
    return TRUE;

  g_set_object (&self->rejected_cert, cert);
  self->rejected_errors = errors;
  g_free (self->rejected_host);
  self->rejected_host = g_steal_pointer (&key);
  return FALSE;
}

/* turn soup's TLS failure into RC_CLIENT_ERROR_UNTRUSTED_CERT */
static void
map_tls_error (RcClient    *self,
               SoupMessage *msg,
               GError     **error)
{
  g_autofree char *key = NULL;

  if (*error == NULL || (*error)->domain != G_TLS_ERROR || self->rejected_cert == NULL)
    return;
  key = host_key (soup_message_get_uri (msg));
  if (g_strcmp0 (key, self->rejected_host) != 0)
    return;
  g_clear_error (error);
  g_set_error (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_UNTRUSTED_CERT,
               "The TLS certificate of %s is not trusted", key);
}

/* ------------------------------------------------------------------------ */
/* HTTP                                                                      */

typedef struct {
  RcClient      *client;
  GCancellable  *cancellable;
  SoupMessage   *msg;
  RcResponseFunc func;
  RcBytesFunc    bytes_func;
  gpointer       user_data;
} Request;

static void
request_free (Request *r)
{
  if (r->msg)
    g_signal_handlers_disconnect_by_data (r->msg, r->client);
  g_clear_object (&r->msg);
  g_clear_object (&r->cancellable);
  rc_client_unref (r->client);
  g_free (r);
}

static void
prepare_message (RcClient    *self,
                 SoupMessage *msg,
                 gboolean     with_auth)
{
  SoupMessageHeaders *h = soup_message_get_request_headers (msg);

  if (with_auth && self->user_id && self->token)
    {
      soup_message_headers_replace (h, "X-User-Id", self->user_id);
      soup_message_headers_replace (h, "X-Auth-Token", self->token);
    }
  g_signal_connect (msg, "accept-certificate", G_CALLBACK (on_accept_certificate), self);
}

static SoupMessage *
build_message (RcClient   *self,
               const char *method,
               const char *base,
               const char *path,
               JsonNode   *body)
{
  g_autofree char *url = g_strconcat (base, path, NULL);
  SoupMessage *msg = soup_message_new (method, url);

  if (msg == NULL)
    return NULL;

  soup_message_headers_append (soup_message_get_request_headers (msg), "Accept", "application/json");
  prepare_message (self, msg, TRUE);

  if (body)
    {
      char *data = json_to_string (body, FALSE);
      gsize len = strlen (data);
      g_autoptr(GBytes) bytes = g_bytes_new_take (data, len);
      soup_message_set_request_body_from_bytes (msg, "application/json", bytes);
    }

  return msg;
}

static JsonObject *
parse_response (SoupMessage *msg,
                GBytes      *bytes,
                GError     **error)
{
  guint status = soup_message_get_status (msg);
  g_autoptr(JsonParser) parser = json_parser_new ();
  gsize len = 0;
  const char *data = g_bytes_get_data (bytes, &len);
  JsonNode *root;
  JsonObject *obj;
  gboolean ok_status = SOUP_STATUS_IS_SUCCESSFUL (status);
  int code = status == 401 ? RC_CLIENT_ERROR_AUTH :
             status == 404 ? RC_CLIENT_ERROR_NOT_FOUND : RC_CLIENT_ERROR_API;

  if (len == 0 || !json_parser_load_from_data (parser, data, len, NULL) ||
      (root = json_parser_get_root (parser)) == NULL ||
      !JSON_NODE_HOLDS_OBJECT (root))
    {
      if (!ok_status)
        g_set_error (error, RC_CLIENT_ERROR,
                     code == RC_CLIENT_ERROR_API ? RC_CLIENT_ERROR_HTTP : code,
                     "HTTP %u %s", status,
                     soup_message_get_reason_phrase (msg) ? soup_message_get_reason_phrase (msg) : "");
      else
        g_set_error_literal (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_PARSE,
                             "The server did not reply like a Rocket.Chat server");
      return NULL;
    }

  obj = json_node_get_object (root);

  if (!ok_status ||
      (json_object_has_member (obj, "success") && !json_object_get_boolean_member_with_default (obj, "success", TRUE)) ||
      g_strcmp0 (json_object_get_string_member_with_default (obj, "status", NULL), "error") == 0)
    {
      const char *m = json_object_get_string_member_with_default (obj, "error", NULL);
      if (m == NULL || *m == '\0')
        m = json_object_get_string_member_with_default (obj, "message", NULL);
      if (m == NULL || *m == '\0')
        m = json_object_get_string_member_with_default (obj, "reason", NULL);
      g_set_error (error, RC_CLIENT_ERROR, code, "%s", m ? m : "Request failed");
      return NULL;
    }

  return json_object_ref (obj);
}

static void
on_json_sent (GObject      *source,
              GAsyncResult *res,
              gpointer      data)
{
  Request *r = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(JsonObject) obj = NULL;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &error);

  if (g_cancellable_is_cancelled (r->cancellable))
    {
      g_clear_error (&error);
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
    }
  else if (error == NULL)
    obj = parse_response (r->msg, bytes, &error);
  else
    map_tls_error (r->client, r->msg, &error);

  r->func (r->client, obj, error, r->user_data);
  request_free (r);
}

static void
send_message (RcClient      *self,
              SoupMessage   *msg,
              RcResponseFunc func,
              gpointer       user_data)
{
  Request *r = g_new0 (Request, 1);

  r->client = rc_client_ref (self);
  r->cancellable = g_object_ref (self->cancellable);
  r->msg = msg;
  r->func = func;
  r->user_data = user_data;
  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_DEFAULT,
                                    r->cancellable, on_json_sent, r);
}

static void
fail_now (RcClient      *self,
          const char    *what,
          RcResponseFunc func,
          gpointer       user_data)
{
  g_autoptr(GError) error = g_error_new (RC_CLIENT_ERROR, RC_CLIENT_ERROR_INVALID_URL,
                                         "Invalid request URL for %s", what);
  func (self, NULL, error, user_data);
}

static void
send_json (RcClient      *self,
           const char    *method,
           const char    *path,
           JsonNode      *body,
           RcResponseFunc func,
           gpointer       user_data)
{
  SoupMessage *msg = self->base_url ? build_message (self, method, self->base_url, path, body) : NULL;

  if (body)
    json_node_unref (body);
  if (msg == NULL)
    fail_now (self, path, func, user_data);
  else
    send_message (self, msg, func, user_data);
}

void
rc_client_get (RcClient      *self,
               const char    *path,
               RcResponseFunc func,
               gpointer       user_data)
{
  send_json (self, "GET", path, NULL, func, user_data);
}

void
rc_client_post (RcClient      *self,
                const char    *path,
                JsonNode      *body,
                RcResponseFunc func,
                gpointer       user_data)
{
  if (body == NULL)
    {
      body = json_node_new (JSON_NODE_OBJECT);
      json_node_take_object (body, json_object_new ());
    }
  send_json (self, "POST", path, body, func, user_data);
}

void
rc_client_upload (RcClient           *self,
                  const char         *path,
                  const char         *filename,
                  const char         *content_type,
                  GBytes             *data,
                  const char * const *fields,
                  RcResponseFunc      func,
                  gpointer            user_data)
{
  g_autoptr(SoupMultipart) mp = soup_multipart_new (SOUP_FORM_MIME_TYPE_MULTIPART);
  g_autofree char *url = NULL;
  SoupMessage *msg;

  if (self->base_url == NULL)
    {
      fail_now (self, path, func, user_data);
      return;
    }

  soup_multipart_append_form_file (mp, "file", filename,
                                   content_type ? content_type : "application/octet-stream",
                                   data);
  for (guint i = 0; fields && fields[i] && fields[i + 1]; i += 2)
    if (*fields[i + 1])
      soup_multipart_append_form_string (mp, fields[i], fields[i + 1]);

  url = g_strconcat (self->base_url, path, NULL);
  msg = soup_message_new_from_multipart (url, mp);
  if (msg == NULL)
    {
      fail_now (self, path, func, user_data);
      return;
    }
  soup_message_headers_append (soup_message_get_request_headers (msg), "Accept", "application/json");
  prepare_message (self, msg, TRUE);
  send_message (self, msg, func, user_data);
}

static void
on_bytes_sent (GObject      *source,
               GAsyncResult *res,
               gpointer      data)
{
  Request *r = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = NULL;
  const char *ctype = NULL;

  bytes = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &error);

  if (g_cancellable_is_cancelled (r->cancellable))
    {
      g_clear_error (&error);
      g_clear_pointer (&bytes, g_bytes_unref);
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
    }
  else if (error == NULL && !SOUP_STATUS_IS_SUCCESSFUL (soup_message_get_status (r->msg)))
    {
      g_clear_pointer (&bytes, g_bytes_unref);
      g_set_error (&error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_HTTP, "HTTP %u",
                   soup_message_get_status (r->msg));
    }
  else if (error)
    map_tls_error (r->client, r->msg, &error);
  else
    ctype = soup_message_headers_get_content_type (soup_message_get_response_headers (r->msg), NULL);

  r->bytes_func (r->client, bytes, ctype, error, r->user_data);
  request_free (r);
}

void
rc_client_fetch_bytes (RcClient    *self,
                       const char  *path,
                       RcBytesFunc  func,
                       gpointer     user_data)
{
  Request *r;
  SoupMessage *msg = NULL;
  gboolean own_server = TRUE;

  if (path && strstr (path, "://"))
    {
      msg = soup_message_new ("GET", path);
      own_server = self->base_url && g_str_has_prefix (path, self->base_url);
    }
  else if (self->base_url && path)
    {
      g_autofree char *url = g_strconcat (self->base_url, path, NULL);
      msg = soup_message_new ("GET", url);
    }

  if (msg == NULL)
    {
      g_autoptr(GError) error = g_error_new_literal (RC_CLIENT_ERROR, RC_CLIENT_ERROR_INVALID_URL,
                                                     "Invalid URL");
      func (self, NULL, NULL, error, user_data);
      return;
    }
  prepare_message (self, msg, own_server);

  r = g_new0 (Request, 1);
  r->client = rc_client_ref (self);
  r->cancellable = g_object_ref (self->cancellable);
  r->msg = msg;
  r->bytes_func = func;
  r->user_data = user_data;

  soup_session_send_and_read_async (self->session, msg, G_PRIORITY_LOW,
                                    r->cancellable, on_bytes_sent, r);
}

/* ------------------------------------------------------------------------ */
/* login with HTTPS(443) -> HTTP(80) fallback                                */

typedef struct {
  RcClient      *client;
  char          *user;
  char          *password;
  char          *code;
  guint          index;
  GString       *errors;
  RcResponseFunc func;
  gpointer       user_data;
} Login;

static void login_try (Login *l);

static void
login_free (Login *l)
{
  rc_client_unref (l->client);
  g_free (l->user);
  if (l->password)
    {
      memset (l->password, 0, strlen (l->password));
      g_free (l->password);
    }
  g_free (l->code);
  g_string_free (l->errors, TRUE);
  g_free (l);
}

static const char *
describe_base (const char *base)
{
  return g_str_has_prefix (base, "https://") ? "HTTPS" : "HTTP";
}

static gboolean
is_final_error (const GError *error)
{
  /* A real Rocket.Chat answer (wrong password, 2FA…) or a certificate the
   * user may want to trust: never silently fall back to plain HTTP. */
  return error->domain == RC_CLIENT_ERROR &&
         (error->code == RC_CLIENT_ERROR_API ||
          error->code == RC_CLIENT_ERROR_AUTH ||
          error->code == RC_CLIENT_ERROR_UNTRUSTED_CERT);
}

static void
on_login_reply (RcClient     *client,
                JsonObject   *result,
                const GError *error,
                gpointer      data)
{
  Login *l = data;
  const char *tried = g_ptr_array_index (client->candidates, l->index);

  if (rc_error_is_cancelled (error))
    {
      l->func (client, NULL, error, l->user_data);
      login_free (l);
      return;
    }

  if (error == NULL)
    {
      JsonObject *d = json_object_get_object_member (result, "data");
      const char *uid = d ? json_object_get_string_member_with_default (d, "userId", NULL) : NULL;
      const char *tok = d ? json_object_get_string_member_with_default (d, "authToken", NULL) : NULL;

      if (uid && tok)
        {
          rc_client_set_credentials (client, uid, tok);
          l->func (client, result, NULL, l->user_data);
        }
      else
        {
          g_autoptr(GError) e = g_error_new_literal (RC_CLIENT_ERROR, RC_CLIENT_ERROR_PARSE,
                                                     "Login reply did not contain a token");
          l->func (client, NULL, e, l->user_data);
        }
      login_free (l);
      return;
    }

  if (is_final_error (error) || l->index + 1 >= client->candidates->len)
    {
      g_autoptr(GError) e = NULL;

      if (l->errors->len > 0 && !is_final_error (error))
        {
          g_string_append_printf (l->errors, "\n%s: %s", describe_base (tried), error->message);
          e = g_error_new (error->domain, error->code, "%s", l->errors->str);
        }
      else
        e = g_error_copy (error);

      l->func (client, NULL, e, l->user_data);
      login_free (l);
      return;
    }

  g_string_append_printf (l->errors, "%s%s: %s",
                          l->errors->len ? "\n" : "", describe_base (tried), error->message);
  l->index++;
  login_try (l);
}

static void
login_try (Login *l)
{
  RcClient *self = l->client;
  g_autoptr(JsonBuilder) b = json_builder_new ();

  g_free (self->base_url);
  self->base_url = g_strdup (g_ptr_array_index (self->candidates, l->index));

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "user");
  json_builder_add_string_value (b, l->user);
  json_builder_set_member_name (b, "password");
  json_builder_add_string_value (b, l->password);
  if (l->code && *l->code)
    {
      json_builder_set_member_name (b, "code");
      json_builder_add_string_value (b, l->code);
    }
  json_builder_end_object (b);

  rc_client_post (self, "/api/v1/login", json_builder_get_root (b), on_login_reply, l);
}

void
rc_client_login (RcClient      *self,
                 const char    *user,
                 const char    *password,
                 const char    *totp_code,
                 RcResponseFunc func,
                 gpointer       user_data)
{
  Login *l = g_new0 (Login, 1);

  g_return_if_fail (self->candidates->len > 0);

  rc_client_set_credentials (self, NULL, NULL);

  l->client = rc_client_ref (self);
  l->user = g_strdup (user);
  l->password = g_strdup (password);
  l->code = g_strdup (totp_code);
  l->errors = g_string_new (NULL);
  l->func = func;
  l->user_data = user_data;
  login_try (l);
}

/* ------------------------------------------------------------------------ */
/* realtime: DDP over WebSocket (ws:// on 80, wss:// on 443)                 */

static void rt_connect (RcClient *self);

static void
rt_emit (RcClient   *self,
         const char *event,
         JsonObject *payload)
{
  if (self->rt_func)
    self->rt_func (self, event, payload, self->rt_data);
}

static void
rt_send (RcClient    *self,
         JsonBuilder *b)
{
  g_autoptr(JsonNode) root = json_builder_get_root (b);
  g_autofree char *text = json_to_string (root, FALSE);

  if (self->ws && soup_websocket_connection_get_state (self->ws) == SOUP_WEBSOCKET_STATE_OPEN)
    soup_websocket_connection_send_text (self->ws, text);
}

static void
rt_send_simple (RcClient   *self,
                const char *msg)
{
  g_autoptr(JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "msg");
  json_builder_add_string_value (b, msg);
  json_builder_end_object (b);
  rt_send (self, b);
}

static char *
rt_subscribe (RcClient   *self,
              const char *name,
              const char *param)
{
  g_autoptr(JsonBuilder) b = json_builder_new ();
  char *id = g_strdup_printf ("sub-%u", ++self->rt_next_id);

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "msg");
  json_builder_add_string_value (b, "sub");
  json_builder_set_member_name (b, "id");
  json_builder_add_string_value (b, id);
  json_builder_set_member_name (b, "name");
  json_builder_add_string_value (b, name);
  json_builder_set_member_name (b, "params");
  json_builder_begin_array (b);
  json_builder_add_string_value (b, param);
  json_builder_add_boolean_value (b, FALSE);
  json_builder_end_array (b);
  json_builder_end_object (b);
  rt_send (self, b);
  return id;
}

static void
rt_unsubscribe (RcClient   *self,
                const char *id)
{
  g_autoptr(JsonBuilder) b = json_builder_new ();

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "msg");
  json_builder_add_string_value (b, "unsub");
  json_builder_set_member_name (b, "id");
  json_builder_add_string_value (b, id);
  json_builder_end_object (b);
  rt_send (self, b);
}

static void
rt_subscribe_room (RcClient *self)
{
  static const char *events[] = { "deleteMessage", "user-activity", "typing", "messagesRead" };

  if (!self->rt_ready || self->rt_room == NULL)
    return;
  for (guint i = 0; i < G_N_ELEMENTS (events); i++)
    {
      g_autofree char *ev = g_strdup_printf ("%s/%s", self->rt_room, events[i]);
      g_ptr_array_add (self->rt_room_subs, rt_subscribe (self, "stream-notify-room", ev));
    }
}

void
rc_client_realtime_watch_room (RcClient   *self,
                               const char *rid)
{
  if (g_strcmp0 (rid, self->rt_room) == 0)
    return;
  for (guint i = 0; i < self->rt_room_subs->len; i++)
    rt_unsubscribe (self, g_ptr_array_index (self->rt_room_subs, i));
  g_ptr_array_set_size (self->rt_room_subs, 0);
  g_free (self->rt_room);
  self->rt_room = g_strdup (rid);
  rt_subscribe_room (self);
}

void
rc_client_realtime_typing (RcClient   *self,
                           const char *rid,
                           const char *username,
                           gboolean    typing)
{
  g_autoptr(JsonBuilder) b = json_builder_new ();
  g_autofree char *ev = g_strdup_printf ("%s/user-activity", rid);
  g_autofree char *id = g_strdup_printf ("typing-%u", ++self->rt_next_id);

  if (!self->rt_ready || !rid || !username)
    return;

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "msg");
  json_builder_add_string_value (b, "method");
  json_builder_set_member_name (b, "method");
  json_builder_add_string_value (b, "stream-notify-room");
  json_builder_set_member_name (b, "id");
  json_builder_add_string_value (b, id);
  json_builder_set_member_name (b, "params");
  json_builder_begin_array (b);
  json_builder_add_string_value (b, ev);
  json_builder_add_string_value (b, username);
  json_builder_begin_array (b);
  if (typing)
    json_builder_add_string_value (b, "user-typing");
  json_builder_end_array (b);
  json_builder_begin_object (b);
  json_builder_end_object (b);
  json_builder_end_array (b);
  json_builder_end_object (b);
  rt_send (self, b);
}

static void
rt_handle_room_event (RcClient   *self,
                      const char *event_name,
                      JsonArray  *args)
{
  const char *slash = strrchr (event_name, '/');
  g_autofree char *rid = NULL;
  g_autoptr(JsonObject) p = json_object_new ();
  const char *kind;
  JsonNode *a0 = json_array_get_length (args) > 0 ? json_array_get_element (args, 0) : NULL;
  JsonNode *a1 = json_array_get_length (args) > 1 ? json_array_get_element (args, 1) : NULL;

  if (slash == NULL)
    return;
  rid = g_strndup (event_name, slash - event_name);
  kind = slash + 1;
  json_object_set_string_member (p, "rid", rid);

  if (g_str_equal (kind, "deleteMessage"))
    {
      if (a0 && JSON_NODE_HOLDS_OBJECT (a0))
        {
          const char *id = json_object_get_string_member_with_default (json_node_get_object (a0), "_id", NULL);
          if (id)
            {
              json_object_set_string_member (p, "_id", id);
              rt_emit (self, "deleted", p);
            }
        }
    }
  else if (g_str_equal (kind, "user-activity") || g_str_equal (kind, "typing"))
    {
      gboolean typing = FALSE;

      if (!a0 || !JSON_NODE_HOLDS_VALUE (a0) || json_node_get_value_type (a0) != G_TYPE_STRING)
        return;
      if (a1 && JSON_NODE_HOLDS_ARRAY (a1))           /* new: [user, ["user-typing"], {}] */
        {
          JsonArray *acts = json_node_get_array (a1);
          for (guint i = 0; i < json_array_get_length (acts); i++)
            if (g_strcmp0 (json_array_get_string_element (acts, i), "user-typing") == 0)
              typing = TRUE;
        }
      else if (a1 && JSON_NODE_HOLDS_VALUE (a1))      /* old: [user, bool] */
        typing = json_node_get_boolean (a1);
      json_object_set_string_member (p, "username", json_node_get_string (a0));
      json_object_set_boolean_member (p, "typing", typing);
      rt_emit (self, "typing", p);
    }
  else if (g_str_equal (kind, "messagesRead"))
    {
      rt_emit (self, "messages-read", p);
    }
}

static void
rt_handle (RcClient   *self,
           JsonObject *obj)
{
  const char *msg = json_object_get_string_member_with_default (obj, "msg", "");

  if (g_str_equal (msg, "ping"))
    {
      rt_send_simple (self, "pong");
    }
  else if (g_str_equal (msg, "connected"))
    {
      g_autoptr(JsonBuilder) b = json_builder_new ();

      json_builder_begin_object (b);
      json_builder_set_member_name (b, "msg");
      json_builder_add_string_value (b, "method");
      json_builder_set_member_name (b, "method");
      json_builder_add_string_value (b, "login");
      json_builder_set_member_name (b, "id");
      json_builder_add_string_value (b, "login");
      json_builder_set_member_name (b, "params");
      json_builder_begin_array (b);
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "resume");
      json_builder_add_string_value (b, self->token ? self->token : "");
      json_builder_end_object (b);
      json_builder_end_array (b);
      json_builder_end_object (b);
      rt_send (self, b);
    }
  else if (g_str_equal (msg, "result") &&
           g_strcmp0 (json_object_get_string_member_with_default (obj, "id", NULL), "login") == 0)
    {
      g_autofree char *s1 = NULL, *s2 = NULL;

      if (json_object_has_member (obj, "error"))
        {
          g_warning ("Realtime login failed");
          return;
        }
      self->rt_ready = TRUE;
      s1 = rt_subscribe (self, "stream-room-messages", "__my_messages__");
      if (self->user_id)
        {
          g_autofree char *ev = g_strconcat (self->user_id, "/subscriptions-changed", NULL);
          s2 = rt_subscribe (self, "stream-notify-user", ev);
        }
      g_ptr_array_set_size (self->rt_room_subs, 0);
      rt_subscribe_room (self);
      rt_emit (self, "connected", NULL);
    }
  else if (g_str_equal (msg, "changed"))
    {
      const char *coll = json_object_get_string_member_with_default (obj, "collection", "");
      JsonObject *fields = json_object_has_member (obj, "fields") ? json_object_get_object_member (obj, "fields") : NULL;
      JsonArray *args = fields && json_object_has_member (fields, "args")
                        ? json_object_get_array_member (fields, "args") : NULL;
      const char *event_name = fields ? json_object_get_string_member_with_default (fields, "eventName", "") : "";

      if (args == NULL)
        return;

      if (g_str_equal (coll, "stream-room-messages"))
        {
          for (guint i = 0; i < json_array_get_length (args); i++)
            {
              JsonNode *n = json_array_get_element (args, i);
              if (JSON_NODE_HOLDS_OBJECT (n))
                rt_emit (self, "message", json_node_get_object (n));
            }
        }
      else if (g_str_equal (coll, "stream-notify-user"))
        {
          JsonObject *sub = NULL;
          if (json_array_get_length (args) > 1 &&
              JSON_NODE_HOLDS_OBJECT (json_array_get_element (args, 1)))
            sub = json_array_get_object_element (args, 1);
          rt_emit (self, "subscriptions-changed", sub);
        }
      else if (g_str_equal (coll, "stream-notify-room"))
        {
          rt_handle_room_event (self, event_name, args);
        }
    }
}

static void
on_ws_message (SoupWebsocketConnection *ws,
               gint                     type,
               GBytes                  *message,
               gpointer                 data)
{
  RcClient *self = data;
  g_autoptr(JsonParser) parser = json_parser_new ();
  gsize len = 0;
  const char *text = g_bytes_get_data (message, &len);
  JsonNode *root;

  if (type != SOUP_WEBSOCKET_DATA_TEXT)
    return;
  if (!json_parser_load_from_data (parser, text, len, NULL))
    return;
  root = json_parser_get_root (parser);
  if (root && JSON_NODE_HOLDS_OBJECT (root))
    rt_handle (self, json_node_get_object (root));
}

static gboolean
rt_reconnect_cb (gpointer data)
{
  RcClient *self = data;

  self->rt_reconnect_id = 0;
  if (self->rt_wanted)
    rt_connect (self);
  return G_SOURCE_REMOVE;
}

static void
rt_schedule_reconnect (RcClient *self)
{
  if (!self->rt_wanted || self->rt_reconnect_id)
    return;
  self->rt_reconnect_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, RECONNECT_SECONDS,
                                                      rt_reconnect_cb,
                                                      rc_client_ref (self),
                                                      (GDestroyNotify) rc_client_unref);
}

static void
on_ws_closed (SoupWebsocketConnection *ws,
              gpointer                 data)
{
  RcClient *self = data;

  if (self->ws != ws)
    return;
  g_signal_handlers_disconnect_by_data (ws, self);
  g_clear_object (&self->ws);
  self->rt_ready = FALSE;
  rt_emit (self, "disconnected", NULL);
  rt_schedule_reconnect (self);
}

static void
on_ws_connected (GObject      *source,
                 GAsyncResult *res,
                 gpointer      data)
{
  RcClient *self = data;
  g_autoptr(GError) error = NULL;
  SoupWebsocketConnection *ws;

  ws = soup_session_websocket_connect_finish (SOUP_SESSION (source), res, &error);

  if (ws == NULL)
    {
      if (!rc_error_is_cancelled (error))
        {
          g_message ("Realtime connection failed: %s", error->message);
          rt_emit (self, "disconnected", NULL);
          rt_schedule_reconnect (self);
        }
      rc_client_unref (self);
      return;
    }

  if (!self->rt_wanted)
    {
      soup_websocket_connection_close (ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
      g_object_unref (ws);
      rc_client_unref (self);
      return;
    }

  self->ws = ws;
  soup_websocket_connection_set_keepalive_interval (ws, 30);
  soup_websocket_connection_set_max_incoming_payload_size (ws, 0);
  g_signal_connect (ws, "message", G_CALLBACK (on_ws_message), self);
  g_signal_connect (ws, "closed", G_CALLBACK (on_ws_closed), self);

  {
    g_autoptr(JsonBuilder) b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "msg");
    json_builder_add_string_value (b, "connect");
    json_builder_set_member_name (b, "version");
    json_builder_add_string_value (b, "1");
    json_builder_set_member_name (b, "support");
    json_builder_begin_array (b);
    json_builder_add_string_value (b, "1");
    json_builder_end_array (b);
    json_builder_end_object (b);
    rt_send (self, b);
  }

  rc_client_unref (self);
}

static void
rt_connect (RcClient *self)
{
  g_autofree char *url = NULL;
  SoupMessage *msg;

  if (self->base_url == NULL || self->ws != NULL)
    return;

  /* https://host[:443] -> wss://host/websocket, http://host[:80] -> ws://host/websocket */
  if (g_str_has_prefix (self->base_url, "https://"))
    url = g_strconcat ("wss://", self->base_url + strlen ("https://"), "/websocket", NULL);
  else
    url = g_strconcat ("ws://", self->base_url + strlen ("http://"), "/websocket", NULL);

  msg = soup_message_new ("GET", url);
  if (msg == NULL)
    return;
  /* same certificate pinning as REST (self-signed servers) */
  g_signal_connect (msg, "accept-certificate", G_CALLBACK (on_accept_certificate), self);

  soup_session_websocket_connect_async (self->session, msg, NULL, NULL,
                                        G_PRIORITY_DEFAULT, self->rt_cancellable,
                                        on_ws_connected, rc_client_ref (self));
  g_object_unref (msg);
}

void
rc_client_realtime_start (RcClient       *self,
                          RcRealtimeFunc  func,
                          gpointer        user_data)
{
  rc_client_realtime_stop (self);
  self->rt_func = func;
  self->rt_data = user_data;
  self->rt_wanted = TRUE;
  self->rt_cancellable = g_cancellable_new ();
  rt_connect (self);
}

void
rc_client_realtime_stop (RcClient *self)
{
  self->rt_wanted = FALSE;
  self->rt_ready = FALSE;
  self->rt_func = NULL;
  self->rt_data = NULL;
  g_clear_handle_id (&self->rt_reconnect_id, g_source_remove);
  if (self->rt_room_subs)
    g_ptr_array_set_size (self->rt_room_subs, 0);
  g_clear_pointer (&self->rt_room, g_free);

  if (self->rt_cancellable)
    {
      g_cancellable_cancel (self->rt_cancellable);
      g_clear_object (&self->rt_cancellable);
    }

  if (self->ws)
    {
      SoupWebsocketConnection *ws = g_steal_pointer (&self->ws);
      g_signal_handlers_disconnect_by_data (ws, self);
      if (soup_websocket_connection_get_state (ws) == SOUP_WEBSOCKET_STATE_OPEN)
        soup_websocket_connection_close (ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
      g_object_unref (ws);
    }
}
