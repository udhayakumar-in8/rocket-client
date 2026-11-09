/* Unit tests for server address / port handling (no network needed). */
#include <string.h>
#include "src/rc-client.h"

typedef struct {
  const char *input;
  RcConnMode  mode;
  const char *expected; /* first candidate */
  gboolean    secure;
} Case;

static void
test_server_urls (void)
{
  static const Case cases[] = {
    { "chat.example.com",              RC_CONN_AUTO,  "https://chat.example.com",       TRUE  },
    { "chat.example.com",              RC_CONN_HTTPS, "https://chat.example.com",       TRUE  },
    { "chat.example.com",              RC_CONN_HTTP,  "http://chat.example.com",        FALSE },
    { "  chat.example.com/  ",         RC_CONN_HTTP,  "http://chat.example.com",        FALSE },
    { "http://10.0.0.5",               RC_CONN_AUTO,  "http://10.0.0.5",                FALSE },
    { "http://10.0.0.5:80/",           RC_CONN_HTTPS, "http://10.0.0.5",                FALSE },
    { "https://chat.example.com:443",  RC_CONN_HTTP,  "https://chat.example.com",       TRUE  },
    { "https://host:8443/rocket/",     RC_CONN_AUTO,  "https://host:8443/rocket",       TRUE  },
    { "host:3000",                     RC_CONN_HTTP,  "http://host:3000",               FALSE },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      RcClient *c = rc_client_new ();
      g_autoptr(GError) error = NULL;

      g_assert_true (rc_client_set_server (c, cases[i].input, cases[i].mode, &error));
      g_assert_no_error (error);
      g_assert_cmpstr (rc_client_get_base_url (c), ==, cases[i].expected);
      g_assert_cmpint (rc_client_is_secure (c), ==, cases[i].secure);
      rc_client_unref (c);
    }
}

static void
test_invalid (void)
{
  const char *bad[] = { "", "   ", "ftp://example.com", "https://" };

  for (guint i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      RcClient *c = rc_client_new ();
      g_autoptr(GError) error = NULL;

      g_assert_false (rc_client_set_server (c, bad[i], RC_CONN_AUTO, &error));
      g_assert_error (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_INVALID_URL);
      rc_client_unref (c);
    }
}

typedef struct {
  GMainLoop *loop;
  GError    *error;
} LoginData;

static void
on_login (RcClient *c, JsonObject *r, const GError *e, gpointer data)
{
  LoginData *d = data;
  d->error = e ? g_error_copy (e) : NULL;
  g_main_loop_quit (d->loop);
}

/* Auto mode must try 443 then 80 and report both failures. Uses a local
 * address where nothing listens (127.0.0.1 on unprivileged CI). */
static void
test_auto_fallback_reports_both (void)
{
  RcClient *c = rc_client_new ();
  LoginData d = { g_main_loop_new (NULL, FALSE), NULL };

  g_assert_true (rc_client_set_server (c, "127.0.0.1:1", RC_CONN_AUTO, NULL));
  rc_client_login (c, "u", "p", NULL, on_login, &d);
  g_main_loop_run (d.loop);

  g_assert_nonnull (d.error);
  g_assert_nonnull (strstr (d.error->message, "HTTPS:"));
  g_assert_nonnull (strstr (d.error->message, "HTTP:"));
  /* last candidate tried is the plain-HTTP one */
  g_assert_cmpstr (rc_client_get_base_url (c), ==, "http://127.0.0.1:1");

  g_error_free (d.error);
  g_main_loop_unref (d.loop);
  rc_client_unref (c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/client/server-urls", test_server_urls);
  g_test_add_func ("/client/invalid", test_invalid);
  g_test_add_func ("/client/auto-fallback", test_auto_fallback_reports_both);
  return g_test_run ();
}
