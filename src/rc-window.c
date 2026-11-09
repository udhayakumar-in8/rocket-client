/* rc-window.c - Rocket Client main window (GTK 4 + libadwaita) */

#include "config.h"
#include "rc-window.h"
#include "rc-client.h"

#include <string.h>
#include <glib/gstdio.h>

#define GROUP_WINDOW_MS (5 * 60 * 1000)
#define HISTORY_COUNT   50

enum { SECTION_FAVORITES, SECTION_PEOPLE, SECTION_ROOMS };

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

typedef struct {
  GtkWindow            *window;
  RcClient             *client;
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
  AdwNavigationPage    *content_page;
  AdwWindowTitle       *sidebar_title;
  GtkListBox           *room_list;
  GtkSearchBar         *search_bar;
  GtkEditable          *search_entry;
  AdwWindowTitle       *room_title;
  AdwBanner            *banner;
  GtkStack             *content_stack;
  GtkWidget            *msg_box;
  GtkScrolledWindow    *msg_scroller;
  GtkWidget            *jump_button;
  GtkEntry             *composer;
  GtkWidget            *send_button;

  /* state */
  GHashTable           *rooms;        /* rid -> Room* */
  char                 *current_rid;
  GHashTable           *msg_labels;   /* message id -> GtkLabel* (current room) */
  char                 *last_user;
  gint64                last_ts;
  gboolean              stick_bottom;
  char                 *me_username;
  GHashTable           *avatar_cache;   /* key -> GdkTexture* or NULL */
  GHashTable           *avatar_waiting; /* key -> GPtrArray<AdwAvatar> */
  guint                 subs_reload_id;
  gboolean              rebuilding;
} Win;

static void load_subscriptions (Win *w);
static void show_login (Win *w);

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

static void
toast (Win *w, const char *fmt, ...) G_GNUC_PRINTF (2, 3);

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
  if (o == NULL || !json_object_has_member (o, member))
    return NULL;
  return json_object_get_string_member_with_default (o, member, NULL);
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
    {
      JsonNode *d = json_object_get_member (json_node_get_object (n), "$date");
      if (d && JSON_NODE_HOLDS_VALUE (d))
        {
          if (json_node_get_value_type (d) == G_TYPE_INT64)
            return json_node_get_int (d);
          return (gint64) json_node_get_double (d);
        }
    }
  return 0;
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

/* Rocket.Chat-ish markdown (*bold*, _italic_, ~strike~, `code`, links) -> Pango */
static char *
format_inline (const char *text)
{
  static GRegex *re_code, *re_bold, *re_ital, *re_strike;
  g_autofree char *s0 = g_markup_escape_text (text, -1);
  g_autofree char *s1 = NULL, *s2 = NULL, *s3 = NULL;

  if (g_once_init_enter_pointer (&re_code))
    {
      re_bold = g_regex_new ("(?<![\\w*])\\*([^*\\n]+)\\*(?![\\w*])", G_REGEX_OPTIMIZE, 0, NULL);
      re_ital = g_regex_new ("(?<![\\w_])_([^_\\n]+)_(?![\\w_])", G_REGEX_OPTIMIZE, 0, NULL);
      re_strike = g_regex_new ("(?<![\\w~])~([^~\\n]+)~(?![\\w~])", G_REGEX_OPTIMIZE, 0, NULL);
      g_once_init_leave_pointer (&re_code, g_regex_new ("`([^`\\n]+)`", G_REGEX_OPTIMIZE, 0, NULL));
    }

  s1 = g_regex_replace (re_code, s0, -1, 0, "<tt>\\1</tt>", 0, NULL);
  s2 = g_regex_replace (re_bold, s1, -1, 0, "<b>\\1</b>", 0, NULL);
  s3 = g_regex_replace (re_ital, s2, -1, 0, "<i>\\1</i>", 0, NULL);
  return g_regex_replace (re_strike, s3, -1, 0, "<s>\\1</s>", 0, NULL);
}

static char *
format_markup (const char *text)
{
  static GRegex *re_url;
  g_autoptr(GMatchInfo) mi = NULL;
  GString *out = g_string_new (NULL);
  GString *check = g_string_new (NULL); /* same markup minus <a>, which Pango can't parse */
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

  /* fall back to plain escaped text if our naive formatting produced bad markup */
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
/* session persistence (~/.config/rocket-client/session.ini, mode 0600)      */

static char *
session_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "rocket-client", "session.ini", NULL);
}

static GKeyFile *
session_load (void)
{
  g_autofree char *path = session_path ();
  GKeyFile *kf = g_key_file_new ();
  g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL);
  return kf;
}

static void
session_save (GKeyFile *kf)
{
  g_autofree char *path = session_path ();
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


/* ------------------------------------------------------------------------ */
/* avatars                                                                   */

typedef struct {
  Win  *w;
  char *key;
} AvatarReq;

static void
on_avatar_bytes (RcClient     *client,
                 GBytes       *bytes,
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

/* ------------------------------------------------------------------------ */
/* messages                                                                  */

static void
clear_messages (Win *w)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (w->msg_box)) != NULL)
    gtk_box_remove (GTK_BOX (w->msg_box), child);
  g_hash_table_remove_all (w->msg_labels);
  g_clear_pointer (&w->last_user, g_free);
  w->last_ts = 0;
}

static char *
message_text (JsonObject *m)
{
  const char *msg = jstr (m, "msg");
  GString *s = g_string_new (msg ? msg : "");
  JsonArray *atts = json_object_has_member (m, "attachments") &&
                    JSON_NODE_HOLDS_ARRAY (json_object_get_member (m, "attachments"))
                    ? json_object_get_array_member (m, "attachments") : NULL;

  for (guint i = 0; atts && i < json_array_get_length (atts); i++)
    {
      JsonNode *n = json_array_get_element (atts, i);
      JsonObject *a;
      const char *title, *desc;

      if (!JSON_NODE_HOLDS_OBJECT (n))
        continue;
      a = json_node_get_object (n);
      title = jstr (a, "title");
      desc = jstr (a, "description");
      if (!title)
        title = jstr (a, "text");
      if (s->len)
        g_string_append_c (s, '\n');
      g_string_append_printf (s, "📎 %s", title ? title : "Attachment");
      if (desc && *desc)
        g_string_append_printf (s, " — %s", desc);
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
  };
  for (guint i = 0; i < G_N_ELEMENTS (map); i++)
    if (g_str_equal (map[i].t, t))
      return map[i].text;
  return NULL;
}

static void
append_message (Win        *w,
                JsonObject *m)
{
  const char *id = jstr (m, "_id");
  const char *type = jstr (m, "t");
  JsonObject *u = json_object_has_member (m, "u") ? json_object_get_object_member (m, "u") : NULL;
  const char *username = jstr (u, "username");
  const char *name = jstr (u, "name");
  const char *display = (name && *name) ? name : (username ? username : "Unknown");
  gint64 ts = parse_ts_ms (m, "ts");
  g_autofree char *text = message_text (m);
  g_autofree char *markup = NULL;
  GtkWidget *label;

  if (id == NULL)
    return;

  /* edited message we already show */
  label = g_hash_table_lookup (w->msg_labels, id);
  if (label)
    {
      if (!type)
        {
          markup = format_markup (text);
          gtk_label_set_markup (GTK_LABEL (label), markup);
        }
      return;
    }

  if (type && *type)
    {
      const char *act = system_action (type);
      g_autofree char *line = NULL;
      GtkWidget *sys;

      if (act == NULL)
        return; /* unknown system/event message: hide */
      line = g_strdup_printf ("%s %s%s%s", display, act, *text ? ": " : "", text);
      sys = gtk_label_new (line);
      gtk_label_set_wrap (GTK_LABEL (sys), TRUE);
      gtk_widget_add_css_class (sys, "dim-label");
      gtk_widget_add_css_class (sys, "caption");
      gtk_widget_add_css_class (sys, "system-message");
      gtk_box_append (GTK_BOX (w->msg_box), sys);
      g_hash_table_insert (w->msg_labels, g_strdup (id), sys);
      g_clear_pointer (&w->last_user, g_free);
      return;
    }

  markup = format_markup (text);
  label = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (label), markup);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  gtk_widget_set_focusable (label, FALSE);
  gtk_widget_add_css_class (label, "msg-body");

  if (w->last_user && username && g_str_equal (w->last_user, username) &&
      ts - w->last_ts < GROUP_WINDOW_MS && ts >= w->last_ts)
    {
      /* continuation of the previous block */
      gtk_widget_set_margin_start (label, 36 + 12);
      gtk_widget_add_css_class (label, "msg-continued");
      gtk_box_append (GTK_BOX (w->msg_box), label);
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
      if (username && g_strcmp0 (username, w->me_username) == 0)
        gtk_widget_add_css_class (author, "msg-author-me");
      gtk_box_append (GTK_BOX (head), author);

      gtk_widget_add_css_class (time_label, "dim-label");
      gtk_widget_add_css_class (time_label, "caption");
      gtk_box_append (GTK_BOX (head), time_label);

      gtk_box_append (GTK_BOX (col), head);
      gtk_box_append (GTK_BOX (col), label);
      gtk_widget_set_hexpand (col, TRUE);
      gtk_box_append (GTK_BOX (row), col);
      gtk_box_append (GTK_BOX (w->msg_box), row);
    }

  g_hash_table_insert (w->msg_labels, g_strdup (id), label);
  g_free (w->last_user);
  w->last_user = g_strdup (username);
  w->last_ts = ts;
}

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
  double max = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);

  w->stick_bottom = v >= max - 40;
  gtk_widget_set_visible (w->jump_button, !w->stick_bottom);
}

static void
on_adj_changed (GtkAdjustment *adj,
                Win           *w)
{
  if (w->stick_bottom)
    gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));
}

static void
on_jump_clicked (GtkButton *b,
                 Win       *w)
{
  scroll_to_bottom (w);
}

/* ------------------------------------------------------------------------ */
/* room opening / history                                                    */

static void
ignore_reply (RcClient     *client,
              JsonObject   *result,
              const GError *error,
              gpointer      data)
{
}

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
  Win  *w;
  char *rid;
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
on_history (RcClient     *client,
            JsonObject   *result,
            const GError *error,
            gpointer      data)
{
  RoomReq *q = data;
  Win *w = q->w;
  JsonArray *msgs;

  if (rc_error_is_cancelled (error) || g_strcmp0 (q->rid, w->current_rid) != 0)
    goto out;

  if (error)
    {
      toast (w, "Could not load messages: %s", error->message);
      goto out;
    }

  msgs = json_object_has_member (result, "messages") ? json_object_get_array_member (result, "messages") : NULL;
  clear_messages (w);
  /* API returns newest first */
  for (gint i = msgs ? (gint) json_array_get_length (msgs) - 1 : -1; i >= 0; i--)
    {
      JsonNode *n = json_array_get_element (msgs, i);
      if (JSON_NODE_HOLDS_OBJECT (n))
        append_message (w, json_node_get_object (n));
    }
  scroll_to_bottom (w);

out:
  room_req_free (q);
}

static void
on_room_info (RcClient     *client,
              JsonObject   *result,
              const GError *error,
              gpointer      data)
{
  RoomReq *q = data;
  Win *w = q->w;
  JsonObject *room;
  const char *topic;

  if (rc_error_is_cancelled (error) || error || g_strcmp0 (q->rid, w->current_rid) != 0)
    goto out;

  room = json_object_has_member (result, "room") ? json_object_get_object_member (result, "room") : NULL;
  topic = jstr (room, "topic");
  if (topic && *topic)
    adw_window_title_set_subtitle (w->room_title, topic);

out:
  room_req_free (q);
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
  clear_messages (w);

  rc_client_get (w->client, hist, on_history, room_req_new (w, r->rid));
  rc_client_get (w->client, info, on_room_info, room_req_new (w, r->rid));
  mark_read (w, r);

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

  subs = json_object_has_member (result, "update") ? json_object_get_array_member (result, "update") : NULL;

  w->rebuilding = TRUE;
  gtk_list_box_remove_all (w->room_list);
  g_hash_table_remove_all (w->rooms);

  for (guint i = 0; subs && i < json_array_get_length (subs); i++)
    {
      JsonNode *n = json_array_get_element (subs, i);
      JsonObject *s;
      Room *r;

      if (!JSON_NODE_HOLDS_OBJECT (n))
        continue;
      s = json_node_get_object (n);
      if (!jstr (s, "rid") || !json_object_get_boolean_member_with_default (s, "open", TRUE))
        continue;

      r = g_new0 (Room, 1);
      r->rid = g_strdup (jstr (s, "rid"));
      r->name = g_strdup (jstr (s, "name"));
      r->fname = g_strdup (jstr (s, "fname"));
      r->type = g_strdup (jstr (s, "t"));
      r->unread = (int) json_object_get_int_member_with_default (s, "unread", 0);
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
/* realtime                                                                  */

static gboolean
message_mentions_me (Win        *w,
                     JsonObject *m)
{
  JsonArray *mentions;

  if (!json_object_has_member (m, "mentions") ||
      !JSON_NODE_HOLDS_ARRAY (json_object_get_member (m, "mentions")))
    return FALSE;
  mentions = json_object_get_array_member (m, "mentions");
  for (guint i = 0; i < json_array_get_length (mentions); i++)
    {
      JsonNode *n = json_array_get_element (mentions, i);
      const char *un;
      if (!JSON_NODE_HOLDS_OBJECT (n))
        continue;
      un = jstr (json_node_get_object (n), "username");
      if (un && (g_strcmp0 (un, w->me_username) == 0 || g_str_equal (un, "all") || g_str_equal (un, "here")))
        return TRUE;
    }
  return FALSE;
}

static void
notify_message (Win        *w,
                Room       *room,
                JsonObject *m,
                const char *author)
{
  g_autoptr(GNotification) n = NULL;
  g_autofree char *title = NULL;
  g_autofree char *body = message_text (m);
  GApplication *app = G_APPLICATION (gtk_window_get_application (w->window));

  if (!app)
    return;
  if (g_strcmp0 (room->type, "d") == 0)
    title = g_strdup (author);
  else
    title = g_strdup_printf ("%s in %s", author, room_display_name (room));
  n = g_notification_new (title);
  g_notification_set_body (n, body);
  g_application_send_notification (app, room->rid, n);
}

static void
on_realtime (RcClient   *client,
             const char *event,
             JsonObject *payload,
             gpointer    data)
{
  Win *w = data;

  if (g_str_equal (event, "connected"))
    {
      adw_banner_set_revealed (w->banner, FALSE);
    }
  else if (g_str_equal (event, "disconnected"))
    {
      adw_banner_set_revealed (w->banner, TRUE);
    }
  else if (g_str_equal (event, "subscriptions-changed"))
    {
      schedule_subs_reload (w);
    }
  else if (g_str_equal (event, "message") && payload)
    {
      const char *rid = jstr (payload, "rid");
      JsonObject *u = json_object_has_member (payload, "u") ? json_object_get_object_member (payload, "u") : NULL;
      const char *un = jstr (u, "username");
      const char *nm = jstr (u, "name");
      gboolean from_me = un && g_strcmp0 (un, w->me_username) == 0;
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

      if (g_strcmp0 (rid, w->current_rid) == 0)
        {
          append_message (w, payload);
          if (active)
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
/* composer                                                                  */

typedef struct {
  Win  *w;
  char *text;
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
      if (*gtk_editable_get_text (GTK_EDITABLE (w->composer)) == '\0')
        gtk_editable_set_text (GTK_EDITABLE (w->composer), q->text);
    }
  else if (json_object_has_member (result, "message"))
    {
      JsonObject *m = json_object_get_object_member (result, "message");
      if (g_strcmp0 (jstr (m, "rid"), w->current_rid) == 0)
        {
          append_message (w, m);
          scroll_to_bottom (w);
        }
    }

out:
  g_free (q->text);
  g_free (q);
}

static void
send_current (Win *w)
{
  g_autofree char *text = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (w->composer))));
  g_autoptr(JsonBuilder) b = NULL;
  SendReq *q;

  if (*text == '\0' || w->current_rid == NULL)
    return;

  b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "message");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "rid");
  json_builder_add_string_value (b, w->current_rid);
  json_builder_set_member_name (b, "msg");
  json_builder_add_string_value (b, text);
  json_builder_end_object (b);
  json_builder_end_object (b);

  q = g_new0 (SendReq, 1);
  q->w = w;
  q->text = g_strdup (text);
  gtk_editable_set_text (GTK_EDITABLE (w->composer), "");
  w->stick_bottom = TRUE;
  rc_client_post (w->client, "/api/v1/chat.sendMessage", json_builder_get_root (b), on_sent, q);
}

static void
on_composer_activate (GtkEntry *e,
                      Win      *w)
{
  send_current (w);
}

static void
on_send_clicked (GtkButton *b,
                 Win       *w)
{
  send_current (w);
}

static void
on_composer_changed (GtkEditable *e,
                     Win         *w)
{
  gtk_widget_set_sensitive (w->send_button, *gtk_editable_get_text (e) != '\0');
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
  JsonObject *d, *me;

  if (rc_error_is_cancelled (error))
    return;
  set_login_busy (w, FALSE);

  if (error)
    {
      g_autofree char *msg = NULL;
      if (g_error_matches (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_API) &&
          strstr (error->message, "totp"))
        msg = g_strdup ("Two-factor code required – enter it in the 2FA field");
      else
        msg = g_strdup_printf ("Sign in failed: %s", error->message);
      toast (w, "%s", msg);
      return;
    }

  d = json_object_get_object_member (result, "data");
  me = d && json_object_has_member (d, "me") ? json_object_get_object_member (d, "me") : NULL;
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
  if (server)
    gtk_editable_set_text (GTK_EDITABLE (w->server_row), server);
  if (user)
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
  session_forget ();

  g_clear_handle_id (&w->subs_reload_id, g_source_remove);
  g_clear_pointer (&w->current_rid, g_free);
  g_clear_pointer (&w->me_username, g_free);
  w->rebuilding = TRUE;
  gtk_list_box_remove_all (w->room_list);
  w->rebuilding = FALSE;
  g_hash_table_remove_all (w->rooms);
  clear_messages (w);

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
  const char *modes[] = {
    "Auto (443 → 80)",
    "HTTPS (443)",
    "HTTP (80)",
    NULL
  };

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
  gtk_editable_set_text (GTK_EDITABLE (w->server_row), "");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (server_group), GTK_WIDGET (w->server_row));

  w->mode_row = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (w->mode_row), "Connection");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (w->mode_row), "Auto tries HTTPS first, then plain HTTP");
  adw_combo_row_set_model (w->mode_row, G_LIST_MODEL (gtk_string_list_new (modes)));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (server_group), GTK_WIDGET (w->mode_row));
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
  gtk_editable_set_max_width_chars (GTK_EDITABLE (w->code_row), 8);
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
  GMenu *menu = g_menu_new ();
  GtkWidget *page;

  w->sidebar_title = ADW_WINDOW_TITLE (adw_window_title_new ("Rocket Client", ""));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (w->sidebar_title));

  g_menu_append (menu, "_Refresh", "win.refresh");
  g_menu_append (menu, "_Log Out", "win.logout");
  g_menu_append (menu, "_About Rocket Client", "app.about");
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_btn), "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_btn), G_MENU_MODEL (menu));
  gtk_widget_set_tooltip_text (menu_btn, "Main Menu");
  g_object_unref (menu);
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

  page = GTK_WIDGET (adw_navigation_page_new (tv, "Rooms"));
  return page;
}

static GtkWidget *
build_content (Win *w)
{
  GtkWidget *tv = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *empty = adw_status_page_new ();
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *clamp = adw_clamp_new ();
  GtkWidget *composer_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *chat = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkAdjustment *adj;

  w->room_title = ADW_WINDOW_TITLE (adw_window_title_new ("", ""));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (w->room_title));
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);

  w->banner = ADW_BANNER (adw_banner_new ("Connection lost – reconnecting…"));
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), GTK_WIDGET (w->banner));

  /* empty state */
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty), "chat-message-new-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (empty), "No Room Selected");
  adw_status_page_set_description (ADW_STATUS_PAGE (empty), "Pick a room or a person from the list");

  /* message list */
  w->msg_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (w->msg_box, "message-list");
  gtk_widget_set_valign (w->msg_box, GTK_ALIGN_END);
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 900);
  adw_clamp_set_tightening_threshold (ADW_CLAMP (clamp), 600);
  adw_clamp_set_child (ADW_CLAMP (clamp), w->msg_box);
  w->msg_scroller = GTK_SCROLLED_WINDOW (scroller);
  gtk_scrolled_window_set_policy (w->msg_scroller, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (w->msg_scroller, clamp);
  gtk_widget_set_vexpand (scroller, TRUE);
  adj = gtk_scrolled_window_get_vadjustment (w->msg_scroller);
  g_signal_connect (adj, "value-changed", G_CALLBACK (on_adj_value_changed), w);
  g_signal_connect (adj, "changed", G_CALLBACK (on_adj_changed), w);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), scroller);

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

  /* composer */
  gtk_widget_add_css_class (composer_box, "composer");
  w->composer = GTK_ENTRY (gtk_entry_new ());
  gtk_entry_set_placeholder_text (w->composer, "Send a message…");
  gtk_widget_set_hexpand (GTK_WIDGET (w->composer), TRUE);
  g_signal_connect (w->composer, "activate", G_CALLBACK (on_composer_activate), w);
  g_signal_connect (w->composer, "changed", G_CALLBACK (on_composer_changed), w);
  gtk_box_append (GTK_BOX (composer_box), GTK_WIDGET (w->composer));

  w->send_button = gtk_button_new_from_icon_name ("document-send-symbolic");
  gtk_widget_add_css_class (w->send_button, "circular");
  gtk_widget_add_css_class (w->send_button, "suggested-action");
  gtk_widget_set_tooltip_text (w->send_button, "Send");
  gtk_widget_set_sensitive (w->send_button, FALSE);
  g_signal_connect (w->send_button, "clicked", G_CALLBACK (on_send_clicked), w);
  gtk_box_append (GTK_BOX (composer_box), w->send_button);
  gtk_box_append (GTK_BOX (chat), composer_box);

  w->content_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_add_named (w->content_stack, empty, "empty");
  gtk_stack_add_named (w->content_stack, chat, "chat");
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), GTK_WIDGET (w->content_stack));

  w->content_page = adw_navigation_page_new (tv, "Chat");
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

static void
win_free (Win *w)
{
  rc_client_shutdown (w->client);
  rc_client_unref (w->client);
  g_clear_handle_id (&w->subs_reload_id, g_source_remove);
  g_hash_table_unref (w->rooms);
  g_hash_table_unref (w->msg_labels);
  g_hash_table_unref (w->avatar_cache);
  g_hash_table_unref (w->avatar_waiting);
  g_free (w->current_rid);
  g_free (w->last_user);
  g_free (w->me_username);
  g_free (w);
}

static void
cache_value_free (gpointer p)
{
  if (p)
    g_object_unref (p);
}

GtkWindow *
rc_window_new (AdwApplication *app)
{
  static const GActionEntry actions[] = {
    { .name = "logout", .activate = action_logout },
    { .name = "refresh", .activate = action_refresh },
  };
  Win *w = g_new0 (Win, 1);
  GtkWidget *window = adw_application_window_new (GTK_APPLICATION (app));
  AdwBreakpoint *bp;

  w->window = GTK_WINDOW (window);
  w->client = rc_client_new ();
  w->rooms = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) room_free);
  w->msg_labels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  w->avatar_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, cache_value_free);
  w->avatar_waiting = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify) g_ptr_array_unref);
  w->stick_bottom = TRUE;

  gtk_window_set_title (w->window, "Rocket Client");
  gtk_window_set_icon_name (w->window, APP_ID);
  gtk_window_set_default_size (w->window, 960, 640);
  gtk_widget_set_size_request (window, 360, 400);
  g_action_map_add_action_entries (G_ACTION_MAP (window), actions, G_N_ELEMENTS (actions), w);

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

  bp = adw_breakpoint_new (adw_breakpoint_condition_parse ("max-width: 600sp"));
  adw_breakpoint_add_setters (bp, G_OBJECT (w->split), "collapsed", TRUE, NULL);
  adw_application_window_add_breakpoint (ADW_APPLICATION_WINDOW (window), bp);

  g_object_set_data_full (G_OBJECT (window), "rc-win", w, (GDestroyNotify) win_free);

  try_resume (w);
  return w->window;
}
