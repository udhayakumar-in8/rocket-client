/* rc-window.c - Rocket Client main window (GTK 4 + libadwaita) */

#include "config.h"
#include "rc-window.h"
#include "rc-client.h"
#include "rc-emoji.h"
#include "rc-preview.h"
#include "rc-planner.h"

#include <string.h>
#include <glib/gstdio.h>

#define GROUP_WINDOW_MS     (5 * 60 * 1000)
#define HISTORY_COUNT       50
#define INLINE_SNIPPET_MAX  (512 * 1024)
#define SNIPPET_LINES       14
#define TYPING_TIMEOUT_US   (6 * G_USEC_PER_SEC)

enum { SECTION_FAVORITES, SECTION_PEOPLE, SECTION_ROOMS };

typedef struct _Win Win;

typedef struct {
  char      *rid;
  char      *name;
  char      *fname;
  char      *type;    /* "c" channel, "p" private group, "d" direct */
  int        unread;
  gboolean   favorite;
  GtkWidget *row;
  GtkWidget *badge;
  GtkWidget *label;
} Room;

/* one rendered message list (main timeline or thread panel) */
typedef struct {
  Win        *w;
  GtkWidget  *box;
  GHashTable *views;   /* id -> MsgView* */
  GPtrArray  *order;   /* JsonObject* (ref), chronological */
  char       *last_user;
  gint64      last_ts;
  gboolean    is_thread;
} MsgList;

typedef struct {
  MsgList    *list;
  char       *id;
  JsonObject *msg;
  GtkWidget  *outer;
  GtkWidget  *unit;
  GtkWidget  *body;
  GtkWidget  *meta;
  GtkWidget  *attach_box;
  GtkWidget  *reactions_box;
  GtkWidget  *thread_btn;
  GtkWidget  *more_btn;
  guint       n_attachments;
} MsgView;

struct _Win {
  GtkWindow            *window;
  RcClient             *client;
  RcPlanner            *planner;
  AdwToastOverlay      *toasts;
  GtkStack             *stack;

  /* login page */
  AdwEntryRow          *server_row;
  AdwComboRow          *mode_row;
  AdwEntryRow          *user_row;
  AdwPasswordEntryRow  *pass_row;
  AdwEntryRow          *code_row;
  GtkWidget            *login_button;
  GtkWidget            *login_spinner;

  /* main page */
  AdwNavigationSplitView *split;
  AdwOverlaySplitView  *side_split;
  GtkStack             *side_stack;
  GtkWidget            *preview;
  AdwNavigationPage    *content_page;
  AdwWindowTitle       *sidebar_title;
  GtkListBox           *room_list;
  GtkSearchBar         *search_bar;
  GtkEditable          *search_entry;
  AdwWindowTitle       *room_title;
  AdwBanner            *banner;
  GtkStack             *content_stack;
  GtkScrolledWindow    *msg_scroller;
  GtkWidget            *older_button;
  GtkWidget            *jump_button;
  GtkWidget            *typing_label;
  GtkRevealer          *edit_bar;
  GtkEntry             *composer;
  GtkWidget            *send_button;

  /* thread panel */
  AdwWindowTitle       *thread_title;
  GtkScrolledWindow    *thread_scroller;
  GtkEntry             *thread_composer;
  char                 *thread_root;

  MsgList              *main_list;
  MsgList              *thread_list;

  /* state */
  GHashTable           *rooms;        /* rid -> Room* */
  char                 *current_rid;
  gboolean              stick_bottom;
  double                last_upper;
  double                last_page;
  double                restore_from_bottom; /* >= 0: keep this distance after prepend */
  char                 *me_username;
  GHashTable           *avatar_cache;   /* key -> GdkTexture* or NULL */
  GHashTable           *avatar_waiting; /* key -> GPtrArray<AdwAvatar> */
  guint                 subs_reload_id;
  gboolean              rebuilding;
  char                 *editing_id;
  GHashTable           *typing;         /* username -> gint64 expiry (monotonic) */
  guint                 typing_tick_id;
  gint64                typing_sent_at;
  guint                 typing_stop_id;
  gboolean              background_notice_sent;
};

static void load_subscriptions (Win *w);
static void show_login (Win *w);
static void try_resume (Win *w);
static void do_login (Win *w);
static void open_thread (Win *w, const char *root_id);
static void open_preview_url (Win *w, const char *url, const char *name, const char *mime);
static void update_view (MsgView *v);
static void msg_list_rerender (MsgList *l);

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */

static void
room_free (Room *r)
{
  g_free (r->rid);
  g_free (r->name);
  g_free (r->fname);
  g_free (r->type);
  g_free (r);
}

static const char *
room_display_name (Room *r)
{
  return (r->fname && *r->fname) ? r->fname : (r->name ? r->name : r->rid);
}

static int
room_section (Room *r)
{
  if (r->favorite)
    return SECTION_FAVORITES;
  if (g_strcmp0 (r->type, "d") == 0)
    return SECTION_PEOPLE;
  return SECTION_ROOMS;
}

static const char *
history_endpoint (const char *type)
{
  if (g_strcmp0 (type, "d") == 0)
    return "im";
  if (g_strcmp0 (type, "p") == 0)
    return "groups";
  return "channels";
}

static void toast (Win *w, const char *fmt, ...) G_GNUC_PRINTF (2, 3);

static void
toast (Win        *w,
       const char *fmt,
       ...)
{
  va_list ap;
  g_autofree char *text = NULL;
  AdwToast *t;

  va_start (ap, fmt);
  text = g_strdup_vprintf (fmt, ap);
  va_end (ap);

  t = adw_toast_new (text);
  adw_toast_set_timeout (t, 4);
  adw_toast_overlay_add_toast (w->toasts, t);
}

static const char *
jstr (JsonObject *o,
      const char *member)
{
  JsonNode *n;

  if (o == NULL || !json_object_has_member (o, member))
    return NULL;
  n = json_object_get_member (o, member);
  if (!JSON_NODE_HOLDS_VALUE (n) || json_node_get_value_type (n) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string (n);
}

static JsonObject *
jobj (JsonObject *o,
      const char *member)
{
  JsonNode *n;

  if (o == NULL || !json_object_has_member (o, member))
    return NULL;
  n = json_object_get_member (o, member);
  return JSON_NODE_HOLDS_OBJECT (n) ? json_node_get_object (n) : NULL;
}

static JsonArray *
jarr (JsonObject *o,
      const char *member)
{
  JsonNode *n;

  if (o == NULL || !json_object_has_member (o, member))
    return NULL;
  n = json_object_get_member (o, member);
  return JSON_NODE_HOLDS_ARRAY (n) ? json_node_get_array (n) : NULL;
}

static gint64
jint (JsonObject *o,
      const char *member)
{
  JsonNode *n;

  if (o == NULL || !json_object_has_member (o, member))
    return 0;
  n = json_object_get_member (o, member);
  if (!JSON_NODE_HOLDS_VALUE (n))
    return 0;
  if (json_node_get_value_type (n) == G_TYPE_INT64)
    return json_node_get_int (n);
  if (json_node_get_value_type (n) == G_TYPE_DOUBLE)
    return (gint64) json_node_get_double (n);
  return 0;
}

static gint64
parse_ts_ms (JsonObject *m,
             const char *member)
{
  JsonNode *n;

  if (m == NULL || !json_object_has_member (m, member))
    return 0;
  n = json_object_get_member (m, member);

  if (JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING)
    {
      g_autoptr(GDateTime) dt = g_date_time_new_from_iso8601 (json_node_get_string (n), NULL);
      if (dt)
        return g_date_time_to_unix (dt) * 1000 + g_date_time_get_microsecond (dt) / 1000;
    }
  else if (JSON_NODE_HOLDS_OBJECT (n))
    return jint (json_node_get_object (n), "$date");
  return 0;
}

static char *
ms_to_iso (gint64 ms)
{
  g_autoptr(GDateTime) dt = g_date_time_new_from_unix_utc (ms / 1000);
  g_autofree char *base = g_date_time_format (dt, "%Y-%m-%dT%H:%M:%S");
  return g_strdup_printf ("%s.%03dZ", base, (int) (ms % 1000));
}

static char *
format_time (gint64 ms)
{
  g_autoptr(GDateTime) dt = NULL;
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  gboolean today;

  if (ms <= 0)
    return g_strdup ("");

  dt = g_date_time_new_from_unix_local (ms / 1000);
  today = g_date_time_get_year (dt) == g_date_time_get_year (now) &&
          g_date_time_get_day_of_year (dt) == g_date_time_get_day_of_year (now);
  return g_date_time_format (dt, today ? "%-I:%M %p" : "%b %-d, %-I:%M %p");
}

static gboolean
is_me (Win        *w,
       const char *username)
{
  return username && w->me_username && g_str_equal (username, w->me_username);
}

static JsonObject *
msg_user (JsonObject *m)
{
  return jobj (m, "u");
}

/* ------------------------------------------------------------------------ */
/* Rocket.Chat markdown -> Pango markup                                      */

static char *
format_inline (const char *text)
{
  static GRegex *re_code, *re_bold, *re_ital, *re_strike, *re_mention;
  g_autofree char *s0 = g_markup_escape_text (text, -1);
  g_autofree char *s1 = NULL, *s2 = NULL, *s3 = NULL, *s4 = NULL;

  if (g_once_init_enter_pointer (&re_code))
    {
      re_bold = g_regex_new ("(?<![\\w*])\\*([^*\\n]+)\\*(?![\\w*])", G_REGEX_OPTIMIZE, 0, NULL);
      re_ital = g_regex_new ("(?<![\\w_])_([^_\\n]+)_(?![\\w_])", G_REGEX_OPTIMIZE, 0, NULL);
      re_strike = g_regex_new ("(?<![\\w~])~([^~\\n]+)~(?![\\w~])", G_REGEX_OPTIMIZE, 0, NULL);
      re_mention = g_regex_new ("(?<![\\w@])@([\\w.\\-]+)", G_REGEX_OPTIMIZE, 0, NULL);
      g_once_init_leave_pointer (&re_code, g_regex_new ("`([^`\\n]+)`", G_REGEX_OPTIMIZE, 0, NULL));
    }

  s1 = g_regex_replace (re_code, s0, -1, 0, "<tt>\\1</tt>", 0, NULL);
  s2 = g_regex_replace (re_bold, s1, -1, 0, "<b>\\1</b>", 0, NULL);
  s3 = g_regex_replace (re_ital, s2, -1, 0, "<i>\\1</i>", 0, NULL);
  s4 = g_regex_replace (re_strike, s3, -1, 0, "<s>\\1</s>", 0, NULL);
  return g_regex_replace (re_mention, s4, -1, 0, "<b>@\\1</b>", 0, NULL);
}

/* @out gets the markup, @check the same markup minus <a> (Pango can't parse it) */
static void
format_segment (const char *text,
                GString    *out,
                GString    *check)
{
  static GRegex *re_url;
  g_autoptr(GMatchInfo) mi = NULL;
  gssize pos = 0;

  if (g_once_init_enter_pointer (&re_url))
    g_once_init_leave_pointer (&re_url, g_regex_new ("https?://[^\\s<>\"']+[^\\s<>\"'.,;:!?)\\]]",
                                                     G_REGEX_OPTIMIZE, 0, NULL));

  g_regex_match (re_url, text, 0, &mi);
  while (g_match_info_matches (mi))
    {
      int start, end;
      g_autofree char *url = g_match_info_fetch (mi, 0);
      g_autofree char *pre = NULL, *pre_fmt = NULL, *esc = NULL;

      g_match_info_fetch_pos (mi, 0, &start, &end);
      pre = g_strndup (text + pos, start - pos);
      pre_fmt = format_inline (pre);
      esc = g_markup_escape_text (url, -1);
      g_string_append (out, pre_fmt);
      g_string_append_printf (out, "<a href=\"%s\">%s</a>", esc, esc);
      g_string_append (check, pre_fmt);
      g_string_append (check, esc);
      pos = end;
      g_match_info_next (mi, NULL);
    }

  {
    g_autofree char *rest = format_inline (text + pos);
    g_string_append (out, rest);
    g_string_append (check, rest);
  }
}

static char *
format_markup (const char *raw)
{
  g_autofree char *text = rc_emoji_replace_shortcodes (raw);
  g_auto(GStrv) parts = g_strsplit (text, "```", -1);
  GString *out = g_string_new (NULL);
  GString *check = g_string_new (NULL);

  for (guint i = 0; parts[i]; i++)
    {
      /* odd parts are ```code blocks``` (only if closed) */
      if (i % 2 == 1 && parts[i + 1] != NULL)
        {
          g_autofree char *esc = g_markup_escape_text (g_strstrip (parts[i]), -1);
          g_string_append_printf (out, "<tt>%s</tt>", esc);
          g_string_append_printf (check, "<tt>%s</tt>", esc);
        }
      else
        {
          if (i % 2 == 1)
            {
              g_string_append (out, "```");
              g_string_append (check, "```");
            }
          format_segment (parts[i], out, check);
        }
    }

  if (!pango_parse_markup (check->str, -1, 0, NULL, NULL, NULL, NULL))
    {
      g_string_free (check, TRUE);
      g_string_free (out, TRUE);
      return g_markup_escape_text (text, -1);
    }
  g_string_free (check, TRUE);
  return g_string_free (out, FALSE);
}

/* ------------------------------------------------------------------------ */
/* persistence (~/.config/rocket-client/, files are mode 0600)              */

static char *
config_path (const char *name)
{
  return g_build_filename (g_get_user_config_dir (), "rocket-client", name, NULL);
}

static GKeyFile *
session_load (void)
{
  g_autofree char *path = config_path ("session.ini");
  GKeyFile *kf = g_key_file_new ();
  g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL);
  return kf;
}

static void
session_save (GKeyFile *kf)
{
  g_autofree char *path = config_path ("session.ini");
  g_autofree char *dir = g_path_get_dirname (path);
  g_autofree char *data = NULL;
  gsize len = 0;
  g_autoptr(GError) error = NULL;

  g_mkdir_with_parents (dir, 0700);
  data = g_key_file_to_data (kf, &len, NULL);
  if (!g_file_set_contents_full (path, data, len, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error))
    g_warning ("Could not save session: %s", error->message);
}

static void
session_store (Win        *w,
               const char *server_input,
               int         mode,
               const char *username)
{
  g_autoptr(GKeyFile) kf = session_load ();

  if (server_input)
    g_key_file_set_string (kf, "login", "server", server_input);
  if (mode >= 0)
    g_key_file_set_integer (kf, "login", "mode", mode);
  if (username)
    g_key_file_set_string (kf, "login", "username", username);

  g_key_file_set_string (kf, "session", "base_url", rc_client_get_base_url (w->client));
  g_key_file_set_string (kf, "session", "user_id", rc_client_get_user_id (w->client));
  g_key_file_set_string (kf, "session", "token", rc_client_get_token (w->client));
  session_save (kf);
}

static void
session_forget (void)
{
  g_autoptr(GKeyFile) kf = session_load ();
  g_key_file_remove_group (kf, "session", NULL);
  session_save (kf);
}

static gboolean
pref_get_bool (const char *key,
               gboolean    def)
{
  g_autoptr(GKeyFile) kf = session_load ();
  g_autoptr(GError) error = NULL;
  gboolean v = g_key_file_get_boolean (kf, "prefs", key, &error);
  return error ? def : v;
}

static void
pref_set_bool (const char *key,
               gboolean    value)
{
  g_autoptr(GKeyFile) kf = session_load ();
  g_key_file_set_boolean (kf, "prefs", key, value);
  session_save (kf);
}

/* ------------------------------------------------------------------------ */
/* avatars                                                                   */

typedef struct {
  Win  *w;
  char *key;
} AvatarReq;

static void
on_avatar_bytes (RcClient     *client,
                 GBytes       *bytes,
                 const char   *ctype,
                 const GError *error,
                 gpointer      data)
{
  AvatarReq *req = data;
  Win *w = req->w;
  g_autoptr(GdkTexture) tex = NULL;
  GPtrArray *waiting;

  if (rc_error_is_cancelled (error))
    goto out;

  if (bytes != NULL && g_bytes_get_size (bytes) > 0)
    tex = gdk_texture_new_from_bytes (bytes, NULL);

  g_hash_table_insert (w->avatar_cache, g_strdup (req->key), tex ? g_object_ref (tex) : NULL);

  waiting = g_hash_table_lookup (w->avatar_waiting, req->key);
  if (waiting && tex)
    for (guint i = 0; i < waiting->len; i++)
      adw_avatar_set_custom_image (g_ptr_array_index (waiting, i), GDK_PAINTABLE (tex));
  g_hash_table_remove (w->avatar_waiting, req->key);

out:
  g_free (req->key);
  g_free (req);
}

/* @key is the path after /avatar/, e.g. "jdoe" or "room/<rid>" */
static GtkWidget *
make_avatar (Win        *w,
             const char *key,
             const char *display,
             int         size)
{
  GtkWidget *av = adw_avatar_new (size, display, TRUE);
  GPtrArray *waiting;

  if (key == NULL || *key == '\0')
    return av;

  if (g_hash_table_contains (w->avatar_cache, key))
    {
      GdkPaintable *p = g_hash_table_lookup (w->avatar_cache, key);
      if (p)
        adw_avatar_set_custom_image (ADW_AVATAR (av), p);
      return av;
    }

  waiting = g_hash_table_lookup (w->avatar_waiting, key);
  if (waiting == NULL)
    {
      AvatarReq *req = g_new0 (AvatarReq, 1);
      g_autofree char *path = g_strconcat ("/avatar/", key, NULL);

      waiting = g_ptr_array_new_with_free_func (g_object_unref);
      g_hash_table_insert (w->avatar_waiting, g_strdup (key), waiting);
      req->w = w;
      req->key = g_strdup (key);
      rc_client_fetch_bytes (w->client, path, on_avatar_bytes, req);
    }
  g_ptr_array_add (waiting, g_object_ref (av));
  return av;
}

static char *
user_avatar_key (const char *username)
{
  return username ? g_uri_escape_string (username, NULL, FALSE) : NULL;
}

static char *
room_avatar_key (Room *r)
{
  g_autofree char *esc = NULL;

  if (g_strcmp0 (r->type, "d") == 0 && r->name && !strchr (r->name, ','))
    return user_avatar_key (r->name);
  esc = g_uri_escape_string (r->rid, NULL, FALSE);
  return g_strconcat ("room/", esc, NULL);
}

/* ------------------------------------------------------------------------ */
/* room list                                                                 */

static void
room_update_badge (Room *r)
{
  g_autofree char *txt = g_strdup_printf ("%d", r->unread);

  if (r->badge == NULL)
    return;
  gtk_label_set_text (GTK_LABEL (r->badge), txt);
  gtk_widget_set_visible (r->badge, r->unread > 0);
  if (r->unread > 0)
    gtk_widget_add_css_class (r->label, "heading");
  else
    gtk_widget_remove_css_class (r->label, "heading");
}

static GtkWidget *
build_room_row (Win  *w,
                Room *r)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  g_autofree char *key = room_avatar_key (r);

  gtk_widget_set_margin_top (box, 6);
  gtk_widget_set_margin_bottom (box, 6);
  gtk_widget_set_margin_start (box, 6);
  gtk_widget_set_margin_end (box, 6);

  gtk_box_append (GTK_BOX (box), make_avatar (w, key, room_display_name (r), 32));

  r->label = gtk_label_new (room_display_name (r));
  gtk_label_set_xalign (GTK_LABEL (r->label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (r->label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (r->label, TRUE);
  gtk_box_append (GTK_BOX (box), r->label);

  r->badge = gtk_label_new (NULL);
  gtk_widget_add_css_class (r->badge, "unread-badge");
  gtk_widget_set_valign (r->badge, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (box), r->badge);

  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  g_object_set_data (G_OBJECT (row), "room", r);
  r->row = row;
  room_update_badge (r);
  return row;
}

static int
room_sort (GtkListBoxRow *a,
           GtkListBoxRow *b,
           gpointer       data)
{
  Room *ra = g_object_get_data (G_OBJECT (a), "room");
  Room *rb = g_object_get_data (G_OBJECT (b), "room");
  int sa = room_section (ra), sb = room_section (rb);
  g_autofree char *ka = NULL, *kb = NULL;

  if (sa != sb)
    return sa - sb;
  ka = g_utf8_collate_key (room_display_name (ra), -1);
  kb = g_utf8_collate_key (room_display_name (rb), -1);
  return strcmp (ka, kb);
}

static void
room_header (GtkListBoxRow *row,
             GtkListBoxRow *before,
             gpointer       data)
{
  static const char *titles[] = { "Favorites", "People", "Rooms" };
  Room *r = g_object_get_data (G_OBJECT (row), "room");
  int sec = room_section (r);
  GtkWidget *label;

  if (before && room_section (g_object_get_data (G_OBJECT (before), "room")) == sec)
    {
      gtk_list_box_row_set_header (row, NULL);
      return;
    }

  label = gtk_label_new (titles[sec]);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_add_css_class (label, "room-section");
  gtk_widget_add_css_class (label, "caption-heading");
  gtk_widget_add_css_class (label, "dim-label");
  gtk_list_box_row_set_header (row, label);
}

static gboolean
room_filter (GtkListBoxRow *row,
             gpointer       data)
{
  Win *w = data;
  Room *r = g_object_get_data (G_OBJECT (row), "room");
  const char *q = gtk_editable_get_text (w->search_entry);
  g_autofree char *nq = NULL, *nn = NULL;

  if (q == NULL || *q == '\0')
    return TRUE;
  nq = g_utf8_casefold (q, -1);
  nn = g_utf8_casefold (room_display_name (r), -1);
  return strstr (nn, nq) != NULL;
}

static void
on_search_changed (GtkEditable *e,
                   Win         *w)
{
  gtk_list_box_invalidate_filter (w->room_list);
}

static void
ignore_reply (RcClient     *client,
              JsonObject   *result,
              const GError *error,
              gpointer      data)
{
}

/* simple fire-and-forget POST with a toast on failure */
typedef struct {
  Win  *w;
  char *what;
} ActReq;

static void
on_action_reply (RcClient     *client,
                 JsonObject   *result,
                 const GError *error,
                 gpointer      data)
{
  ActReq *q = data;

  if (!rc_error_is_cancelled (error) && error)
    toast (q->w, "%s failed: %s", q->what, error->message);
  g_free (q->what);
  g_free (q);
}

static void
post_action (Win        *w,
             const char *path,
             const char *what,
             const char *k1, const char *v1,
             const char *k2, const char *v2,
             const char *k3, const char *v3)
{
  g_autoptr(JsonBuilder) b = json_builder_new ();
  ActReq *q = g_new0 (ActReq, 1);

  json_builder_begin_object (b);
  if (k1) { json_builder_set_member_name (b, k1); json_builder_add_string_value (b, v1); }
  if (k2) { json_builder_set_member_name (b, k2); json_builder_add_string_value (b, v2); }
  if (k3) { json_builder_set_member_name (b, k3); json_builder_add_string_value (b, v3); }
  json_builder_end_object (b);
  q->w = w;
  q->what = g_strdup (what);
  rc_client_post (w->client, path, json_builder_get_root (b), on_action_reply, q);
}

/* ------------------------------------------------------------------------ */
/* message lists                                                             */

static void
msg_view_free (MsgView *v)
{
  g_free (v->id);
  if (v->msg)
    json_object_unref (v->msg);
  g_free (v);
}

static MsgList *
msg_list_new (Win      *w,
              gboolean  is_thread)
{
  MsgList *l = g_new0 (MsgList, 1);

  l->w = w;
  l->is_thread = is_thread;
  l->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (l->box, "message-list");
  l->views = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) msg_view_free);
  l->order = g_ptr_array_new_with_free_func ((GDestroyNotify) json_object_unref);
  return l;
}

static void
msg_list_free (MsgList *l)
{
  g_hash_table_unref (l->views);
  g_ptr_array_unref (l->order);
  g_free (l->last_user);
  g_free (l);
}

static void
msg_list_clear_widgets (MsgList *l)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (l->box)) != NULL)
    gtk_box_remove (GTK_BOX (l->box), child);
  g_hash_table_remove_all (l->views);
  g_clear_pointer (&l->last_user, g_free);
  l->last_ts = 0;
}

static void
msg_list_clear (MsgList *l)
{
  msg_list_clear_widgets (l);
  g_ptr_array_set_size (l->order, 0);
}

static char *
message_text (JsonObject *m)
{
  const char *msg = jstr (m, "msg");
  return g_strdup (msg ? msg : "");
}

static char *
message_plain_summary (JsonObject *m)
{
  GString *s = g_string_new (jstr (m, "msg"));
  JsonArray *atts = jarr (m, "attachments");

  for (guint i = 0; atts && i < json_array_get_length (atts); i++)
    {
      JsonObject *a = json_array_get_object_element (atts, i);
      const char *t = jstr (a, "title");
      if (t)
        g_string_append_printf (s, "%s📎 %s", s->len ? "\n" : "", t);
    }
  return g_string_free (s, FALSE);
}

static const char *
system_action (const char *t)
{
  static const struct { const char *t, *text; } map[] = {
    { "uj", "joined the room" },
    { "ujt", "joined the team" },
    { "ul", "left the room" },
    { "ult", "left the team" },
    { "au", "added a user" },
    { "ru", "removed a user" },
    { "r", "renamed the room" },
    { "room_changed_topic", "changed the topic" },
    { "room_changed_description", "changed the description" },
    { "room_changed_announcement", "changed the announcement" },
    { "room_changed_avatar", "changed the room avatar" },
    { "user-muted", "muted a user" },
    { "user-unmuted", "unmuted a user" },
    { "message_pinned", "pinned a message" },
    { "rm", "removed a message" },
    { "discussion-created", "started a discussion" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (map); i++)
    if (g_str_equal (map[i].t, t))
      return map[i].text;
  return NULL;
}

/* --- attachments --------------------------------------------------------- */

typedef struct {
  Win       *w;
  GtkWidget *target;  /* weak: GtkPicture or snippet GtkTextView */
  gboolean   diff;
} MediaReq;

static void
media_req_free (MediaReq *q)
{
  g_clear_weak_pointer (&q->target);
  g_free (q);
}

static void
on_thumb_bytes (RcClient *c, GBytes *bytes, const char *ctype, const GError *error, gpointer data)
{
  MediaReq *q = data;
  g_autoptr(GdkTexture) tex = NULL;

  if (!rc_error_is_cancelled (error) && !error && q->target && bytes)
    {
      tex = gdk_texture_new_from_bytes (bytes, NULL);
      if (tex)
        {
          int tw = gdk_texture_get_width (tex), th = gdk_texture_get_height (tex);
          int h = MIN (220, th), wdt = th > 0 ? (int) ((double) tw * h / th) : 220;
          gtk_picture_set_paintable (GTK_PICTURE (q->target), GDK_PAINTABLE (tex));
          gtk_widget_set_size_request (q->target, MIN (wdt, 420), h);
        }
    }
  media_req_free (q);
}

static void
fill_snippet (GtkTextView *tv, const char *data, gsize len, gboolean diff)
{
  GtkTextBuffer *buf = gtk_text_view_get_buffer (tv);
  g_autofree char *valid = g_utf8_make_valid (data, MIN (len, 64 * 1024));
  g_auto(GStrv) lines = g_strsplit (valid, "\n", SNIPPET_LINES + 1);
  GtkTextIter end;

  if (gtk_text_tag_table_lookup (gtk_text_buffer_get_tag_table (buf), "add") == NULL)
    {
      gtk_text_buffer_create_tag (buf, "add", "paragraph-background", "rgba(46,194,126,0.22)", NULL);
      gtk_text_buffer_create_tag (buf, "del", "paragraph-background", "rgba(224,27,36,0.20)", NULL);
      gtk_text_buffer_create_tag (buf, "hunk", "foreground", "#3584e4", NULL);
      gtk_text_buffer_create_tag (buf, "more", "foreground", "gray", "style", PANGO_STYLE_ITALIC, NULL);
    }
  gtk_text_buffer_set_text (buf, "", 0);
  for (guint i = 0; lines[i] && i < SNIPPET_LINES; i++)
    {
      const char *l = lines[i];
      const char *tag = NULL;

      if (diff)
        {
          if (g_str_has_prefix (l, "@@"))
            tag = "hunk";
          else if (l[0] == '+' && !g_str_has_prefix (l, "+++"))
            tag = "add";
          else if (l[0] == '-' && !g_str_has_prefix (l, "---"))
            tag = "del";
        }
      gtk_text_buffer_get_end_iter (buf, &end);
      if (i > 0)
        gtk_text_buffer_insert (buf, &end, "\n", 1);
      gtk_text_buffer_get_end_iter (buf, &end);
      if (tag)
        gtk_text_buffer_insert_with_tags_by_name (buf, &end, l, -1, tag, NULL);
      else
        gtk_text_buffer_insert (buf, &end, l, -1);
    }
  if (g_strv_length (lines) > SNIPPET_LINES)
    {
      gtk_text_buffer_get_end_iter (buf, &end);
      gtk_text_buffer_insert_with_tags_by_name (buf, &end, "\n…", -1, "more", NULL);
    }
}

static void
on_snippet_bytes (RcClient *c, GBytes *bytes, const char *ctype, const GError *error, gpointer data)
{
  MediaReq *q = data;

  if (!rc_error_is_cancelled (error) && q->target)
    {
      if (error || !bytes)
        gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (q->target)),
                                  "(could not load preview)", -1);
      else
        {
          gsize len = 0;
          const char *d = g_bytes_get_data (bytes, &len);
          fill_snippet (GTK_TEXT_VIEW (q->target), d, len, q->diff);
        }
    }
  media_req_free (q);
}

static void
on_open_attachment (GtkButton *b,
                    Win       *w)
{
  open_preview_url (w,
                    g_object_get_data (G_OBJECT (b), "url"),
                    g_object_get_data (G_OBJECT (b), "name"),
                    g_object_get_data (G_OBJECT (b), "mime"));
}

static void
set_attachment_data (GtkWidget  *b,
                     const char *url,
                     const char *name,
                     const char *mime)
{
  g_object_set_data_full (G_OBJECT (b), "url", g_strdup (url), g_free);
  g_object_set_data_full (G_OBJECT (b), "name", g_strdup (name), g_free);
  g_object_set_data_full (G_OBJECT (b), "mime", g_strdup (mime), g_free);
}

static JsonObject *
file_info_for (JsonObject *m,
               guint       index,
               const char *name)
{
  JsonArray *files = jarr (m, "files");
  JsonObject *f = jobj (m, "file");

  if (files)
    {
      for (guint i = 0; i < json_array_get_length (files); i++)
        {
          JsonObject *fi = json_array_get_object_element (files, i);
          if (name && g_strcmp0 (jstr (fi, "name"), name) == 0)
            return fi;
        }
      if (index < json_array_get_length (files))
        return json_array_get_object_element (files, index);
    }
  return f;
}

static GtkWidget *
build_attachment (Win        *w,
                  JsonObject *m,
                  JsonObject *a,
                  guint       index)
{
  const char *title = jstr (a, "title");
  const char *link = jstr (a, "title_link");
  const char *image = jstr (a, "image_url");
  const char *video = jstr (a, "video_url");
  const char *audio = jstr (a, "audio_url");
  const char *desc = jstr (a, "description");
  JsonObject *fi = file_info_for (m, index, title);
  const char *mime = jstr (a, "image_type");
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  RcFileKind kind;
  gint64 size;

  if (!mime) mime = jstr (a, "video_type");
  if (!mime) mime = jstr (a, "audio_type");
  if (!mime) mime = jstr (fi, "type");
  size = jint (a, "image_size");
  if (!size) size = jint (fi, "size");
  if (!size) size = jint (a, "size");

  /* quoted message / plain text attachment */
  if (!image && !video && !audio && !link)
    {
      const char *author = jstr (a, "author_name");
      const char *text = jstr (a, "text");
      GtkWidget *q;

      if (!author && !text && !title)
        return NULL;
      gtk_widget_add_css_class (box, "quote");
      if (author || title)
        {
          GtkWidget *h = gtk_label_new (author ? author : title);
          gtk_label_set_xalign (GTK_LABEL (h), 0);
          gtk_widget_add_css_class (h, "heading");
          gtk_box_append (GTK_BOX (box), h);
        }
      if (text)
        {
          g_autofree char *mk = format_markup (text);
          q = gtk_label_new (NULL);
          gtk_label_set_markup (GTK_LABEL (q), mk);
          gtk_label_set_wrap (GTK_LABEL (q), TRUE);
          gtk_label_set_xalign (GTK_LABEL (q), 0);
          gtk_box_append (GTK_BOX (box), q);
        }
      return box;
    }

  kind = rc_file_kind (title, mime, NULL);

  if (image)
    {
      GtkWidget *btn = gtk_button_new ();
      GtkWidget *pic = gtk_picture_new ();
      MediaReq *q = g_new0 (MediaReq, 1);

      gtk_picture_set_content_fit (GTK_PICTURE (pic), GTK_CONTENT_FIT_COVER);
      gtk_widget_set_size_request (pic, 220, 160);
      gtk_widget_add_css_class (btn, "flat");
      gtk_widget_add_css_class (btn, "image-attachment");
      gtk_widget_set_halign (btn, GTK_ALIGN_START);
      gtk_widget_set_tooltip_text (btn, title ? title : "Image");
      gtk_button_set_child (GTK_BUTTON (btn), pic);
      set_attachment_data (btn, link ? link : image, title ? title : "image", mime);
      g_signal_connect (btn, "clicked", G_CALLBACK (on_open_attachment), w);
      gtk_box_append (GTK_BOX (box), btn);

      q->w = w;
      g_set_weak_pointer (&q->target, pic);
      rc_client_fetch_bytes (w->client, image, on_thumb_bytes, q);
    }
  else
    {
      GtkWidget *card = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
      GtkWidget *icon = gtk_image_new_from_icon_name (rc_file_kind_icon (kind));
      GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      GtkWidget *name = gtk_label_new (title ? title : "File");
      g_autofree char *sz = size > 0 ? g_format_size (size) : NULL;
      g_autofree char *sub = sz ? g_strdup_printf ("%s · %s", rc_file_kind_label (kind), sz)
                                : g_strdup (rc_file_kind_label (kind));
      GtkWidget *subl = gtk_label_new (sub);
      GtkWidget *open = gtk_button_new_from_icon_name ("view-reveal-symbolic");
      const char *url = video ? video : audio ? audio : link;

      gtk_widget_add_css_class (card, "file-card");
      gtk_image_set_pixel_size (GTK_IMAGE (icon), 32);
      gtk_box_append (GTK_BOX (card), icon);
      gtk_label_set_xalign (GTK_LABEL (name), 0);
      gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_MIDDLE);
      gtk_widget_add_css_class (name, "heading");
      gtk_label_set_xalign (GTK_LABEL (subl), 0);
      gtk_widget_add_css_class (subl, "caption");
      gtk_widget_add_css_class (subl, "dim-label");
      gtk_box_append (GTK_BOX (col), name);
      gtk_box_append (GTK_BOX (col), subl);
      gtk_widget_set_hexpand (col, TRUE);
      gtk_widget_set_valign (col, GTK_ALIGN_CENTER);
      gtk_box_append (GTK_BOX (card), col);

      gtk_widget_add_css_class (open, "flat");
      gtk_widget_set_valign (open, GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text (open, "Open in Side Panel");
      set_attachment_data (open, url, title ? title : "file", mime);
      g_signal_connect (open, "clicked", G_CALLBACK (on_open_attachment), w);
      gtk_box_append (GTK_BOX (card), open);
      gtk_box_append (GTK_BOX (box), card);

      /* text / diff files: show the first lines right in the chat */
      if ((kind == RC_FILE_DIFF || kind == RC_FILE_TEXT) && url && (size == 0 || size <= INLINE_SNIPPET_MAX))
        {
          GtkWidget *snip_btn = gtk_button_new ();
          GtkWidget *tv = gtk_text_view_new ();
          MediaReq *q = g_new0 (MediaReq, 1);

          gtk_text_view_set_editable (GTK_TEXT_VIEW (tv), FALSE);
          gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (tv), FALSE);
          gtk_text_view_set_monospace (GTK_TEXT_VIEW (tv), TRUE);
          gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (tv), GTK_WRAP_CHAR);
          gtk_widget_set_can_target (tv, FALSE);
          gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (tv)), "Loading preview…", -1);
          gtk_widget_add_css_class (snip_btn, "file-snippet");
          gtk_widget_set_tooltip_text (snip_btn, "Click to open the whole file in the side panel");
          gtk_button_set_child (GTK_BUTTON (snip_btn), tv);
          set_attachment_data (snip_btn, url, title ? title : "file", mime);
          g_signal_connect (snip_btn, "clicked", G_CALLBACK (on_open_attachment), w);
          gtk_box_append (GTK_BOX (box), snip_btn);

          q->w = w;
          q->diff = kind == RC_FILE_DIFF;
          g_set_weak_pointer (&q->target, tv);
          rc_client_fetch_bytes (w->client, url, on_snippet_bytes, q);
        }
    }

  if (desc && *desc)
    {
      g_autofree char *mk = format_markup (desc);
      GtkWidget *d = gtk_label_new (NULL);
      gtk_label_set_markup (GTK_LABEL (d), mk);
      gtk_label_set_wrap (GTK_LABEL (d), TRUE);
      gtk_label_set_xalign (GTK_LABEL (d), 0);
      gtk_box_append (GTK_BOX (box), d);
    }
  return box;
}

static void
rebuild_attachments (MsgView *v)
{
  JsonArray *atts = jarr (v->msg, "attachments");
  GtkWidget *child;
  guint n = atts ? json_array_get_length (atts) : 0;

  while ((child = gtk_widget_get_first_child (v->attach_box)) != NULL)
    gtk_box_remove (GTK_BOX (v->attach_box), child);
  for (guint i = 0; i < n; i++)
    {
      JsonObject *a = json_array_get_object_element (atts, i);
      GtkWidget *wdg = a ? build_attachment (v->list->w, v->msg, a, i) : NULL;
      if (wdg)
        gtk_box_append (GTK_BOX (v->attach_box), wdg);
    }
  v->n_attachments = n;
  gtk_widget_set_visible (v->attach_box, n > 0);
}

/* --- reactions ------------------------------------------------------------ */

static void
send_reaction (Win        *w,
               const char *msg_id,
               const char *shortcode)
{
  g_autofree char *emoji = g_str_has_prefix (shortcode, ":") ? g_strdup (shortcode)
                                                              : g_strdup_printf (":%s:", shortcode);
  post_action (w, "/api/v1/chat.react", "Reaction",
               "messageId", msg_id, "emoji", emoji, NULL, NULL);
}

static void
on_reaction_clicked (GtkButton *b,
                     MsgView   *v)
{
  send_reaction (v->list->w, v->id, g_object_get_data (G_OBJECT (b), "shortcode"));
}

static void
rebuild_reactions (MsgView *v)
{
  Win *w = v->list->w;
  JsonObject *r = jobj (v->msg, "reactions");
  GtkWidget *child;
  g_autoptr(GList) keys = r ? json_object_get_members (r) : NULL;

  while ((child = gtk_widget_get_first_child (v->reactions_box)) != NULL)
    gtk_box_remove (GTK_BOX (v->reactions_box), child);

  for (GList *k = keys; k; k = k->next)
    {
      const char *code = k->data;
      JsonArray *users = jarr (jobj (r, code), "usernames");
      guint n = users ? json_array_get_length (users) : 0;
      const char *emoji = rc_emoji_lookup (code);
      g_autofree char *label = NULL;
      GString *tip = g_string_new (NULL);
      gboolean mine = FALSE;
      GtkWidget *b;

      if (n == 0)
        {
          g_string_free (tip, TRUE);
          continue;
        }
      for (guint i = 0; i < n; i++)
        {
          const char *u = json_array_get_string_element (users, i);
          if (is_me (w, u))
            mine = TRUE;
          g_string_append_printf (tip, "%s%s", i ? ", " : "", u);
        }
      g_string_append_printf (tip, " reacted with %s", code);
      label = g_strdup_printf ("%s %u", emoji ? emoji : code, n);
      b = gtk_button_new_with_label (label);
      gtk_widget_add_css_class (b, "reaction");
      if (mine)
        gtk_widget_add_css_class (b, "reacted");
      gtk_widget_set_tooltip_text (b, tip->str);
      g_string_free (tip, TRUE);
      g_object_set_data_full (G_OBJECT (b), "shortcode", g_strdup (code), g_free);
      g_signal_connect (b, "clicked", G_CALLBACK (on_reaction_clicked), v);
      gtk_box_append (GTK_BOX (v->reactions_box), b);
    }
  gtk_widget_set_visible (v->reactions_box, gtk_widget_get_first_child (v->reactions_box) != NULL);
}

/* --- meta: edited / pinned / starred / read receipt ----------------------- */

static gboolean
starred_by_me (Win        *w,
               JsonObject *m)
{
  JsonArray *s = jarr (m, "starred");
  const char *me = rc_client_get_user_id (w->client);

  for (guint i = 0; s && i < json_array_get_length (s); i++)
    if (g_strcmp0 (jstr (json_array_get_object_element (s, i), "_id"), me) == 0)
      return TRUE;
  return FALSE;
}

static void
update_meta (MsgView *v)
{
  Win *w = v->list->w;
  GString *s = g_string_new (NULL);
  JsonObject *u = msg_user (v->msg);
  gboolean mine = is_me (w, jstr (u, "username"));

  if (json_object_has_member (v->msg, "editedAt"))
    g_string_append (s, "edited ");
  if (json_object_get_boolean_member_with_default (v->msg, "pinned", FALSE))
    g_string_append (s, "📌 ");
  if (starred_by_me (w, v->msg))
    g_string_append (s, "★ ");
  if (mine)
    {
      gboolean unread = json_object_get_boolean_member_with_default (v->msg, "unread", FALSE);
      g_string_append (s, unread ? "✓" : "✓✓");
      gtk_widget_set_tooltip_text (v->meta, unread ? "Sent – not read yet\n(right-click → Read By… for details)"
                                                   : "Read\n(right-click → Read By… for read times)");
      if (unread)
        gtk_widget_remove_css_class (v->meta, "read");
      else
        gtk_widget_add_css_class (v->meta, "read");
    }
  gtk_label_set_text (GTK_LABEL (v->meta), g_strstrip (s->str));
  gtk_widget_set_visible (v->meta, s->len > 0);
  g_string_free (s, TRUE);
}

static void
update_thread_button (MsgView *v)
{
  gint64 tcount = jint (v->msg, "tcount");
  g_autofree char *label = NULL;
  g_autofree char *last = NULL;

  if (v->list->is_thread || tcount <= 0)
    {
      gtk_widget_set_visible (v->thread_btn, FALSE);
      return;
    }
  last = format_time (parse_ts_ms (v->msg, "tlm"));
  label = g_strdup_printf ("💬 %" G_GINT64_FORMAT " %s%s%s", tcount, tcount == 1 ? "reply" : "replies",
                           *last ? " · last " : "", last);
  gtk_button_set_label (GTK_BUTTON (v->thread_btn), label);
  gtk_widget_set_visible (v->thread_btn, TRUE);
}

static void
update_view (MsgView *v)
{
  JsonArray *atts = jarr (v->msg, "attachments");
  g_autofree char *text = NULL;
  g_autofree char *markup = NULL;

  if (v->body == NULL)  /* system message */
    return;
  text = message_text (v->msg);
  markup = format_markup (text);
  gtk_label_set_markup (GTK_LABEL (v->body), markup);
  gtk_widget_set_visible (v->body, *text != '\0');
  if ((atts ? json_array_get_length (atts) : 0) != v->n_attachments)
    rebuild_attachments (v);
  rebuild_reactions (v);
  update_meta (v);
  update_thread_button (v);
}

/* --- message action menu ------------------------------------------------- */

static void
popdown_from (GtkWidget *wdg)
{
  GtkWidget *pop = gtk_widget_get_ancestor (wdg, GTK_TYPE_POPOVER);
  if (pop)
    gtk_popover_popdown (GTK_POPOVER (pop));
}

static void
on_react_picked (const char *shortcode,
                 const char *emoji,
                 gpointer    data)
{
  MsgView *v = data;
  send_reaction (v->list->w, v->id, shortcode);
}

static GtkWidget *
react_popover (MsgView *v)
{
  return rc_emoji_picker_new (on_react_picked, v);
}

static void
on_react_menu_create (GtkMenuButton *mb,
                      gpointer       data)
{
  gtk_menu_button_set_popover (mb, react_popover (data));
}

static void
start_edit (Win     *w,
            MsgView *v)
{
  g_autofree char *text = message_text (v->msg);

  g_free (w->editing_id);
  w->editing_id = g_strdup (v->id);
  gtk_editable_set_text (GTK_EDITABLE (w->composer), text);
  gtk_editable_set_position (GTK_EDITABLE (w->composer), -1);
  gtk_revealer_set_reveal_child (w->edit_bar, TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (w->composer));
}

static void
cancel_edit (Win *w)
{
  if (!w->editing_id)
    return;
  g_clear_pointer (&w->editing_id, g_free);
  gtk_editable_set_text (GTK_EDITABLE (w->composer), "");
  gtk_revealer_set_reveal_child (w->edit_bar, FALSE);
}

typedef struct {
  Win  *w;
  char *rid;
  char *msg_id;
  char *text;
} MsgRef;

static MsgRef *
msg_ref_new (MsgView *v)
{
  MsgRef *r = g_new0 (MsgRef, 1);
  r->w = v->list->w;
  r->rid = g_strdup (jstr (v->msg, "rid"));
  r->msg_id = g_strdup (v->id);
  r->text = message_plain_summary (v->msg);
  return r;
}

static void
msg_ref_free (MsgRef *r)
{
  g_free (r->rid);
  g_free (r->msg_id);
  g_free (r->text);
  g_free (r);
}

static void
on_delete_response (AdwAlertDialog *d,
                    const char     *response,
                    MsgRef         *r)
{
  if (g_str_equal (response, "delete"))
    post_action (r->w, "/api/v1/chat.delete", "Delete",
                 "roomId", r->rid, "msgId", r->msg_id, NULL, NULL);
}

static void
confirm_delete (MsgView *v)
{
  AdwDialog *d = adw_alert_dialog_new ("Delete Message?", "This can't be undone.");
  MsgRef *r = msg_ref_new (v);

  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (d), "cancel", "_Cancel", "delete", "_Delete", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  g_object_set_data_full (G_OBJECT (d), "ref", r, (GDestroyNotify) msg_ref_free);
  g_signal_connect (d, "response", G_CALLBACK (on_delete_response), r);
  adw_dialog_present (d, GTK_WIDGET (v->list->w->window));
}

static void
on_receipts (RcClient     *client,
             JsonObject   *result,
             const GError *error,
             gpointer      data)
{
  Win *w = data;
  JsonArray *rs;
  GString *body;
  AdwDialog *d;

  if (rc_error_is_cancelled (error))
    return;
  if (error)
    {
      toast (w, "Read receipts aren't available on this server (%s)", error->message);
      return;
    }
  rs = jarr (result, "receipts");
  body = g_string_new (NULL);
  for (guint i = 0; rs && i < json_array_get_length (rs); i++)
    {
      JsonObject *r = json_array_get_object_element (rs, i);
      JsonObject *u = jobj (r, "user");
      const char *name = jstr (u, "name");
      g_autofree char *when = format_time (parse_ts_ms (r, "ts"));
      g_string_append_printf (body, "%s%s — read %s", body->len ? "\n" : "",
                              name && *name ? name : (jstr (u, "username") ? jstr (u, "username") : "?"),
                              when);
    }
  if (body->len == 0)
    g_string_append (body, "Nobody has read this message yet.");
  d = adw_alert_dialog_new ("Read By", body->str);
  g_string_free (body, TRUE);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "close", "_Close");
  adw_dialog_present (d, GTK_WIDGET (w->window));
}

static void
on_remind_picked (GDateTime *dt,
                  gpointer   data)
{
  MsgRef *r = data;
  g_autofree char *when = NULL;
  g_autofree char *text = NULL;

  if (!dt)
    return;
  text = g_strdup_printf ("Message: %s", *r->text ? r->text : "(attachment)");
  rc_planner_add_reminder (r->w->planner, text, g_date_time_to_unix (dt), r->rid, r->msg_id);
  when = rc_format_when (g_date_time_to_unix (dt));
  toast (r->w, "I'll remind you %s", when);
}

static void
on_popover_closed_unparent (GtkPopover *p)
{
  g_idle_add_once ((GSourceOnceFunc) gtk_widget_unparent, p);
}

static void
show_remind_popover (MsgView *v)
{
  MsgRef *r = msg_ref_new (v);
  GtkWidget *pop = rc_datetime_popover_new (FALSE, on_remind_picked, r);

  g_object_set_data_full (G_OBJECT (pop), "ref", r, (GDestroyNotify) msg_ref_free);
  gtk_widget_set_parent (pop, v->unit);
  g_signal_connect (pop, "closed", G_CALLBACK (on_popover_closed_unparent), NULL);
  gtk_popover_popup (GTK_POPOVER (pop));
}

static void
on_menu_item (GtkButton *b,
              MsgView   *v)
{
  const char *what = g_object_get_data (G_OBJECT (b), "what");
  Win *w = v->list->w;
  const char *rid = jstr (v->msg, "rid");

  popdown_from (GTK_WIDGET (b));

  if (g_str_equal (what, "thread"))
    open_thread (w, jstr (v->msg, "tmid") ? jstr (v->msg, "tmid") : v->id);
  else if (g_str_equal (what, "copy"))
    {
      g_autofree char *t = message_plain_summary (v->msg);
      gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (w->window)), t);
      toast (w, "Copied");
    }
  else if (g_str_equal (what, "remind"))
    show_remind_popover (v);
  else if (g_str_equal (what, "pin"))
    post_action (w, json_object_get_boolean_member_with_default (v->msg, "pinned", FALSE)
                    ? "/api/v1/chat.unPinMessage" : "/api/v1/chat.pinMessage",
                 "Pin", "messageId", v->id, NULL, NULL, NULL, NULL);
  else if (g_str_equal (what, "star"))
    {
      gboolean starred = starred_by_me (w, v->msg);
      post_action (w, starred ? "/api/v1/chat.unStarMessage" : "/api/v1/chat.starMessage",
                   "Star", "messageId", v->id, NULL, NULL, NULL, NULL);
      /* stars are per-user and not always broadcast: update locally */
      if (starred)
        json_object_remove_member (v->msg, "starred");
      else
        {
          JsonArray *arr = json_array_new ();
          JsonObject *me = json_object_new ();
          json_object_set_string_member (me, "_id", rc_client_get_user_id (w->client));
          json_array_add_object_element (arr, me);
          json_object_set_array_member (v->msg, "starred", arr);
        }
      update_meta (v);
    }
  else if (g_str_equal (what, "receipts"))
    {
      g_autofree char *esc = g_uri_escape_string (v->id, NULL, FALSE);
      g_autofree char *path = g_strdup_printf ("/api/v1/chat.getMessageReadReceipts?messageId=%s", esc);
      rc_client_get (w->client, path, on_receipts, w);
    }
  else if (g_str_equal (what, "edit"))
    start_edit (w, v);
  else if (g_str_equal (what, "delete"))
    confirm_delete (v);
  (void) rid;
}

static void
add_menu_item (GtkWidget  *box,
               MsgView    *v,
               const char *icon,
               const char *label,
               const char *what,
               gboolean    destructive)
{
  GtkWidget *b = gtk_button_new ();
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *l = gtk_label_new (label);

  gtk_box_append (GTK_BOX (row), gtk_image_new_from_icon_name (icon));
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_box_append (GTK_BOX (row), l);
  gtk_button_set_child (GTK_BUTTON (b), row);
  gtk_widget_add_css_class (b, "flat");
  if (destructive)
    gtk_widget_add_css_class (b, "error");
  g_object_set_data (G_OBJECT (b), "what", (gpointer) what);
  g_signal_connect (b, "clicked", G_CALLBACK (on_menu_item), v);
  gtk_box_append (GTK_BOX (box), b);
}

static void
on_more_menu_create (GtkMenuButton *mb,
                     gpointer       data)
{
  MsgView *v = data;
  Win *w = v->list->w;
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gboolean mine = is_me (w, jstr (msg_user (v->msg), "username"));
  gboolean pinned = json_object_get_boolean_member_with_default (v->msg, "pinned", FALSE);

  gtk_widget_add_css_class (pop, "menu");
  if (!v->list->is_thread)
    add_menu_item (box, v, "mail-reply-sender-symbolic", "Reply in Thread", "thread", FALSE);
  add_menu_item (box, v, "edit-copy-symbolic", "Copy Text", "copy", FALSE);
  add_menu_item (box, v, "alarm-symbolic", "Remind Me…", "remind", FALSE);
  add_menu_item (box, v, "view-pin-symbolic", pinned ? "Unpin" : "Pin", "pin", FALSE);
  add_menu_item (box, v, "starred-symbolic", starred_by_me (w, v->msg) ? "Unstar" : "Star", "star", FALSE);
  if (mine)
    {
      add_menu_item (box, v, "object-select-symbolic", "Read By…", "receipts", FALSE);
      add_menu_item (box, v, "document-edit-symbolic", "Edit", "edit", FALSE);
      add_menu_item (box, v, "user-trash-symbolic", "Delete", "delete", TRUE);
    }
  gtk_popover_set_child (GTK_POPOVER (pop), box);
  gtk_menu_button_set_popover (mb, pop);
}

static void
on_unit_secondary (GtkGestureClick *g,
                   int              n,
                   double           x,
                   double           y,
                   MsgView         *v)
{
  /* our menu (with Copy Text) replaces GtkLabel's Cut/Copy/Paste menu */
  gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
  gtk_menu_button_popup (GTK_MENU_BUTTON (v->more_btn));
}

static void
on_unit_long_press (GtkGestureLongPress *g,
                    double               x,
                    double               y,
                    MsgView             *v)
{
  gtk_menu_button_popup (GTK_MENU_BUTTON (v->more_btn));
}

static void
on_thread_btn (GtkButton *b,
               MsgView   *v)
{
  open_thread (v->list->w, v->id);
}

static GtkWidget *
build_unit (MsgList    *l,
            MsgView    *v)
{
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *bodyrow = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *react = gtk_menu_button_new ();
  GtkGesture *click = gtk_gesture_click_new ();
  GtkGesture *lp = gtk_gesture_long_press_new ();

  gtk_widget_add_css_class (overlay, "msg-unit");

  v->body = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (v->body), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (v->body), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (v->body), 0);
  gtk_label_set_selectable (GTK_LABEL (v->body), TRUE);
  gtk_widget_set_focusable (v->body, FALSE);
  gtk_widget_set_hexpand (v->body, TRUE);
  gtk_widget_add_css_class (v->body, "msg-body");
  gtk_box_append (GTK_BOX (bodyrow), v->body);

  v->meta = gtk_label_new (NULL);
  gtk_widget_set_valign (v->meta, GTK_ALIGN_END);
  gtk_widget_add_css_class (v->meta, "msg-meta");
  gtk_widget_add_css_class (v->meta, "caption");
  gtk_box_append (GTK_BOX (bodyrow), v->meta);
  gtk_box_append (GTK_BOX (col), bodyrow);

  v->attach_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (col), v->attach_box);

  v->reactions_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_halign (v->reactions_box, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (col), v->reactions_box);

  v->thread_btn = gtk_button_new ();
  gtk_widget_add_css_class (v->thread_btn, "flat");
  gtk_widget_add_css_class (v->thread_btn, "thread-button");
  gtk_widget_set_halign (v->thread_btn, GTK_ALIGN_START);
  g_signal_connect (v->thread_btn, "clicked", G_CALLBACK (on_thread_btn), v);
  gtk_box_append (GTK_BOX (col), v->thread_btn);

  gtk_overlay_set_child (GTK_OVERLAY (overlay), col);

  /* hover toolbar: react / thread / more */
  gtk_widget_add_css_class (actions, "msg-actions");
  gtk_widget_set_halign (actions, GTK_ALIGN_END);
  gtk_widget_set_valign (actions, GTK_ALIGN_START);
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (react), "face-smile-symbolic");
  gtk_widget_set_tooltip_text (react, "Add Reaction");
  gtk_menu_button_set_create_popup_func (GTK_MENU_BUTTON (react), on_react_menu_create, v, NULL);
  gtk_box_append (GTK_BOX (actions), react);
  if (!l->is_thread)
    {
      GtkWidget *th = gtk_button_new_from_icon_name ("mail-reply-sender-symbolic");
      gtk_widget_set_tooltip_text (th, "Reply in Thread");
      g_signal_connect (th, "clicked", G_CALLBACK (on_thread_btn), v);
      gtk_box_append (GTK_BOX (actions), th);
    }
  v->more_btn = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (v->more_btn), "view-more-symbolic");
  gtk_widget_set_tooltip_text (v->more_btn, "More");
  gtk_menu_button_set_create_popup_func (GTK_MENU_BUTTON (v->more_btn), on_more_menu_create, v, NULL);
  gtk_box_append (GTK_BOX (actions), v->more_btn);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), actions);

  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_SECONDARY);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click), GTK_PHASE_CAPTURE);
  g_signal_connect (click, "pressed", G_CALLBACK (on_unit_secondary), v);
  gtk_widget_add_controller (overlay, GTK_EVENT_CONTROLLER (click));
  gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (lp), TRUE);
  g_signal_connect (lp, "pressed", G_CALLBACK (on_unit_long_press), v);
  gtk_widget_add_controller (overlay, GTK_EVENT_CONTROLLER (lp));

  v->unit = overlay;
  return overlay;
}

static void
render_message (MsgList    *l,
                JsonObject *m)
{
  Win *w = l->w;
  const char *id = jstr (m, "_id");
  const char *type = jstr (m, "t");
  JsonObject *u = msg_user (m);
  const char *username = jstr (u, "username");
  const char *name = jstr (u, "name");
  const char *display = (name && *name) ? name : (username ? username : "Unknown");
  gint64 ts = parse_ts_ms (m, "ts");
  MsgView *v;

  if (id == NULL)
    return;

  v = g_new0 (MsgView, 1);
  v->list = l;
  v->id = g_strdup (id);
  v->msg = json_object_ref (m);

  if (type && *type)
    {
      const char *act = system_action (type);
      g_autofree char *text = message_text (m);
      g_autofree char *line = NULL;

      if (act == NULL)
        {
          msg_view_free (v);
          return; /* unknown system/event message: hide */
        }
      line = g_strdup_printf ("%s %s%s%s", display, act, *text ? ": " : "", text);
      v->outer = gtk_label_new (line);
      gtk_label_set_wrap (GTK_LABEL (v->outer), TRUE);
      gtk_widget_add_css_class (v->outer, "dim-label");
      gtk_widget_add_css_class (v->outer, "caption");
      gtk_widget_add_css_class (v->outer, "system-message");
      gtk_box_append (GTK_BOX (l->box), v->outer);
      g_hash_table_insert (l->views, v->id, v);
      g_clear_pointer (&l->last_user, g_free);
      return;
    }

  build_unit (l, v);

  if (l->last_user && username && g_str_equal (l->last_user, username) &&
      ts - l->last_ts < GROUP_WINDOW_MS && ts >= l->last_ts)
    {
      gtk_widget_add_css_class (v->unit, "msg-continued");
      gtk_widget_set_margin_start (v->unit, 36 + 12);
      v->outer = v->unit;
      gtk_box_append (GTK_BOX (l->box), v->outer);
    }
  else
    {
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
      GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
      GtkWidget *author = gtk_label_new (display);
      g_autofree char *time = format_time (ts);
      GtkWidget *time_label = gtk_label_new (time);
      g_autofree char *key = user_avatar_key (username);
      GtkWidget *avatar = make_avatar (w, key, display, 36);

      gtk_widget_add_css_class (row, "msg-block");
      gtk_widget_set_valign (avatar, GTK_ALIGN_START);
      gtk_box_append (GTK_BOX (row), avatar);

      gtk_label_set_xalign (GTK_LABEL (author), 0);
      gtk_label_set_ellipsize (GTK_LABEL (author), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand (author, TRUE);
      gtk_widget_add_css_class (author, "msg-author");
      if (is_me (w, username))
        gtk_widget_add_css_class (author, "msg-author-me");
      gtk_box_append (GTK_BOX (head), author);

      gtk_widget_add_css_class (time_label, "dim-label");
      gtk_widget_add_css_class (time_label, "caption");
      gtk_box_append (GTK_BOX (head), time_label);

      gtk_box_append (GTK_BOX (col), head);
      gtk_box_append (GTK_BOX (col), v->unit);
      gtk_widget_set_hexpand (col, TRUE);
      gtk_box_append (GTK_BOX (row), col);
      v->outer = row;
      gtk_box_append (GTK_BOX (l->box), row);
    }

  g_hash_table_insert (l->views, v->id, v);
  g_free (l->last_user);
  l->last_user = g_strdup (username);
  l->last_ts = ts;
  rebuild_attachments (v);
  update_view (v);
}

static void
msg_list_rerender (MsgList *l)
{
  msg_list_clear_widgets (l);
  for (guint i = 0; i < l->order->len; i++)
    render_message (l, g_ptr_array_index (l->order, i));
}

static gboolean
belongs_in (MsgList    *l,
            JsonObject *m)
{
  const char *tmid = jstr (m, "tmid");

  if (l->is_thread)
    return g_strcmp0 (tmid, l->w->thread_root) == 0 ||
           g_strcmp0 (jstr (m, "_id"), l->w->thread_root) == 0;
  /* main timeline: hide thread replies unless "also send to channel" */
  return tmid == NULL || json_object_get_boolean_member_with_default (m, "tshow", FALSE);
}

/* add a new message or update an existing one */
static void
msg_list_add (MsgList    *l,
              JsonObject *m)
{
  const char *id = jstr (m, "_id");
  MsgView *v;

  if (id == NULL || !belongs_in (l, m))
    return;

  v = g_hash_table_lookup (l->views, id);
  if (v)
    {
      for (guint i = 0; i < l->order->len; i++)
        if (g_strcmp0 (jstr (g_ptr_array_index (l->order, i), "_id"), id) == 0)
          {
            json_object_unref (l->order->pdata[i]);
            l->order->pdata[i] = json_object_ref (m);
          }
      json_object_unref (v->msg);
      v->msg = json_object_ref (m);
      update_view (v);
      return;
    }

  /* not rendered yet but maybe known (hidden system msg): avoid dupes */
  for (guint i = 0; i < l->order->len; i++)
    if (g_strcmp0 (jstr (g_ptr_array_index (l->order, i), "_id"), id) == 0)
      return;

  g_ptr_array_add (l->order, json_object_ref (m));
  render_message (l, m);
}

static void clear_restore (gpointer data);

static void
msg_list_remove (MsgList    *l,
                 const char *id)
{
  for (guint i = 0; i < l->order->len; i++)
    if (g_strcmp0 (jstr (g_ptr_array_index (l->order, i), "_id"), id) == 0)
      {
        Win *w = l->w;
        g_ptr_array_remove_index (l->order, i);
        if (!l->is_thread)
          {
            /* keep the same distance from the bottom across the re-render */
            GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (w->msg_scroller);
            w->restore_from_bottom = w->stick_bottom
                                     ? gtk_adjustment_get_page_size (adj)
                                     : gtk_adjustment_get_upper (adj) - gtk_adjustment_get_value (adj);
            g_timeout_add_once (400, clear_restore, w);
          }
        msg_list_rerender (l);
        return;
      }
}

static void
msg_list_mark_own_read (MsgList *l)
{
  GHashTableIter it;
  gpointer val;

  g_hash_table_iter_init (&it, l->views);
  while (g_hash_table_iter_next (&it, NULL, &val))
    {
      MsgView *v = val;
      if (v->body && json_object_get_boolean_member_with_default (v->msg, "unread", FALSE))
        {
          json_object_set_boolean_member (v->msg, "unread", FALSE);
          update_meta (v);
        }
    }
}

static MsgView *
last_own_message (MsgList *l)
{
  for (gint i = (gint) l->order->len - 1; i >= 0; i--)
    {
      JsonObject *m = g_ptr_array_index (l->order, i);
      if (is_me (l->w, jstr (msg_user (m), "username")) && !jstr (m, "t"))
        return g_hash_table_lookup (l->views, jstr (m, "_id"));
    }
  return NULL;
}

/* ------------------------------------------------------------------------ */
/* scrolling                                                                 */

static void
scroll_to_bottom (Win *w)
{
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (w->msg_scroller);

  w->stick_bottom = TRUE;
  gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));
}

static void
on_adj_value_changed (GtkAdjustment *adj,
                      Win           *w)
{
  double v = gtk_adjustment_get_value (adj);
  double upper = gtk_adjustment_get_upper (adj);
  double page = gtk_adjustment_get_page_size (adj);

  /* only a real scroll (same content & viewport size) changes "stick to bottom";
   * layout changes (edit bar, images loading, reactions) must not */
  if (upper == w->last_upper && page == w->last_page)
    w->stick_bottom = v >= upper - page - 40;
  gtk_widget_set_visible (w->jump_button, v < upper - page - 40);
}

static void
on_adj_changed (GtkAdjustment *adj,
                Win           *w)
{
  w->last_upper = gtk_adjustment_get_upper (adj);
  w->last_page = gtk_adjustment_get_page_size (adj);
  if (w->restore_from_bottom >= 0)
    {
      gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj) - w->restore_from_bottom);
      return;
    }
  if (w->stick_bottom)
    gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));
}

static void
on_jump_clicked (GtkButton *b,
                 Win       *w)
{
  scroll_to_bottom (w);
}

static void
on_thread_adj_changed (GtkAdjustment *adj,
                       Win           *w)
{
  /* thread panel always follows the newest reply */
  gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));
}

/* ------------------------------------------------------------------------ */
/* side column: preview / thread / planner                                   */

static void
show_side (Win        *w,
           const char *page)
{
  gtk_stack_set_visible_child_name (w->side_stack, page);
  adw_overlay_split_view_set_show_sidebar (w->side_split, TRUE);
}

static void
close_side (gpointer data)
{
  Win *w = data;
  adw_overlay_split_view_set_show_sidebar (w->side_split, FALSE);
  if (g_strcmp0 (gtk_stack_get_visible_child_name (w->side_stack), "thread") == 0)
    g_clear_pointer (&w->thread_root, g_free);
}

typedef struct {
  Win  *w;
  char *url;
  char *name;
  char *mime;
} PreviewReq;

static void
on_preview_bytes (RcClient *c, GBytes *bytes, const char *ctype, const GError *error, gpointer data)
{
  PreviewReq *q = data;

  if (rc_error_is_cancelled (error))
    goto out;
  /* ignore stale downloads if another file was opened meanwhile */
  if (g_strcmp0 (g_object_get_data (G_OBJECT (q->w->preview), "url"), q->url) != 0)
    goto out;
  if (error || !bytes)
    rc_preview_set_error (q->w->preview, q->name, error ? error->message : "Empty response");
  else
    rc_preview_set_content (q->w->preview, q->name,
                            q->mime && *q->mime ? q->mime : ctype, bytes);
out:
  g_free (q->url);
  g_free (q->name);
  g_free (q->mime);
  g_free (q);
}

static void
open_preview_url (Win        *w,
                  const char *url,
                  const char *name,
                  const char *mime)
{
  PreviewReq *q;

  if (url == NULL)
    return;
  q = g_new0 (PreviewReq, 1);
  q->w = w;
  q->url = g_strdup (url);
  q->name = g_strdup (name);
  q->mime = g_strdup (mime);
  g_object_set_data_full (G_OBJECT (w->preview), "url", g_strdup (url), g_free);
  rc_preview_set_loading (w->preview, name);
  show_side (w, "preview");
  rc_client_fetch_bytes (w->client, url, on_preview_bytes, q);
}

typedef struct {
  Win  *w;
  char *root;
} ThreadReq;

static void
thread_req_free (ThreadReq *q)
{
  g_free (q->root);
  g_free (q);
}

static gint
by_ts (gconstpointer a, gconstpointer b)
{
  gint64 x = parse_ts_ms (*(JsonObject **) a, "ts");
  gint64 y = parse_ts_ms (*(JsonObject **) b, "ts");
  return x < y ? -1 : x > y ? 1 : 0;
}

static void
on_thread_messages (RcClient *client, JsonObject *result, const GError *error, gpointer data)
{
  ThreadReq *q = data;
  Win *w = q->w;
  JsonArray *msgs;
  g_autoptr(GPtrArray) sorted = NULL;

  if (rc_error_is_cancelled (error) || g_strcmp0 (q->root, w->thread_root) != 0)
    goto out;
  if (error)
    {
      toast (w, "Could not load thread: %s", error->message);
      goto out;
    }
  msgs = jarr (result, "messages");
  sorted = g_ptr_array_new ();
  for (guint i = 0; msgs && i < json_array_get_length (msgs); i++)
    g_ptr_array_add (sorted, json_array_get_object_element (msgs, i));
  g_ptr_array_sort (sorted, by_ts);
  for (guint i = 0; i < sorted->len; i++)
    msg_list_add (w->thread_list, g_ptr_array_index (sorted, i));
out:
  thread_req_free (q);
}

static void
on_thread_root (RcClient *client, JsonObject *result, const GError *error, gpointer data)
{
  ThreadReq *q = data;
  Win *w = q->w;
  JsonObject *m;

  if (rc_error_is_cancelled (error) || g_strcmp0 (q->root, w->thread_root) != 0)
    goto out;
  m = jobj (result, "message");
  if (m)
    {
      /* root goes first, replies after it */
      g_ptr_array_insert (w->thread_list->order, 0, json_object_ref (m));
      msg_list_rerender (w->thread_list);
    }
out:
  thread_req_free (q);
}

static void
open_thread (Win        *w,
             const char *root_id)
{
  g_autofree char *esc = g_uri_escape_string (root_id, NULL, FALSE);
  g_autofree char *p1 = g_strdup_printf ("/api/v1/chat.getMessage?msgId=%s", esc);
  g_autofree char *p2 = g_strdup_printf ("/api/v1/chat.getThreadMessages?tmid=%s&count=100", esc);
  ThreadReq *q1 = g_new0 (ThreadReq, 1), *q2 = g_new0 (ThreadReq, 1);

  g_free (w->thread_root);
  w->thread_root = g_strdup (root_id);
  msg_list_clear (w->thread_list);
  show_side (w, "thread");
  gtk_widget_grab_focus (GTK_WIDGET (w->thread_composer));

  q1->w = q2->w = w;
  q1->root = g_strdup (root_id);
  q2->root = g_strdup (root_id);
  rc_client_get (w->client, p1, on_thread_root, q1);
  rc_client_get (w->client, p2, on_thread_messages, q2);
}

/* ------------------------------------------------------------------------ */
/* rooms & history                                                           */

static void
mark_read (Win  *w,
           Room *r)
{
  g_autoptr(JsonBuilder) b = json_builder_new ();

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "rid");
  json_builder_add_string_value (b, r->rid);
  json_builder_end_object (b);
  rc_client_post (w->client, "/api/v1/subscriptions.read", json_builder_get_root (b), ignore_reply, NULL);
  r->unread = 0;
  room_update_badge (r);
}

typedef struct {
  Win      *w;
  char     *rid;
  gboolean  older;
} RoomReq;

static RoomReq *
room_req_new (Win        *w,
              const char *rid)
{
  RoomReq *q = g_new0 (RoomReq, 1);
  q->w = w;
  q->rid = g_strdup (rid);
  return q;
}

static void
room_req_free (RoomReq *q)
{
  g_free (q->rid);
  g_free (q);
}

static void
clear_restore (gpointer data)
{
  Win *w = data;
  w->restore_from_bottom = -1;
}

static void
on_history (RcClient     *client,
            JsonObject   *result,
            const GError *error,
            gpointer      data)
{
  RoomReq *q = data;
  Win *w = q->w;
  JsonArray *msgs;
  guint n;

  if (rc_error_is_cancelled (error) || g_strcmp0 (q->rid, w->current_rid) != 0)
    goto out;

  if (error)
    {
      toast (w, "Could not load messages: %s", error->message);
      goto out;
    }

  msgs = jarr (result, "messages");
  n = msgs ? json_array_get_length (msgs) : 0;

  if (q->older)
    {
      GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (w->msg_scroller);
      g_autoptr(GPtrArray) older = g_ptr_array_new_with_free_func ((GDestroyNotify) json_object_unref);

      for (gint i = (gint) n - 1; i >= 0; i--)
        {
          JsonObject *m = json_array_get_object_element (msgs, i);
          if (m && belongs_in (w->main_list, m))
            g_ptr_array_add (older, json_object_ref (m));
        }
      /* prepend, keeping the visible position */
      for (guint i = 0; i < older->len; i++)
        g_ptr_array_insert (w->main_list->order, i, json_object_ref (g_ptr_array_index (older, i)));
      w->restore_from_bottom = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_value (adj);
      w->stick_bottom = FALSE;
      msg_list_rerender (w->main_list);
      g_timeout_add_once (400, clear_restore, w);
    }
  else
    {
      msg_list_clear (w->main_list);
      /* API returns newest first */
      for (gint i = (gint) n - 1; i >= 0; i--)
        {
          JsonObject *m = json_array_get_object_element (msgs, i);
          if (m)
            msg_list_add (w->main_list, m);
        }
      scroll_to_bottom (w);
    }
  gtk_widget_set_visible (w->older_button, n >= HISTORY_COUNT);

out:
  room_req_free (q);
}

static void
on_older_clicked (GtkButton *b,
                  Win       *w)
{
  Room *r = w->current_rid ? g_hash_table_lookup (w->rooms, w->current_rid) : NULL;
  JsonObject *first;
  g_autofree char *iso = NULL, *iso_esc = NULL, *rid_esc = NULL, *path = NULL;
  RoomReq *q;

  if (!r || w->main_list->order->len == 0)
    return;
  first = g_ptr_array_index (w->main_list->order, 0);
  iso = ms_to_iso (parse_ts_ms (first, "ts"));
  iso_esc = g_uri_escape_string (iso, NULL, FALSE);
  rid_esc = g_uri_escape_string (r->rid, NULL, FALSE);
  path = g_strdup_printf ("/api/v1/%s.history?roomId=%s&count=%d&latest=%s",
                          history_endpoint (r->type), rid_esc, HISTORY_COUNT, iso_esc);
  q = room_req_new (w, r->rid);
  q->older = TRUE;
  rc_client_get (w->client, path, on_history, q);
}

static void
on_room_info (RcClient     *client,
              JsonObject   *result,
              const GError *error,
              gpointer      data)
{
  RoomReq *q = data;
  Win *w = q->w;
  const char *topic;

  if (rc_error_is_cancelled (error) || error || g_strcmp0 (q->rid, w->current_rid) != 0)
    goto out;

  topic = jstr (jobj (result, "room"), "topic");
  if (topic && *topic)
    adw_window_title_set_subtitle (w->room_title, topic);

out:
  room_req_free (q);
}

static void
update_typing_label (Win *w)
{
  GHashTableIter it;
  gpointer key, val;
  gint64 now = g_get_monotonic_time ();
  g_autoptr(GPtrArray) names = g_ptr_array_new ();

  g_hash_table_iter_init (&it, w->typing);
  while (g_hash_table_iter_next (&it, &key, &val))
    {
      if (*(gint64 *) val < now)
        g_hash_table_iter_remove (&it);
      else
        g_ptr_array_add (names, key);
    }

  if (names->len == 0)
    gtk_label_set_text (GTK_LABEL (w->typing_label), "");
  else if (names->len == 1)
    {
      g_autofree char *s = g_strdup_printf ("%s is typing…", (char *) names->pdata[0]);
      gtk_label_set_text (GTK_LABEL (w->typing_label), s);
    }
  else
    {
      g_autofree char *s = g_strdup_printf ("%u people are typing…", names->len);
      gtk_label_set_text (GTK_LABEL (w->typing_label), s);
    }
}

static gboolean
typing_tick (gpointer data)
{
  update_typing_label (data);
  return G_SOURCE_CONTINUE;
}

static void
open_room (Win  *w,
           Room *r)
{
  g_autofree char *rid_esc = g_uri_escape_string (r->rid, NULL, FALSE);
  g_autofree char *hist = g_strdup_printf ("/api/v1/%s.history?roomId=%s&count=%d",
                                           history_endpoint (r->type), rid_esc, HISTORY_COUNT);
  g_autofree char *info = g_strdup_printf ("/api/v1/rooms.info?roomId=%s", rid_esc);
  g_autofree char *subtitle = NULL;

  g_free (w->current_rid);
  w->current_rid = g_strdup (r->rid);
  cancel_edit (w);
  g_hash_table_remove_all (w->typing);
  update_typing_label (w);
  if (g_strcmp0 (gtk_stack_get_visible_child_name (w->side_stack), "thread") == 0)
    close_side (w);

  if (g_strcmp0 (r->type, "d") == 0)
    subtitle = g_strdup_printf ("@%s", r->name ? r->name : "");
  else if (g_strcmp0 (r->type, "p") == 0)
    subtitle = g_strdup ("Private group");
  else
    subtitle = g_strdup ("Channel");

  adw_window_title_set_title (w->room_title, room_display_name (r));
  adw_window_title_set_subtitle (w->room_title, subtitle);
  adw_navigation_page_set_title (w->content_page, room_display_name (r));
  gtk_stack_set_visible_child_name (w->content_stack, "chat");
  msg_list_clear (w->main_list);
  gtk_widget_set_visible (w->older_button, FALSE);

  rc_client_get (w->client, hist, on_history, room_req_new (w, r->rid));
  rc_client_get (w->client, info, on_room_info, room_req_new (w, r->rid));
  rc_client_realtime_watch_room (w->client, r->rid);
  mark_read (w, r);

  if (gtk_list_box_get_selected_row (w->room_list) != GTK_LIST_BOX_ROW (r->row))
    {
      w->rebuilding = TRUE;
      gtk_list_box_select_row (w->room_list, GTK_LIST_BOX_ROW (r->row));
      w->rebuilding = FALSE;
    }
  adw_navigation_split_view_set_show_content (w->split, TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (w->composer));
}

static void
on_room_activated (GtkListBox    *box,
                   GtkListBoxRow *row,
                   Win           *w)
{
  Room *r = g_object_get_data (G_OBJECT (row), "room");

  if (r && !w->rebuilding)
    open_room (w, r);
}

/* ------------------------------------------------------------------------ */
/* subscriptions                                                             */

static void
on_subscriptions (RcClient     *client,
                  JsonObject   *result,
                  const GError *error,
                  gpointer      data)
{
  Win *w = data;
  JsonArray *subs;

  if (rc_error_is_cancelled (error))
    return;
  if (error)
    {
      if (g_error_matches (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_AUTH))
        {
          toast (w, "Session expired, please sign in again");
          session_forget ();
          show_login (w);
          return;
        }
      toast (w, "Could not load rooms: %s", error->message);
      return;
    }

  subs = jarr (result, "update");

  w->rebuilding = TRUE;
  gtk_list_box_remove_all (w->room_list);
  g_hash_table_remove_all (w->rooms);

  for (guint i = 0; subs && i < json_array_get_length (subs); i++)
    {
      JsonObject *s = json_array_get_object_element (subs, i);
      Room *r;

      if (!s || !jstr (s, "rid") || !json_object_get_boolean_member_with_default (s, "open", TRUE))
        continue;

      r = g_new0 (Room, 1);
      r->rid = g_strdup (jstr (s, "rid"));
      r->name = g_strdup (jstr (s, "name"));
      r->fname = g_strdup (jstr (s, "fname"));
      r->type = g_strdup (jstr (s, "t"));
      r->unread = (int) jint (s, "unread");
      r->favorite = json_object_get_boolean_member_with_default (s, "f", FALSE);
      if (g_strcmp0 (r->rid, w->current_rid) == 0)
        r->unread = 0;
      g_hash_table_insert (w->rooms, r->rid, r);
      gtk_list_box_append (w->room_list, build_room_row (w, r));
    }

  if (w->current_rid)
    {
      Room *cur = g_hash_table_lookup (w->rooms, w->current_rid);
      if (cur)
        gtk_list_box_select_row (w->room_list, GTK_LIST_BOX_ROW (cur->row));
    }
  w->rebuilding = FALSE;
}

static void
load_subscriptions (Win *w)
{
  rc_client_get (w->client, "/api/v1/subscriptions.get", on_subscriptions, w);
}

static gboolean
subs_reload_cb (gpointer data)
{
  Win *w = data;
  w->subs_reload_id = 0;
  load_subscriptions (w);
  return G_SOURCE_REMOVE;
}

static void
schedule_subs_reload (Win *w)
{
  if (w->subs_reload_id == 0)
    w->subs_reload_id = g_timeout_add (800, subs_reload_cb, w);
}

/* ------------------------------------------------------------------------ */
/* notifications & realtime                                                  */

static gboolean
message_mentions_me (Win        *w,
                     JsonObject *m)
{
  JsonArray *mentions = jarr (m, "mentions");

  for (guint i = 0; mentions && i < json_array_get_length (mentions); i++)
    {
      const char *un = jstr (json_array_get_object_element (mentions, i), "username");
      if (un && (is_me (w, un) || g_str_equal (un, "all") || g_str_equal (un, "here")))
        return TRUE;
    }
  return FALSE;
}

static void
send_notification (Win        *w,
                   const char *id,
                   const char *title,
                   const char *body,
                   const char *rid)
{
  g_autoptr(GNotification) n = g_notification_new (title);
  GApplication *app = G_APPLICATION (gtk_window_get_application (w->window));

  if (!app)
    return;
  g_notification_set_body (n, body);
  if (rid)
    g_notification_set_default_action_and_target (n, "app.open-room", "s", rid);
  g_application_send_notification (app, id, n);
}

static void
notify_message (Win        *w,
                Room       *room,
                JsonObject *m,
                const char *author)
{
  g_autofree char *title = NULL;
  g_autofree char *raw = message_plain_summary (m);
  g_autofree char *body = rc_emoji_replace_shortcodes (raw);

  if (g_strcmp0 (room->type, "d") == 0)
    title = g_strdup (author);
  else
    title = g_strdup_printf ("%s in %s", author, room_display_name (room));
  send_notification (w, room->rid, title, body, room->rid);
}

static void
on_realtime (RcClient   *client,
             const char *event,
             JsonObject *payload,
             gpointer    data)
{
  Win *w = data;

  if (g_str_equal (event, "connected"))
    adw_banner_set_revealed (w->banner, FALSE);
  else if (g_str_equal (event, "disconnected"))
    adw_banner_set_revealed (w->banner, TRUE);
  else if (g_str_equal (event, "subscriptions-changed"))
    schedule_subs_reload (w);
  else if (g_str_equal (event, "deleted"))
    {
      const char *id = jstr (payload, "_id");
      if (id && g_strcmp0 (jstr (payload, "rid"), w->current_rid) == 0)
        {
          msg_list_remove (w->main_list, id);
          msg_list_remove (w->thread_list, id);
        }
    }
  else if (g_str_equal (event, "typing"))
    {
      const char *un = jstr (payload, "username");
      if (!un || is_me (w, un) || g_strcmp0 (jstr (payload, "rid"), w->current_rid) != 0)
        return;
      if (json_object_get_boolean_member_with_default (payload, "typing", FALSE))
        {
          gint64 *exp = g_new (gint64, 1);
          *exp = g_get_monotonic_time () + TYPING_TIMEOUT_US;
          g_hash_table_insert (w->typing, g_strdup (un), exp);
        }
      else
        g_hash_table_remove (w->typing, un);
      update_typing_label (w);
    }
  else if (g_str_equal (event, "messages-read"))
    {
      if (g_strcmp0 (jstr (payload, "rid"), w->current_rid) == 0)
        {
          msg_list_mark_own_read (w->main_list);
          msg_list_mark_own_read (w->thread_list);
        }
    }
  else if (g_str_equal (event, "message") && payload)
    {
      const char *rid = jstr (payload, "rid");
      JsonObject *u = msg_user (payload);
      const char *un = jstr (u, "username");
      const char *nm = jstr (u, "name");
      gboolean from_me = is_me (w, un);
      gboolean is_edit = json_object_has_member (payload, "editedAt");
      gboolean active = gtk_window_is_active (w->window);
      Room *room = rid ? g_hash_table_lookup (w->rooms, rid) : NULL;

      if (rid == NULL)
        return;
      if (room == NULL)
        {
          schedule_subs_reload (w);
          return;
        }

      if (un)
        {
          g_hash_table_remove (w->typing, un);
          update_typing_label (w);
        }

      if (g_strcmp0 (rid, w->current_rid) == 0)
        {
          if (from_me && !is_edit)
            w->stick_bottom = TRUE;  /* always follow my own new messages */
          msg_list_add (w->main_list, payload);
          if (w->thread_root)
            msg_list_add (w->thread_list, payload);
          if (active && !from_me)
            mark_read (w, room);
        }
      else if (!from_me && !is_edit && !jstr (payload, "t"))
        {
          room->unread++;
          room_update_badge (room);
        }

      if (!from_me && !is_edit && !jstr (payload, "t") &&
          (!active || g_strcmp0 (rid, w->current_rid) != 0) &&
          (g_strcmp0 (room->type, "d") == 0 || message_mentions_me (w, payload)))
        notify_message (w, room, payload, (nm && *nm) ? nm : (un ? un : "Someone"));
    }
}

/* ------------------------------------------------------------------------ */
/* composer: send / edit / typing / emoji                                    */

typedef struct {
  Win      *w;
  char     *text;
  GtkEntry *entry;
} SendReq;

static void
on_sent (RcClient     *client,
         JsonObject   *result,
         const GError *error,
         gpointer      data)
{
  SendReq *q = data;
  Win *w = q->w;

  if (rc_error_is_cancelled (error))
    goto out;

  if (error)
    {
      toast (w, "Message not sent: %s", error->message);
      if (*gtk_editable_get_text (GTK_EDITABLE (q->entry)) == '\0')
        gtk_editable_set_text (GTK_EDITABLE (q->entry), q->text);
    }
  else
    {
      JsonObject *m = jobj (result, "message");
      if (m && g_strcmp0 (jstr (m, "rid"), w->current_rid) == 0)
        {
          msg_list_add (w->main_list, m);
          if (w->thread_root)
            msg_list_add (w->thread_list, m);
          scroll_to_bottom (w);
        }
    }

out:
  g_free (q->text);
  g_free (q);
}

static void
stop_typing (Win *w)
{
  g_clear_handle_id (&w->typing_stop_id, g_source_remove);
  if (w->typing_sent_at && w->current_rid)
    rc_client_realtime_typing (w->client, w->current_rid, w->me_username, FALSE);
  w->typing_sent_at = 0;
}

static void
send_text (Win        *w,
           GtkEntry   *entry,
           const char *tmid)
{
  g_autofree char *text = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry))));
  g_autoptr(JsonBuilder) b = NULL;
  SendReq *q;

  if (*text == '\0' || w->current_rid == NULL)
    return;

  if (entry == w->composer)
    stop_typing (w);

  /* editing an existing message */
  if (entry == w->composer && w->editing_id)
    {
      post_action (w, "/api/v1/chat.update", "Edit",
                   "roomId", w->current_rid, "msgId", w->editing_id, "text", text);
      cancel_edit (w);
      return;
    }

  b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "message");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "rid");
  json_builder_add_string_value (b, w->current_rid);
  json_builder_set_member_name (b, "msg");
  json_builder_add_string_value (b, text);
  if (tmid)
    {
      json_builder_set_member_name (b, "tmid");
      json_builder_add_string_value (b, tmid);
    }
  json_builder_end_object (b);
  json_builder_end_object (b);

  q = g_new0 (SendReq, 1);
  q->w = w;
  q->text = g_strdup (text);
  q->entry = entry;
  gtk_editable_set_text (GTK_EDITABLE (entry), "");
  w->stick_bottom = TRUE;
  rc_client_post (w->client, "/api/v1/chat.sendMessage", json_builder_get_root (b), on_sent, q);
}

static void
on_send_clicked (GtkButton *b,
                 Win       *w)
{
  send_text (w, w->composer, NULL);
}

static void
on_thread_send (GtkWidget *wdg,
                Win       *w)
{
  if (w->thread_root)
    send_text (w, w->thread_composer, w->thread_root);
}

static gboolean
typing_stop_cb (gpointer data)
{
  Win *w = data;
  w->typing_stop_id = 0;
  stop_typing (w);
  return G_SOURCE_REMOVE;
}

static void
on_composer_changed (GtkEditable *e,
                     Win         *w)
{
  gboolean has_text = *gtk_editable_get_text (e) != '\0';
  gint64 now = g_get_monotonic_time ();

  gtk_widget_set_sensitive (w->send_button, has_text);
  if (!has_text || w->editing_id || !w->current_rid)
    {
      stop_typing (w);
      return;
    }
  if (now - w->typing_sent_at > 4 * G_USEC_PER_SEC)
    {
      rc_client_realtime_typing (w->client, w->current_rid, w->me_username, TRUE);
      w->typing_sent_at = now;
    }
  g_clear_handle_id (&w->typing_stop_id, g_source_remove);
  w->typing_stop_id = g_timeout_add_seconds (5, typing_stop_cb, w);
}

static gboolean
on_composer_key (GtkEventControllerKey *c,
                 guint                  keyval,
                 guint                  keycode,
                 GdkModifierType        state,
                 Win                   *w)
{
  if (keyval == GDK_KEY_Escape && w->editing_id)
    {
      cancel_edit (w);
      return TRUE;
    }
  if (keyval == GDK_KEY_Up && *gtk_editable_get_text (GTK_EDITABLE (w->composer)) == '\0')
    {
      MsgView *v = last_own_message (w->main_list);
      if (v)
        {
          start_edit (w, v);
          return TRUE;
        }
    }
  return FALSE;
}

static void
on_cancel_edit (GtkButton *b,
                Win       *w)
{
  cancel_edit (w);
}

static void
on_emoji_picked (GtkEmojiChooser *chooser,
                 const char      *text,
                 GtkEditable     *entry)
{
  int pos = gtk_editable_get_position (entry);
  gtk_editable_insert_text (entry, text, -1, &pos);
  gtk_editable_set_position (entry, pos);
  gtk_widget_grab_focus (GTK_WIDGET (entry));
}

/* ------------------------------------------------------------------------ */
/* uploads: button, drag & drop                                              */

typedef struct {
  Win    *w;
  char   *rid;
  char   *tmid;
  char   *name;
  char   *mime;
  char   *desc;
  GBytes *bytes;
} Upload;

static void
upload_free (Upload *u)
{
  g_free (u->rid);
  g_free (u->tmid);
  g_free (u->name);
  g_free (u->mime);
  g_free (u->desc);
  g_clear_pointer (&u->bytes, g_bytes_unref);
  g_free (u);
}

static void
on_upload_done (RcClient *client, JsonObject *result, const GError *error, gpointer data)
{
  Upload *u = data;

  if (!rc_error_is_cancelled (error))
    {
      if (error)
        toast (u->w, "Upload of %s failed: %s", u->name, error->message);
      else
        {
          toast (u->w, "Sent %s", u->name);
          if (g_strcmp0 (u->rid, u->w->current_rid) == 0)
            scroll_to_bottom (u->w);
        }
    }
  upload_free (u);
}

static void
upload_legacy (Upload *u)
{
  g_autofree char *esc = g_uri_escape_string (u->rid, NULL, FALSE);
  g_autofree char *path = g_strdup_printf ("/api/v1/rooms.upload/%s", esc);
  const char *fields[] = { "description", u->desc ? u->desc : "",
                           "tmid", u->tmid ? u->tmid : "", NULL };

  rc_client_upload (u->w->client, path, u->name, u->mime, u->bytes, fields, on_upload_done, u);
}

static void
on_media_uploaded (RcClient *client, JsonObject *result, const GError *error, gpointer data)
{
  Upload *u = data;
  const char *file_id;
  g_autoptr(JsonBuilder) b = NULL;
  g_autofree char *rid_esc = NULL, *fid_esc = NULL, *path = NULL;

  if (rc_error_is_cancelled (error))
    {
      upload_free (u);
      return;
    }
  /* Rocket.Chat < 6.8: rooms.media doesn't exist, use rooms.upload */
  if (error && (g_error_matches (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_NOT_FOUND) ||
                g_error_matches (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_HTTP)))
    {
      upload_legacy (u);
      return;
    }
  if (error)
    {
      on_upload_done (client, NULL, error, u);
      return;
    }

  file_id = jstr (jobj (result, "file"), "_id");
  if (!file_id)
    {
      g_autoptr(GError) e = g_error_new_literal (RC_CLIENT_ERROR, RC_CLIENT_ERROR_PARSE, "no file id");
      on_upload_done (client, NULL, e, u);
      return;
    }

  b = json_builder_new ();
  json_builder_begin_object (b);
  if (u->desc && *u->desc)
    {
      /* caption goes into the attachment description (shown once, under the file) */
      json_builder_set_member_name (b, "description");
      json_builder_add_string_value (b, u->desc);
    }
  if (u->tmid)
    {
      json_builder_set_member_name (b, "tmid");
      json_builder_add_string_value (b, u->tmid);
    }
  json_builder_end_object (b);
  rid_esc = g_uri_escape_string (u->rid, NULL, FALSE);
  fid_esc = g_uri_escape_string (file_id, NULL, FALSE);
  path = g_strdup_printf ("/api/v1/rooms.mediaConfirm/%s/%s", rid_esc, fid_esc);
  rc_client_post (client, path, json_builder_get_root (b), on_upload_done, u);
}

static void
start_upload (Upload *u)
{
  g_autofree char *esc = g_uri_escape_string (u->rid, NULL, FALSE);
  g_autofree char *path = g_strdup_printf ("/api/v1/rooms.media/%s", esc);
  g_autofree char *size = g_format_size (g_bytes_get_size (u->bytes));

  toast (u->w, "Uploading %s (%s)…", u->name, size);
  rc_client_upload (u->w->client, path, u->name, u->mime, u->bytes, NULL, on_media_uploaded, u);
}

typedef struct {
  Win      *w;
  char     *tmid;
  GList    *files;   /* GFile* */
  GtkEntry *desc;
} UploadBatch;

static void
batch_free (UploadBatch *b)
{
  g_free (b->tmid);
  g_list_free_full (b->files, g_object_unref);
  g_free (b);
}

typedef struct {
  Win  *w;
  char *rid;
  char *tmid;
  char *desc;
} LoadCtx;

static void
on_file_loaded (GObject *src, GAsyncResult *res, gpointer data)
{
  LoadCtx *c = data;
  g_autoptr(GError) error = NULL;
  g_autofree char *etag = NULL;
  GBytes *bytes = g_file_load_bytes_finish (G_FILE (src), res, &etag, &error);
  g_autofree char *name = g_file_get_basename (G_FILE (src));

  if (!bytes)
    toast (c->w, "Could not read %s: %s", name, error->message);
  else
    {
      Upload *u = g_new0 (Upload, 1);
      gsize len = 0;
      const guchar *d = g_bytes_get_data (bytes, &len);
      g_autofree char *ct = g_content_type_guess (name, d, MIN (len, 4096), NULL);

      u->w = c->w;
      u->rid = g_strdup (c->rid);
      u->tmid = g_strdup (c->tmid);
      u->name = g_strdup (name);
      u->mime = g_content_type_get_mime_type (ct);
      u->desc = g_strdup (c->desc);
      u->bytes = bytes;
      start_upload (u);
    }
  g_free (c->rid);
  g_free (c->tmid);
  g_free (c->desc);
  g_free (c);
}

static void
on_upload_response (AdwAlertDialog *d,
                    const char     *response,
                    UploadBatch    *b)
{
  const char *desc;

  if (!g_str_equal (response, "send") || !b->w->current_rid)
    return;
  desc = gtk_editable_get_text (GTK_EDITABLE (b->desc));
  for (GList *l = b->files; l; l = l->next)
    {
      LoadCtx *c = g_new0 (LoadCtx, 1);
      c->w = b->w;
      c->rid = g_strdup (b->w->current_rid);
      c->tmid = g_strdup (b->tmid);
      /* attach the caption to the first file only */
      c->desc = l == b->files ? g_strdup (desc) : NULL;
      g_file_load_bytes_async (l->data, NULL, on_file_loaded, c);
    }
}

static void
confirm_upload (Win        *w,
                GList      *files,  /* transfer full */
                const char *tmid)
{
  UploadBatch *b = g_new0 (UploadBatch, 1);
  guint n = g_list_length (files);
  g_autofree char *first = g_file_get_basename (files->data);
  g_autofree char *heading = n == 1 ? g_strdup_printf ("Send “%s”?", first)
                                    : g_strdup_printf ("Send %u files?", n);
  AdwDialog *d;
  Room *r = w->current_rid ? g_hash_table_lookup (w->rooms, w->current_rid) : NULL;
  g_autofree char *body = g_strdup_printf ("To %s%s", r ? room_display_name (r) : "this room",
                                           tmid ? " (in thread)" : "");

  b->w = w;
  b->files = files;
  b->tmid = g_strdup (tmid);
  b->desc = GTK_ENTRY (gtk_entry_new ());
  gtk_entry_set_placeholder_text (b->desc, "Add a message (optional)");
  gtk_entry_set_activates_default (b->desc, TRUE);

  d = adw_alert_dialog_new (heading, body);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (d), GTK_WIDGET (b->desc));
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (d), "cancel", "_Cancel", "send", "_Send", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "send", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "send");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  g_object_set_data_full (G_OBJECT (d), "batch", b, (GDestroyNotify) batch_free);
  g_signal_connect (d, "response", G_CALLBACK (on_upload_response), b);
  adw_dialog_present (d, GTK_WIDGET (w->window));
}

typedef struct {
  Win  *w;
  char *tmid;
} PickCtx;

static void
on_files_picked (GObject *src, GAsyncResult *res, gpointer data)
{
  PickCtx *c = data;
  g_autoptr(GListModel) model = gtk_file_dialog_open_multiple_finish (GTK_FILE_DIALOG (src), res, NULL);
  GList *files = NULL;

  for (guint i = 0; model && i < g_list_model_get_n_items (model); i++)
    files = g_list_append (files, g_list_model_get_item (model, i));
  if (files)
    confirm_upload (c->w, files, c->tmid);
  g_free (c->tmid);
  g_free (c);
}

static void
pick_files (Win        *w,
            const char *tmid)
{
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new ();
  PickCtx *c = g_new0 (PickCtx, 1);

  if (!w->current_rid)
    return;
  c->w = w;
  c->tmid = g_strdup (tmid);
  gtk_file_dialog_set_title (dialog, "Attach Files");
  gtk_file_dialog_open_multiple (dialog, w->window, NULL, on_files_picked, c);
}

static void
on_attach_clicked (GtkButton *b,
                   Win       *w)
{
  pick_files (w, NULL);
}

static void
on_thread_attach_clicked (GtkButton *b,
                          Win       *w)
{
  if (w->thread_root)
    pick_files (w, w->thread_root);
}

static gboolean
on_drop (GtkDropTarget *t,
         const GValue  *value,
         double         x,
         double         y,
         Win           *w)
{
  GSList *list;
  GList *files = NULL;

  if (!w->current_rid || !G_VALUE_HOLDS (value, GDK_TYPE_FILE_LIST))
    return FALSE;
  list = g_value_get_boxed (value);
  for (GSList *l = list; l; l = l->next)
    files = g_list_append (files, g_object_ref (l->data));
  if (!files)
    return FALSE;
  confirm_upload (w, files, g_object_get_data (G_OBJECT (t), "thread") ? w->thread_root : NULL);
  return TRUE;
}

static void
add_drop_target (Win       *w,
                 GtkWidget *wdg,
                 gboolean   thread)
{
  GtkDropTarget *t = gtk_drop_target_new (GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);

  if (thread)
    g_object_set_data (G_OBJECT (t), "thread", GINT_TO_POINTER (1));
  g_signal_connect (t, "drop", G_CALLBACK (on_drop), w);
  gtk_widget_add_controller (wdg, GTK_EVENT_CONTROLLER (t));
}

/* ------------------------------------------------------------------------ */
/* untrusted certificate dialog (self-signed / private CA servers)           */

typedef void (*RetryFunc) (Win *w);

typedef struct {
  Win       *w;
  RetryFunc  retry;
} CertCtx;

static void
on_cert_response (AdwAlertDialog *d,
                  const char     *response,
                  CertCtx        *c)
{
  if (g_str_equal (response, "trust"))
    {
      rc_client_trust_rejected_certificate (c->w->client);
      c->retry (c->w);
    }
  else
    show_login (c->w);
}

static void
show_cert_dialog (Win       *w,
                  RetryFunc  retry)
{
  const char *host = NULL;
  GTlsCertificateFlags errors = 0;
  GTlsCertificate *cert = rc_client_get_rejected_certificate (w->client, &host, &errors);
  g_autofree char *fp = NULL, *problems = NULL, *subject = NULL, *issuer = NULL, *until = NULL;
  g_autofree char *body = NULL;
  g_autoptr(GDateTime) exp = NULL;
  CertCtx *c;
  AdwDialog *d;

  if (cert == NULL)
    {
      toast (w, "The server's TLS certificate is not trusted");
      return;
    }
  fp = rc_cert_fingerprint (cert);
  problems = rc_cert_describe_errors (errors);
  subject = g_tls_certificate_get_subject_name (cert);
  issuer = g_tls_certificate_get_issuer_name (cert);
  exp = g_tls_certificate_get_not_valid_after (cert);
  until = exp ? g_date_time_format (exp, "%Y-%m-%d") : g_strdup ("?");

  {
    g_autofree char *e_host = g_markup_escape_text (host ? host : "", -1);
    g_autofree char *e_prob = g_markup_escape_text (problems, -1);
    g_autofree char *e_subj = g_markup_escape_text (subject ? subject : "?", -1);
    g_autofree char *e_iss = g_markup_escape_text (issuer ? issuer : "?", -1);
    body = g_strdup_printf ("The certificate presented by <b>%s</b> could not be verified:\n%s\n\n"
                            "<small><b>Subject:</b> %s\n<b>Issuer:</b> %s\n<b>Valid until:</b> %s\n"
                            "<b>SHA-256:</b>\n<tt>%s</tt></small>\n\n"
                            "Only continue if this is your own server (for example a self-hosted "
                            "Rocket.Chat with a self-signed certificate) and the fingerprint matches. "
                            "The certificate will be remembered for this server.",
                            e_host, e_prob, e_subj, e_iss, until, fp);
  }

  c = g_new0 (CertCtx, 1);
  c->w = w;
  c->retry = retry;
  d = adw_alert_dialog_new ("Untrusted Certificate", NULL);
  adw_alert_dialog_set_body_use_markup (ADW_ALERT_DIALOG (d), TRUE);
  adw_alert_dialog_set_body (ADW_ALERT_DIALOG (d), body);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (d), "cancel", "_Cancel", "trust", "_Trust and Connect", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "trust", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  g_object_set_data_full (G_OBJECT (d), "ctx", c, g_free);
  g_signal_connect (d, "response", G_CALLBACK (on_cert_response), c);
  adw_dialog_present (d, GTK_WIDGET (w->window));
}

/* ------------------------------------------------------------------------ */
/* login / logout                                                            */

static void
enter_main (Win *w)
{
  g_autofree char *sub = g_strdup_printf ("@%s · %s", w->me_username ? w->me_username : "",
                                          rc_client_is_secure (w->client) ? "HTTPS" : "HTTP");

  adw_window_title_set_subtitle (w->sidebar_title, sub);
  gtk_stack_set_visible_child_name (w->stack, "main");
  gtk_stack_set_visible_child_name (w->content_stack, "empty");
  adw_banner_set_revealed (w->banner, FALSE);
  load_subscriptions (w);
  rc_client_realtime_start (w->client, on_realtime, w);
  rc_planner_set_client (w->planner, w->client);

  if (!rc_client_is_secure (w->client))
    toast (w, "Connected over plain HTTP (port 80) – traffic is not encrypted");
}

static void
set_login_busy (Win      *w,
                gboolean  busy)
{
  gtk_widget_set_sensitive (w->login_button, !busy);
  gtk_widget_set_visible (w->login_spinner, busy);
}

static void
on_login_done (RcClient     *client,
               JsonObject   *result,
               const GError *error,
               gpointer      data)
{
  Win *w = data;
  JsonObject *me;

  if (rc_error_is_cancelled (error))
    return;
  set_login_busy (w, FALSE);

  if (error)
    {
      if (g_error_matches (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_UNTRUSTED_CERT))
        show_cert_dialog (w, do_login);
      else if (g_error_matches (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_API) && strstr (error->message, "totp"))
        toast (w, "Two-factor code required – enter it in the 2FA field");
      else
        toast (w, "Sign in failed: %s", error->message);
      return;
    }

  me = jobj (jobj (result, "data"), "me");
  g_free (w->me_username);
  w->me_username = g_strdup (jstr (me, "username"));

  gtk_editable_set_text (GTK_EDITABLE (w->pass_row), "");
  gtk_editable_set_text (GTK_EDITABLE (w->code_row), "");
  session_store (w,
                 gtk_editable_get_text (GTK_EDITABLE (w->server_row)),
                 (int) adw_combo_row_get_selected (w->mode_row),
                 gtk_editable_get_text (GTK_EDITABLE (w->user_row)));
  enter_main (w);
}

static void
do_login (Win *w)
{
  g_autoptr(GError) error = NULL;
  const char *server = gtk_editable_get_text (GTK_EDITABLE (w->server_row));
  const char *user = gtk_editable_get_text (GTK_EDITABLE (w->user_row));
  const char *pass = gtk_editable_get_text (GTK_EDITABLE (w->pass_row));
  const char *code = gtk_editable_get_text (GTK_EDITABLE (w->code_row));
  RcConnMode mode = (RcConnMode) adw_combo_row_get_selected (w->mode_row);

  if (!gtk_widget_get_sensitive (w->login_button))
    return;

  if (!rc_client_set_server (w->client, server, mode, &error))
    {
      toast (w, "%s", error->message);
      return;
    }
  if (*user == '\0' || *pass == '\0')
    {
      toast (w, "Please enter your username and password");
      return;
    }

  set_login_busy (w, TRUE);
  rc_client_login (w->client, user, pass, code, on_login_done, w);
}

static void
on_login_clicked (GtkButton *b,
                  Win       *w)
{
  do_login (w);
}

static void
on_login_entry_activated (AdwEntryRow *row,
                          Win         *w)
{
  do_login (w);
}

static void
on_server_changed (GtkEditable *e,
                   Win         *w)
{
  /* "https://…" / "http://…" typed explicitly decides the port by itself */
  gboolean explicit = strstr (gtk_editable_get_text (e), "://") != NULL;

  gtk_widget_set_sensitive (GTK_WIDGET (w->mode_row), !explicit);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (w->mode_row),
                               explicit ? "Taken from the address you typed"
                                        : "Auto tries HTTPS first, then plain HTTP");
}

static void
on_me (RcClient     *client,
       JsonObject   *result,
       const GError *error,
       gpointer      data)
{
  Win *w = data;

  if (rc_error_is_cancelled (error))
    return;

  if (error)
    {
      if (g_error_matches (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_UNTRUSTED_CERT))
        {
          show_cert_dialog (w, try_resume);
          return;
        }
      if (g_error_matches (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_AUTH))
        {
          session_forget ();
          toast (w, "Session expired, please sign in again");
        }
      else
        toast (w, "Could not reach server: %s", error->message);
      show_login (w);
      return;
    }

  g_free (w->me_username);
  w->me_username = g_strdup (jstr (result, "username"));
  enter_main (w);
}

static void
show_login (Win *w)
{
  g_autoptr(GKeyFile) kf = session_load ();
  g_autofree char *server = g_key_file_get_string (kf, "login", "server", NULL);
  g_autofree char *user = g_key_file_get_string (kf, "login", "username", NULL);
  int mode = g_key_file_get_integer (kf, "login", "mode", NULL);

  rc_client_shutdown (w->client);
  if (server && *gtk_editable_get_text (GTK_EDITABLE (w->server_row)) == '\0')
    gtk_editable_set_text (GTK_EDITABLE (w->server_row), server);
  if (user && *gtk_editable_get_text (GTK_EDITABLE (w->user_row)) == '\0')
    gtk_editable_set_text (GTK_EDITABLE (w->user_row), user);
  if (mode >= RC_CONN_AUTO && mode <= RC_CONN_HTTP)
    adw_combo_row_set_selected (w->mode_row, mode);
  set_login_busy (w, FALSE);
  gtk_stack_set_visible_child_name (w->stack, "login");
}

static void
try_resume (Win *w)
{
  g_autoptr(GKeyFile) kf = session_load ();
  g_autofree char *base = g_key_file_get_string (kf, "session", "base_url", NULL);
  g_autofree char *uid = g_key_file_get_string (kf, "session", "user_id", NULL);
  g_autofree char *tok = g_key_file_get_string (kf, "session", "token", NULL);

  if (base && uid && tok && rc_client_set_server (w->client, base, RC_CONN_AUTO, NULL))
    {
      rc_client_set_credentials (w->client, uid, tok);
      gtk_stack_set_visible_child_name (w->stack, "loading");
      rc_client_get (w->client, "/api/v1/me", on_me, w);
    }
  else
    show_login (w);
}

static void
action_logout (GSimpleAction *a,
               GVariant      *p,
               gpointer       data)
{
  Win *w = data;

  rc_client_realtime_stop (w->client);
  rc_client_post (w->client, "/api/v1/logout", NULL, ignore_reply, NULL);
  rc_client_set_credentials (w->client, NULL, NULL);
  rc_planner_set_client (w->planner, NULL);
  session_forget ();

  g_clear_handle_id (&w->subs_reload_id, g_source_remove);
  g_clear_pointer (&w->current_rid, g_free);
  g_clear_pointer (&w->me_username, g_free);
  cancel_edit (w);
  close_side (w);
  w->rebuilding = TRUE;
  gtk_list_box_remove_all (w->room_list);
  w->rebuilding = FALSE;
  g_hash_table_remove_all (w->rooms);
  msg_list_clear (w->main_list);
  msg_list_clear (w->thread_list);

  /* show_login() would cancel the logout request; just switch pages */
  set_login_busy (w, FALSE);
  gtk_stack_set_visible_child_name (w->stack, "login");
}

static void
action_refresh (GSimpleAction *a,
                GVariant      *p,
                gpointer       data)
{
  Win *w = data;
  Room *r;

  load_subscriptions (w);
  if (w->current_rid && (r = g_hash_table_lookup (w->rooms, w->current_rid)))
    open_room (w, r);
}

static void
action_planner (GSimpleAction *a,
                GVariant      *p,
                gpointer       data)
{
  Win *w = data;

  if (adw_overlay_split_view_get_show_sidebar (w->side_split) &&
      g_strcmp0 (gtk_stack_get_visible_child_name (w->side_stack), "planner") == 0)
    close_side (w);
  else
    {
      if (p)
        rc_planner_show_page (w->planner, g_variant_get_string (p, NULL));
      show_side (w, "planner");
    }
}

static void
action_attach (GSimpleAction *a,
               GVariant      *p,
               gpointer       data)
{
  pick_files (data, NULL);
}

static void
action_background (GSimpleAction *a,
                   GVariant      *p,
                   gpointer       data)
{
  gboolean v = g_variant_get_boolean (p);
  g_simple_action_set_state (a, p);
  pref_set_bool ("background", v);
}

/* planner callbacks */
static void
on_planner_alert (const char *title,
                  const char *body,
                  const char *rid,
                  gpointer    data)
{
  Win *w = data;
  g_autofree char *id = g_strdup_printf ("planner-%" G_GINT64_FORMAT, g_get_real_time ());

  send_notification (w, id, title, body, rid);
  toast (w, "⏰ %s", body);
  gdk_display_beep (gtk_widget_get_display (GTK_WIDGET (w->window)));
}

static void
on_planner_open_room (const char *rid,
                      const char *msg_id,
                      gpointer    data)
{
  Win *w = data;
  Room *r = g_hash_table_lookup (w->rooms, rid);

  if (r)
    open_room (w, r);
  else
    toast (w, "That room is not available");
}

/* ------------------------------------------------------------------------ */
/* UI construction                                                           */

static GtkWidget *
build_login_page (Win *w)
{
  GtkWidget *tv = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *clamp = adw_clamp_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 24);
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
  GtkWidget *icon = gtk_image_new_from_icon_name (gtk_icon_theme_has_icon (theme, APP_ID)
                                                  ? APP_ID : "chat-message-new-symbolic");
  GtkWidget *title = gtk_label_new ("Rocket Client");
  GtkWidget *desc = gtk_label_new ("Sign in to your Rocket.Chat server");
  GtkWidget *server_group = adw_preferences_group_new ();
  GtkWidget *account_group = adw_preferences_group_new ();
  GtkWidget *button_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  const char *modes[] = { "Auto (443 → 80)", "HTTPS (443)", "HTTP (80)", NULL };

  adw_header_bar_set_show_title (ADW_HEADER_BAR (header), FALSE);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);

  gtk_widget_set_margin_top (box, 24);
  gtk_widget_set_margin_bottom (box, 24);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 96);
  gtk_box_append (GTK_BOX (box), icon);
  gtk_widget_add_css_class (title, "title-1");
  gtk_box_append (GTK_BOX (box), title);
  gtk_widget_add_css_class (desc, "dim-label");
  gtk_widget_set_margin_top (desc, -18);
  gtk_box_append (GTK_BOX (box), desc);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (server_group), "Server");
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (server_group),
                                         "e.g. chat.example.com, http://10.0.0.5 or https://host:8443");
  w->server_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->server_row), "Server address");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (server_group), GTK_WIDGET (w->server_row));

  w->mode_row = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->mode_row), "Connection");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (w->mode_row), "Auto tries HTTPS first, then plain HTTP");
  adw_combo_row_set_model (w->mode_row, G_LIST_MODEL (gtk_string_list_new (modes)));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (server_group), GTK_WIDGET (w->mode_row));
  g_signal_connect (w->server_row, "changed", G_CALLBACK (on_server_changed), w);
  gtk_box_append (GTK_BOX (box), server_group);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (account_group), "Account");
  w->user_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->user_row), "Username or email");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (account_group), GTK_WIDGET (w->user_row));

  w->pass_row = ADW_PASSWORD_ENTRY_ROW (adw_password_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->pass_row), "Password");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (account_group), GTK_WIDGET (w->pass_row));

  w->code_row = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->code_row), "2FA code (optional)");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (account_group), GTK_WIDGET (w->code_row));
  gtk_box_append (GTK_BOX (box), account_group);

  g_signal_connect (w->server_row, "entry-activated", G_CALLBACK (on_login_entry_activated), w);
  g_signal_connect (w->user_row, "entry-activated", G_CALLBACK (on_login_entry_activated), w);
  g_signal_connect (w->pass_row, "entry-activated", G_CALLBACK (on_login_entry_activated), w);
  g_signal_connect (w->code_row, "entry-activated", G_CALLBACK (on_login_entry_activated), w);

  gtk_widget_set_halign (button_box, GTK_ALIGN_CENTER);
  w->login_button = gtk_button_new_with_label ("Sign In");
  gtk_widget_add_css_class (w->login_button, "pill");
  gtk_widget_add_css_class (w->login_button, "suggested-action");
  g_signal_connect (w->login_button, "clicked", G_CALLBACK (on_login_clicked), w);
  w->login_spinner = gtk_spinner_new ();
  gtk_spinner_start (GTK_SPINNER (w->login_spinner));
  gtk_widget_set_visible (w->login_spinner, FALSE);
  gtk_box_append (GTK_BOX (button_box), w->login_button);
  gtk_box_append (GTK_BOX (button_box), w->login_spinner);
  gtk_box_append (GTK_BOX (box), button_box);

  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 440);
  adw_clamp_set_child (ADW_CLAMP (clamp), box);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), clamp);
  gtk_widget_set_vexpand (scroller, TRUE);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), scroller);
  return tv;
}

static GtkWidget *
build_sidebar (Win *w)
{
  GtkWidget *tv = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *search_btn = gtk_toggle_button_new ();
  GtkWidget *menu_btn = gtk_menu_button_new ();
  GtkWidget *entry = gtk_search_entry_new ();
  GtkWidget *scroller = gtk_scrolled_window_new ();
  g_autoptr(GMenu) menu = g_menu_new ();
  g_autoptr(GMenu) sec1 = g_menu_new ();
  g_autoptr(GMenu) sec2 = g_menu_new ();
  g_autoptr(GMenu) sec3 = g_menu_new ();

  w->sidebar_title = ADW_WINDOW_TITLE (adw_window_title_new ("Rocket Client", ""));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (w->sidebar_title));

  g_menu_append (sec1, "_Tasks", "win.planner::tasks");
  g_menu_append (sec1, "_Timers & Reminders", "win.planner::reminders");
  g_menu_append (sec1, "_Calendar", "win.planner::calendar");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (sec1));
  g_menu_append (sec2, "_Refresh", "win.refresh");
  g_menu_append (sec2, "Run in _Background", "win.background");
  g_menu_append (sec2, "_Log Out", "win.logout");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (sec2));
  g_menu_append (sec3, "_About Rocket Client", "app.about");
  g_menu_append (sec3, "_Quit", "app.quit");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (sec3));
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_btn), "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_btn), G_MENU_MODEL (menu));
  gtk_widget_set_tooltip_text (menu_btn, "Main Menu");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu_btn);

  gtk_button_set_icon_name (GTK_BUTTON (search_btn), "system-search-symbolic");
  gtk_widget_set_tooltip_text (search_btn, "Search Rooms");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), search_btn);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);

  w->search_bar = GTK_SEARCH_BAR (gtk_search_bar_new ());
  w->search_entry = GTK_EDITABLE (entry);
  gtk_search_bar_set_child (w->search_bar, entry);
  gtk_search_bar_connect_entry (w->search_bar, GTK_EDITABLE (entry));
  g_object_bind_property (search_btn, "active", w->search_bar, "search-mode-enabled",
                          G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  g_signal_connect (entry, "search-changed", G_CALLBACK (on_search_changed), w);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), GTK_WIDGET (w->search_bar));

  w->room_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_widget_add_css_class (GTK_WIDGET (w->room_list), "navigation-sidebar");
  gtk_list_box_set_sort_func (w->room_list, room_sort, w, NULL);
  gtk_list_box_set_header_func (w->room_list, room_header, w, NULL);
  gtk_list_box_set_filter_func (w->room_list, room_filter, w, NULL);
  g_signal_connect (w->room_list, "row-activated", G_CALLBACK (on_room_activated), w);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), GTK_WIDGET (w->room_list));
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), scroller);
  gtk_search_bar_set_key_capture_widget (w->search_bar, scroller);

  return GTK_WIDGET (adw_navigation_page_new (tv, "Rooms"));
}

static GtkWidget *
composer_row (Win        *w,
              GtkEntry  **entry_out,
              GCallback   attach_cb,
              GCallback   send_cb,
              GtkWidget **send_out)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *attach = gtk_button_new_from_icon_name ("mail-attachment-symbolic");
  GtkWidget *emoji = gtk_menu_button_new ();
  GtkWidget *chooser = gtk_emoji_chooser_new ();
  GtkWidget *entry = gtk_entry_new ();
  GtkWidget *send = gtk_button_new_from_icon_name ("document-send-symbolic");

  gtk_widget_add_css_class (row, "composer");
  gtk_widget_add_css_class (attach, "flat");
  gtk_widget_set_tooltip_text (attach, "Attach Files");
  g_signal_connect (attach, "clicked", attach_cb, w);
  gtk_box_append (GTK_BOX (row), attach);

  gtk_entry_set_placeholder_text (GTK_ENTRY (entry), "Send a message…");
  gtk_widget_set_hexpand (entry, TRUE);
  g_signal_connect (entry, "activate", send_cb, w);

  gtk_widget_add_css_class (emoji, "flat");
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (emoji), "face-smile-symbolic");
  gtk_widget_set_tooltip_text (emoji, "Insert Emoji");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (emoji), chooser);
  g_signal_connect (chooser, "emoji-picked", G_CALLBACK (on_emoji_picked), entry);

  gtk_box_append (GTK_BOX (row), entry);
  gtk_box_append (GTK_BOX (row), emoji);

  gtk_widget_add_css_class (send, "circular");
  gtk_widget_add_css_class (send, "suggested-action");
  gtk_widget_set_tooltip_text (send, "Send");
  g_signal_connect (send, "clicked", send_cb, w);
  gtk_box_append (GTK_BOX (row), send);

  *entry_out = GTK_ENTRY (entry);
  if (send_out)
    *send_out = send;
  return row;
}

static GtkWidget *
build_thread_panel (Win *w)
{
  GtkWidget *tv = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *close = gtk_button_new_from_icon_name ("window-close-symbolic");
  GtkWidget *scroller = gtk_scrolled_window_new ();

  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  w->thread_title = ADW_WINDOW_TITLE (adw_window_title_new ("Thread", ""));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (w->thread_title));
  gtk_widget_set_tooltip_text (close, "Close Thread");
  g_signal_connect_swapped (close, "clicked", G_CALLBACK (close_side), w);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), close);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);

  w->thread_list = msg_list_new (w, TRUE);
  w->thread_scroller = GTK_SCROLLED_WINDOW (scroller);
  gtk_scrolled_window_set_policy (w->thread_scroller, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (w->thread_scroller, w->thread_list->box);
  g_signal_connect (gtk_scrolled_window_get_vadjustment (w->thread_scroller), "changed",
                    G_CALLBACK (on_thread_adj_changed), w);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), scroller);
  add_drop_target (w, scroller, TRUE);

  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (tv),
                                   composer_row (w, &w->thread_composer,
                                                 G_CALLBACK (on_thread_attach_clicked),
                                                 G_CALLBACK (on_thread_send), NULL));
  gtk_entry_set_placeholder_text (w->thread_composer, "Reply in thread…");
  return tv;
}

static GtkWidget *
build_content (Win *w)
{
  GtkWidget *tv = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *planner_btn = gtk_button_new_from_icon_name ("x-office-calendar-symbolic");
  GtkWidget *empty = adw_status_page_new ();
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *clamp = adw_clamp_new ();
  GtkWidget *timeline = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *chat = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *edit_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *edit_label = gtk_label_new ("Editing message – Esc to cancel");
  GtkWidget *edit_cancel = gtk_button_new_from_icon_name ("window-close-symbolic");
  GtkEventController *keys = gtk_event_controller_key_new ();
  GtkWidget *bottom = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkAdjustment *adj;

  w->room_title = ADW_WINDOW_TITLE (adw_window_title_new ("", ""));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (w->room_title));
  gtk_widget_set_tooltip_text (planner_btn, "Tasks, Timers & Calendar");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (planner_btn), "win.planner");
  gtk_actionable_set_action_target (GTK_ACTIONABLE (planner_btn), "s", "tasks");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), planner_btn);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);

  w->banner = ADW_BANNER (adw_banner_new ("Connection lost – reconnecting…"));
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), GTK_WIDGET (w->banner));

  adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty), "chat-message-new-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (empty), "No Room Selected");
  adw_status_page_set_description (ADW_STATUS_PAGE (empty), "Pick a room or a person from the list");

  /* timeline */
  w->main_list = msg_list_new (w, FALSE);
  w->older_button = gtk_button_new_with_label ("Load older messages");
  gtk_widget_add_css_class (w->older_button, "pill");
  gtk_widget_set_halign (w->older_button, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (w->older_button, 12);
  gtk_widget_set_visible (w->older_button, FALSE);
  g_signal_connect (w->older_button, "clicked", G_CALLBACK (on_older_clicked), w);
  gtk_box_append (GTK_BOX (timeline), w->older_button);
  gtk_box_append (GTK_BOX (timeline), w->main_list->box);
  gtk_widget_set_valign (timeline, GTK_ALIGN_END);

  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 900);
  adw_clamp_set_tightening_threshold (ADW_CLAMP (clamp), 600);
  adw_clamp_set_child (ADW_CLAMP (clamp), timeline);
  w->msg_scroller = GTK_SCROLLED_WINDOW (scroller);
  gtk_scrolled_window_set_policy (w->msg_scroller, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (w->msg_scroller, clamp);
  gtk_widget_set_vexpand (scroller, TRUE);
  adj = gtk_scrolled_window_get_vadjustment (w->msg_scroller);
  g_signal_connect (adj, "value-changed", G_CALLBACK (on_adj_value_changed), w);
  g_signal_connect (adj, "changed", G_CALLBACK (on_adj_changed), w);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), scroller);
  add_drop_target (w, overlay, FALSE);

  w->jump_button = gtk_button_new_from_icon_name ("go-bottom-symbolic");
  gtk_widget_add_css_class (w->jump_button, "osd");
  gtk_widget_add_css_class (w->jump_button, "circular");
  gtk_widget_set_tooltip_text (w->jump_button, "Jump to Latest");
  gtk_widget_set_halign (w->jump_button, GTK_ALIGN_END);
  gtk_widget_set_valign (w->jump_button, GTK_ALIGN_END);
  gtk_widget_set_margin_end (w->jump_button, 18);
  gtk_widget_set_margin_bottom (w->jump_button, 18);
  gtk_widget_set_visible (w->jump_button, FALSE);
  g_signal_connect (w->jump_button, "clicked", G_CALLBACK (on_jump_clicked), w);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), w->jump_button);
  gtk_box_append (GTK_BOX (chat), overlay);

  /* typing indicator + edit bar + composer */
  w->typing_label = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (w->typing_label), 0);
  gtk_widget_add_css_class (w->typing_label, "typing-label");
  gtk_widget_add_css_class (w->typing_label, "caption");
  gtk_widget_add_css_class (w->typing_label, "dim-label");
  gtk_box_append (GTK_BOX (bottom), w->typing_label);

  gtk_widget_add_css_class (edit_box, "edit-bar");
  gtk_box_append (GTK_BOX (edit_box), gtk_image_new_from_icon_name ("document-edit-symbolic"));
  gtk_label_set_xalign (GTK_LABEL (edit_label), 0);
  gtk_widget_set_hexpand (edit_label, TRUE);
  gtk_box_append (GTK_BOX (edit_box), edit_label);
  gtk_widget_add_css_class (edit_cancel, "flat");
  gtk_widget_add_css_class (edit_cancel, "circular");
  g_signal_connect (edit_cancel, "clicked", G_CALLBACK (on_cancel_edit), w);
  gtk_box_append (GTK_BOX (edit_box), edit_cancel);
  w->edit_bar = GTK_REVEALER (gtk_revealer_new ());
  gtk_revealer_set_child (w->edit_bar, edit_box);
  gtk_box_append (GTK_BOX (bottom), GTK_WIDGET (w->edit_bar));

  gtk_box_append (GTK_BOX (bottom),
                  composer_row (w, &w->composer, G_CALLBACK (on_attach_clicked),
                                G_CALLBACK (on_send_clicked), &w->send_button));
  g_signal_connect (w->composer, "changed", G_CALLBACK (on_composer_changed), w);
  gtk_widget_set_sensitive (w->send_button, FALSE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_composer_key), w);
  gtk_widget_add_controller (GTK_WIDGET (w->composer), keys);
  gtk_box_append (GTK_BOX (chat), bottom);

  w->content_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_add_named (w->content_stack, empty, "empty");
  gtk_stack_add_named (w->content_stack, chat, "chat");
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), GTK_WIDGET (w->content_stack));

  /* right-hand column: file preview / thread / planner */
  w->side_stack = GTK_STACK (gtk_stack_new ());
  w->preview = rc_preview_new (G_CALLBACK (close_side), w);
  gtk_stack_add_named (w->side_stack, w->preview, "preview");
  gtk_stack_add_named (w->side_stack, build_thread_panel (w), "thread");
  gtk_stack_add_named (w->side_stack, rc_planner_get_widget (w->planner), "planner");

  w->side_split = ADW_OVERLAY_SPLIT_VIEW (adw_overlay_split_view_new ());
  adw_overlay_split_view_set_sidebar_position (w->side_split, GTK_PACK_END);
  adw_overlay_split_view_set_min_sidebar_width (w->side_split, 320);
  adw_overlay_split_view_set_max_sidebar_width (w->side_split, 560);
  adw_overlay_split_view_set_sidebar_width_fraction (w->side_split, 0.42);
  adw_overlay_split_view_set_show_sidebar (w->side_split, FALSE);
  adw_overlay_split_view_set_content (w->side_split, tv);
  adw_overlay_split_view_set_sidebar (w->side_split, GTK_WIDGET (w->side_stack));

  w->content_page = adw_navigation_page_new (GTK_WIDGET (w->side_split), "Chat");
  return GTK_WIDGET (w->content_page);
}

static GtkWidget *
build_loading_page (void)
{
  GtkWidget *tv = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *spinner = gtk_spinner_new ();

  adw_header_bar_set_show_title (ADW_HEADER_BAR (header), FALSE);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);
  gtk_spinner_start (GTK_SPINNER (spinner));
  gtk_widget_set_size_request (spinner, 32, 32);
  gtk_widget_set_halign (spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (spinner, GTK_ALIGN_CENTER);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), spinner);
  return tv;
}

static gboolean
on_close_request (GtkWindow *window,
                  Win       *w)
{
  GVariant *state = g_action_group_get_action_state (G_ACTION_GROUP (window), "background");
  gboolean bg = state && g_variant_get_boolean (state);
  gboolean logged_in = g_strcmp0 (gtk_stack_get_visible_child_name (w->stack), "main") == 0;

  if (state)
    g_variant_unref (state);
  if (bg && (logged_in || rc_planner_has_pending (w->planner)))
    {
      gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
      if (!w->background_notice_sent)
        {
          send_notification (w, "background", "Rocket Client is still running",
                             "You'll keep getting messages and reminders. Use Quit (Ctrl+Q) to exit.", NULL);
          w->background_notice_sent = TRUE;
        }
      return TRUE;
    }
  return FALSE;
}

static void
win_free (Win *w)
{
  rc_client_shutdown (w->client);
  g_clear_handle_id (&w->subs_reload_id, g_source_remove);
  g_clear_handle_id (&w->typing_tick_id, g_source_remove);
  g_clear_handle_id (&w->typing_stop_id, g_source_remove);
  rc_planner_free (w->planner);
  rc_client_unref (w->client);
  msg_list_free (w->main_list);
  msg_list_free (w->thread_list);
  g_hash_table_unref (w->rooms);
  g_hash_table_unref (w->avatar_cache);
  g_hash_table_unref (w->avatar_waiting);
  g_hash_table_unref (w->typing);
  g_free (w->current_rid);
  g_free (w->thread_root);
  g_free (w->editing_id);
  g_free (w->me_username);
  g_free (w);
}

static void
cache_value_free (gpointer p)
{
  if (p)
    g_object_unref (p);
}

void
rc_window_open_room (GtkWindow  *window,
                     const char *rid)
{
  Win *w = g_object_get_data (G_OBJECT (window), "rc-win");
  Room *r;

  gtk_window_present (window);
  if (w && rid && (r = g_hash_table_lookup (w->rooms, rid)))
    open_room (w, r);
}

GtkWindow *
rc_window_new (AdwApplication *app)
{
  static const GActionEntry actions[] = {
    { .name = "logout", .activate = action_logout },
    { .name = "refresh", .activate = action_refresh },
    { .name = "attach", .activate = action_attach },
    { .name = "planner", .activate = action_planner, .parameter_type = "s" },
    { .name = "background", .state = "true", .change_state = action_background },
  };
  Win *w = g_new0 (Win, 1);
  GtkWidget *window = adw_application_window_new (GTK_APPLICATION (app));
  g_autofree char *trust = config_path ("trusted-certs.ini");
  g_autofree char *planner_file = g_build_filename (g_get_user_data_dir (), "rocket-client", "planner.json", NULL);
  AdwBreakpoint *bp;
  GAction *bg;

  w->window = GTK_WINDOW (window);
  w->client = rc_client_new ();
  rc_client_set_trust_file (w->client, trust);
  w->planner = rc_planner_new (planner_file, on_planner_alert, on_planner_open_room,
                               G_CALLBACK (close_side), w);
  w->rooms = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) room_free);
  w->avatar_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, cache_value_free);
  w->avatar_waiting = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify) g_ptr_array_unref);
  w->typing = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  w->stick_bottom = TRUE;
  w->restore_from_bottom = -1;
  w->typing_tick_id = g_timeout_add_seconds (1, typing_tick, w);

  gtk_window_set_title (w->window, "Rocket Client");
  gtk_window_set_icon_name (w->window, APP_ID);
  gtk_window_set_default_size (w->window, 1100, 700);
  gtk_widget_set_size_request (window, 360, 400);
  g_action_map_add_action_entries (G_ACTION_MAP (window), actions, G_N_ELEMENTS (actions), w);
  bg = g_action_map_lookup_action (G_ACTION_MAP (window), "background");
  g_simple_action_set_state (G_SIMPLE_ACTION (bg), g_variant_new_boolean (pref_get_bool ("background", TRUE)));
  g_signal_connect (window, "close-request", G_CALLBACK (on_close_request), w);

  w->split = ADW_NAVIGATION_SPLIT_VIEW (adw_navigation_split_view_new ());
  adw_navigation_split_view_set_min_sidebar_width (w->split, 240);
  adw_navigation_split_view_set_max_sidebar_width (w->split, 300);
  adw_navigation_split_view_set_sidebar (w->split, ADW_NAVIGATION_PAGE (build_sidebar (w)));
  adw_navigation_split_view_set_content (w->split, ADW_NAVIGATION_PAGE (build_content (w)));

  w->stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (w->stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named (w->stack, build_loading_page (), "loading");
  gtk_stack_add_named (w->stack, build_login_page (w), "login");
  gtk_stack_add_named (w->stack, GTK_WIDGET (w->split), "main");

  w->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (w->toasts, GTK_WIDGET (w->stack));
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (window), GTK_WIDGET (w->toasts));

  /* medium width: side column overlays the chat; narrow: everything collapses */
  bp = adw_breakpoint_new (adw_breakpoint_condition_parse ("max-width: 1000sp"));
  adw_breakpoint_add_setters (bp, G_OBJECT (w->side_split), "collapsed", TRUE, NULL);
  adw_application_window_add_breakpoint (ADW_APPLICATION_WINDOW (window), bp);
  bp = adw_breakpoint_new (adw_breakpoint_condition_parse ("max-width: 600sp"));
  adw_breakpoint_add_setters (bp,
                              G_OBJECT (w->side_split), "collapsed", TRUE,
                              G_OBJECT (w->split), "collapsed", TRUE,
                              NULL);
  adw_application_window_add_breakpoint (ADW_APPLICATION_WINDOW (window), bp);

  g_object_set_data_full (G_OBJECT (window), "rc-win", w, (GDestroyNotify) win_free);

  try_resume (w);
  return w->window;
}
