/* rc-client.h - Minimal Rocket.Chat REST + realtime (DDP over WebSocket) client
 *
 * Supports plain HTTP (port 80, ws://) and HTTPS (port 443, wss://), with an
 * "auto" mode that tries HTTPS first and falls back to HTTP.
 */
#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define RC_CLIENT_ERROR (rc_client_error_quark ())

typedef enum {
  RC_CLIENT_ERROR_INVALID_URL,
  RC_CLIENT_ERROR_HTTP,  /* non-2xx with non-JSON body */
  RC_CLIENT_ERROR_PARSE, /* 2xx but body is not Rocket.Chat JSON */
  RC_CLIENT_ERROR_API,   /* Rocket.Chat returned an error object */
  RC_CLIENT_ERROR_AUTH,  /* 401: not logged in / bad token */
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
                             const GError *error,
                             gpointer      user_data);

/* Realtime events:
 *   "connected"             payload NULL
 *   "disconnected"          payload NULL
 *   "message"               payload = message object (new or edited)
 *   "subscriptions-changed" payload = subscription object or NULL
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
/* cancel every pending request and stop realtime */
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

/* POST /api/v1/login – on success credentials are stored on the client and
 * the base URL is fixed to whichever candidate (443/80) worked. */
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
void        rc_client_fetch_bytes      (RcClient    *client,
                                        const char  *path,
                                        RcBytesFunc  func,
                                        gpointer     user_data);

void        rc_client_realtime_start   (RcClient       *client,
                                        RcRealtimeFunc  func,
                                        gpointer        user_data);
void        rc_client_realtime_stop    (RcClient *client);

G_END_DECLS
