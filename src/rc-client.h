/* rc-client.h - Minimal Rocket.Chat REST + realtime (DDP over WebSocket) client
 *
 * Supports plain HTTP (port 80, ws://) and HTTPS (port 443, wss://), with an
 * "auto" mode that tries HTTPS first and falls back to HTTP.  Self-signed /
 * private-CA certificates can be pinned (trust on first use).
 */
#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define RC_CLIENT_ERROR (rc_client_error_quark ())

typedef enum {
  RC_CLIENT_ERROR_INVALID_URL,
  RC_CLIENT_ERROR_HTTP,           /* non-2xx with non-JSON body */
  RC_CLIENT_ERROR_PARSE,          /* 2xx but body is not Rocket.Chat JSON */
  RC_CLIENT_ERROR_API,            /* Rocket.Chat returned an error object */
  RC_CLIENT_ERROR_AUTH,           /* 401: not logged in / bad token */
  RC_CLIENT_ERROR_NOT_FOUND,      /* 404: endpoint does not exist */
  RC_CLIENT_ERROR_UNTRUSTED_CERT, /* TLS certificate not trusted (yet) */
} RcClientError;

typedef enum {
  RC_CONN_AUTO,  /* try https:// (443) then http:// (80) */
  RC_CONN_HTTPS, /* https:// only, default port 443 */
  RC_CONN_HTTP,  /* http:// only, default port 80 */
} RcConnMode;

typedef struct _RcClient RcClient;

/* @result is borrowed (may be NULL on error). The callback is ALWAYS
 * invoked exactly once; if the client was shut down the error is
 * G_IO_ERROR_CANCELLED and the receiver must not touch freed state. */
typedef void (*RcResponseFunc) (RcClient     *client,
                                JsonObject   *result,
                                const GError *error,
                                gpointer      user_data);

typedef void (*RcBytesFunc) (RcClient     *client,
                             GBytes       *bytes,
                             const char   *content_type,
                             const GError *error,
                             gpointer      user_data);

/* Realtime events:
 *   "connected" / "disconnected"   payload NULL
 *   "message"                      payload = message object (new or edited)
 *   "subscriptions-changed"        payload = subscription object or NULL
 *   "deleted"                      payload = { "_id", "rid" }
 *   "typing"                       payload = { "rid", "username", "typing": bool }
 *   "messages-read"                payload = { "rid" }
 */
typedef void (*RcRealtimeFunc) (RcClient   *client,
                                const char *event,
                                JsonObject *payload,
                                gpointer    user_data);

GQuark      rc_client_error_quark      (void);
gboolean    rc_error_is_cancelled      (const GError *error);

RcClient   *rc_client_new              (void);
RcClient   *rc_client_ref              (RcClient *client);
void        rc_client_unref            (RcClient *client);
void        rc_client_shutdown         (RcClient *client);

gboolean    rc_client_set_server       (RcClient    *client,
                                        const char  *input,
                                        RcConnMode   mode,
                                        GError     **error);
const char *rc_client_get_base_url     (RcClient *client);
gboolean    rc_client_is_secure        (RcClient *client);

void        rc_client_set_credentials  (RcClient   *client,
                                        const char *user_id,
                                        const char *token);
const char *rc_client_get_user_id      (RcClient *client);
const char *rc_client_get_token        (RcClient *client);

/* --- certificate trust ------------------------------------------------- */
/* keyfile where pinned certificate fingerprints are stored */
void             rc_client_set_trust_file          (RcClient   *client,
                                                    const char *path);
/* certificate that caused the last RC_CLIENT_ERROR_UNTRUSTED_CERT */
GTlsCertificate *rc_client_get_rejected_certificate (RcClient             *client,
                                                     const char          **host,
                                                     GTlsCertificateFlags *errors);
/* pin the rejected certificate for its host:port and persist it */
void             rc_client_trust_rejected_certificate (RcClient *client);
char            *rc_cert_fingerprint               (GTlsCertificate *cert);
char            *rc_cert_describe_errors           (GTlsCertificateFlags errors);

/* --- REST ---------------------------------------------------------------- */
void        rc_client_login            (RcClient      *client,
                                        const char    *user,
                                        const char    *password,
                                        const char    *totp_code,
                                        RcResponseFunc func,
                                        gpointer       user_data);
void        rc_client_get              (RcClient      *client,
                                        const char    *path,
                                        RcResponseFunc func,
                                        gpointer       user_data);
/* takes ownership of @body (may be NULL) */
void        rc_client_post             (RcClient      *client,
                                        const char    *path,
                                        JsonNode      *body,
                                        RcResponseFunc func,
                                        gpointer       user_data);
/* multipart/form-data upload; @fields is a NULL-terminated key,value list */
void        rc_client_upload           (RcClient           *client,
                                        const char         *path,
                                        const char         *filename,
                                        const char         *content_type,
                                        GBytes             *data,
                                        const char * const *fields,
                                        RcResponseFunc      func,
                                        gpointer            user_data);
/* @path may be server-relative ("/file-upload/…") or an absolute URL;
 * auth headers are only sent to the Rocket.Chat server itself. */
void        rc_client_fetch_bytes      (RcClient    *client,
                                        const char  *path,
                                        RcBytesFunc  func,
                                        gpointer     user_data);

/* --- realtime ------------------------------------------------------------ */
void        rc_client_realtime_start   (RcClient       *client,
                                        RcRealtimeFunc  func,
                                        gpointer        user_data);
void        rc_client_realtime_stop    (RcClient *client);
/* follow per-room events (deletes, typing, read) for @rid (NULL = none) */
void        rc_client_realtime_watch_room (RcClient   *client,
                                           const char *rid);
void        rc_client_realtime_typing  (RcClient   *client,
                                        const char *rid,
                                        const char *username,
                                        gboolean    typing);

G_END_DECLS
