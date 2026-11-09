/* rc-planner.c - tasks, timer reminders and calendar */

#include "rc-planner.h"
#include <string.h>

typedef enum { ITEM_TASK, ITEM_REMINDER, ITEM_EVENT } ItemKind;

typedef struct {
  ItemKind kind;
  char    *id;
  char    *title;
  gint64   at;            /* unix seconds; 0 = none */
  int      remind_before; /* events: minutes before start, -1 = none */
  gboolean done;          /* tasks */
  gboolean fired;
  char    *rid;
  char    *msg_id;
  char    *server_id;     /* event synced to Rocket.Chat calendar */
} Item;

struct _RcPlanner {
  char             *file;
  GPtrArray        *items;
  RcPlannerAlert    alert;
  RcPlannerOpenRoom open_room;
  GCallback         close_cb;
  gpointer          data;
  RcClient         *client;
  gboolean          server_calendar;
  guint             tick_id;
  guint             ui_tick_id;

  GtkWidget        *widget;
  GtkStack         *stack;
  /* tasks */
  GtkEntry         *task_entry;
  GtkMenuButton    *task_due_button;
  gint64            task_due;
  GtkListBox       *task_list;
  /* reminders */
  GtkEntry         *rem_entry;
  GtkListBox       *rem_list;
  GPtrArray        *countdowns; /* GtkLabel* with "item" data */
  /* calendar */
  GtkCalendar      *calendar;
  GtkLabel         *day_label;
  GtkListBox       *event_list;
  GtkEntry         *event_entry;
  GtkSpinButton    *event_hour;
  GtkSpinButton    *event_min;
  GtkDropDown      *event_remind;
  GtkSwitch        *event_sync;
  GtkWidget        *event_sync_row;
  guint             cal_serial;
};

static const int remind_choices[] = { -1, 0, 5, 15, 30, 60, 1440 };

static void refresh_all (RcPlanner *pl);
static void refresh_calendar (RcPlanner *pl);

/* ------------------------------------------------------------------------ */
/* items & persistence                                                       */

static void
item_free (Item *it)
{
  g_free (it->id);
  g_free (it->title);
  g_free (it->rid);
  g_free (it->msg_id);
  g_free (it->server_id);
  g_free (it);
}

static Item *
item_new (ItemKind kind, const char *title, gint64 at)
{
  Item *it = g_new0 (Item, 1);
  it->kind = kind;
  it->id = g_uuid_string_random ();
  it->title = g_strdup (title);
  it->at = at;
  it->remind_before = -1;
  return it;
}

static gint64
fire_time (Item *it)
{
  if (it->at <= 0)
    return 0;
  if (it->kind == ITEM_EVENT)
    return it->remind_before < 0 ? 0 : it->at - (gint64) it->remind_before * 60;
  return it->at;
}

static void
save (RcPlanner *pl)
{
  g_autoptr(JsonBuilder) b = json_builder_new ();
  g_autoptr(JsonNode) root = NULL;
  g_autofree char *data = NULL;
  g_autofree char *dir = g_path_get_dirname (pl->file);

  json_builder_begin_object (b);
  json_builder_set_member_name (b, "version");
  json_builder_add_int_value (b, 1);
  json_builder_set_member_name (b, "items");
  json_builder_begin_array (b);
  for (guint i = 0; i < pl->items->len; i++)
    {
      Item *it = g_ptr_array_index (pl->items, i);
      json_builder_begin_object (b);
#define S(k, v) if (v) { json_builder_set_member_name (b, k); json_builder_add_string_value (b, v); }
#define I(k, v) json_builder_set_member_name (b, k); json_builder_add_int_value (b, v);
      I ("kind", it->kind);
      S ("id", it->id);
      S ("title", it->title);
      I ("at", it->at);
      I ("remind_before", it->remind_before);
      I ("done", it->done);
      I ("fired", it->fired);
      S ("rid", it->rid);
      S ("msg_id", it->msg_id);
      S ("server_id", it->server_id);
#undef S
#undef I
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);

  root = json_builder_get_root (b);
  data = json_to_string (root, TRUE);
  g_mkdir_with_parents (dir, 0700);
  g_file_set_contents_full (pl->file, data, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
}

static void
load (RcPlanner *pl)
{
  g_autoptr(JsonParser) parser = json_parser_new ();
  JsonNode *root;
  JsonArray *arr;

  if (!json_parser_load_from_file (parser, pl->file, NULL))
    return;
  root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root) ||
      !json_object_has_member (json_node_get_object (root), "items"))
    return;
  arr = json_object_get_array_member (json_node_get_object (root), "items");
  for (guint i = 0; i < json_array_get_length (arr); i++)
    {
      JsonObject *o = json_array_get_object_element (arr, i);
      Item *it;

      if (o == NULL)
        continue;
      it = g_new0 (Item, 1);
      it->kind = json_object_get_int_member_with_default (o, "kind", ITEM_TASK);
      it->id = g_strdup (json_object_get_string_member_with_default (o, "id", NULL));
      if (it->id == NULL)
        it->id = g_uuid_string_random ();
      it->title = g_strdup (json_object_get_string_member_with_default (o, "title", ""));
      it->at = json_object_get_int_member_with_default (o, "at", 0);
      it->remind_before = json_object_get_int_member_with_default (o, "remind_before", -1);
      it->done = json_object_get_int_member_with_default (o, "done", 0);
      it->fired = json_object_get_int_member_with_default (o, "fired", 0);
      it->rid = g_strdup (json_object_get_string_member_with_default (o, "rid", NULL));
      it->msg_id = g_strdup (json_object_get_string_member_with_default (o, "msg_id", NULL));
      it->server_id = g_strdup (json_object_get_string_member_with_default (o, "server_id", NULL));
      g_ptr_array_add (pl->items, it);
    }
}

/* ------------------------------------------------------------------------ */
/* time helpers                                                              */

char *
rc_format_when (gint64 t)
{
  g_autoptr(GDateTime) dt = NULL;
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autoptr(GDateTime) tomorrow = g_date_time_add_days (now, 1);
  const char *fmt;

  if (t <= 0)
    return g_strdup ("");
  dt = g_date_time_new_from_unix_local (t);
  if (g_date_time_get_year (dt) == g_date_time_get_year (now) &&
      g_date_time_get_day_of_year (dt) == g_date_time_get_day_of_year (now))
    fmt = "Today %-I:%M %p";
  else if (g_date_time_get_year (dt) == g_date_time_get_year (tomorrow) &&
           g_date_time_get_day_of_year (dt) == g_date_time_get_day_of_year (tomorrow))
    fmt = "Tomorrow %-I:%M %p";
  else if (g_date_time_get_year (dt) == g_date_time_get_year (now))
    fmt = "%a %-d %b, %-I:%M %p";
  else
    fmt = "%-d %b %Y, %-I:%M %p";
  return g_date_time_format (dt, fmt);
}

static char *
format_countdown (gint64 secs)
{
  if (secs <= 0)
    return g_strdup ("now");
  if (secs < 3600)
    return g_strdup_printf ("in %dm %02ds", (int) (secs / 60), (int) (secs % 60));
  if (secs < 86400)
    return g_strdup_printf ("in %dh %02dm", (int) (secs / 3600), (int) (secs % 3600 / 60));
  return g_strdup_printf ("in %dd %dh", (int) (secs / 86400), (int) (secs % 86400 / 3600));
}

static gint64
now_unix (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

/* ------------------------------------------------------------------------ */
/* scheduler                                                                 */

static gboolean
tick (gpointer data)
{
  RcPlanner *pl = data;
  gint64 now = now_unix ();
  gboolean changed = FALSE;

  for (guint i = 0; i < pl->items->len; i++)
    {
      Item *it = g_ptr_array_index (pl->items, i);
      gint64 ft = fire_time (it);
      g_autofree char *when = NULL;
      g_autofree char *body = NULL;
      const char *title;

      if (it->fired || ft <= 0 || now < ft || (it->kind == ITEM_TASK && it->done))
        continue;

      it->fired = TRUE;
      changed = TRUE;
      when = rc_format_when (it->at);
      switch (it->kind)
        {
        case ITEM_TASK:
          title = "Task due";
          body = g_strdup_printf ("%s\nDue %s", it->title, when);
          break;
        case ITEM_EVENT:
          title = it->remind_before > 0 ? "Upcoming event" : "Event starting";
          body = g_strdup_printf ("%s\n%s", it->title, when);
          break;
        case ITEM_REMINDER:
        default:
          title = "Reminder";
          body = g_strdup (it->title);
          break;
        }
      /* reminders missed while the app was closed are still delivered */
      if (now - ft > 120)
        {
          char *tmp = g_strdup_printf ("%s\n(missed – was due %s)", body, when);
          g_free (body);
          body = tmp;
        }
      if (pl->alert)
        pl->alert (title, body, it->rid, pl->data);
    }

  if (changed)
    {
      save (pl);
      refresh_all (pl);
    }
  return G_SOURCE_CONTINUE;
}

static void
tick_once (gpointer data)
{
  tick (data);
}

static gboolean
ui_tick (gpointer data)
{
  RcPlanner *pl = data;
  gint64 now = now_unix ();

  if (!pl->widget || !gtk_widget_get_mapped (pl->widget))
    return G_SOURCE_CONTINUE;
  for (guint i = 0; i < pl->countdowns->len; i++)
    {
      GtkLabel *l = g_ptr_array_index (pl->countdowns, i);
      Item *it = g_object_get_data (G_OBJECT (l), "item");
      g_autofree char *s = format_countdown (it->at - now);
      gtk_label_set_text (l, s);
    }
  return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------------ */
/* date/time picker popover                                                  */

typedef struct {
  RcPickedTime   func;
  gpointer       data;
  GtkCalendar   *cal;
  GtkSpinButton *hour;
  GtkSpinButton *min;
} Picker;

static void
picker_emit (GtkWidget *any, Picker *pk, GDateTime *dt)
{
  GtkWidget *pop = gtk_widget_get_ancestor (any, GTK_TYPE_POPOVER);
  if (pop)
    gtk_popover_popdown (GTK_POPOVER (pop));
  pk->func (dt, pk->data);
}

static void
on_preset (GtkButton *b, Picker *pk)
{
  int kind = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "preset"));
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autoptr(GDateTime) dt = NULL;
  g_autoptr(GDateTime) day = NULL;

  switch (kind)
    {
    case 0: dt = g_date_time_add_minutes (now, 20); break;
    case 1: dt = g_date_time_add_hours (now, 1); break;
    case 2: dt = g_date_time_add_hours (now, 3); break;
    case 3:
      day = g_date_time_add_days (now, 1);
      dt = g_date_time_new_local (g_date_time_get_year (day), g_date_time_get_month (day),
                                  g_date_time_get_day_of_month (day), 9, 0, 0);
      break;
    case 4:
      day = g_date_time_add_days (now, 8 - g_date_time_get_day_of_week (now));
      dt = g_date_time_new_local (g_date_time_get_year (day), g_date_time_get_month (day),
                                  g_date_time_get_day_of_month (day), 9, 0, 0);
      break;
    default:
      break;
    }
  picker_emit (GTK_WIDGET (b), pk, dt);
}

static void
on_pick_set (GtkButton *b, Picker *pk)
{
  g_autoptr(GDateTime) d = gtk_calendar_get_date (pk->cal);
  g_autoptr(GDateTime) dt = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d),
                                                   g_date_time_get_day_of_month (d),
                                                   gtk_spin_button_get_value_as_int (pk->hour),
                                                   gtk_spin_button_get_value_as_int (pk->min), 0);
  picker_emit (GTK_WIDGET (b), pk, dt);
}

static void
on_pick_clear (GtkButton *b, Picker *pk)
{
  picker_emit (GTK_WIDGET (b), pk, NULL);
}

static gboolean
on_spin_output (GtkSpinButton *spin, gpointer data)
{
  g_autofree char *s = g_strdup_printf ("%02d", gtk_spin_button_get_value_as_int (spin));
  gtk_editable_set_text (GTK_EDITABLE (spin), s);
  return TRUE;
}

static GtkSpinButton *
time_spin (int max, int value)
{
  GtkWidget *s = gtk_spin_button_new_with_range (0, max, 1);
  gtk_spin_button_set_wrap (GTK_SPIN_BUTTON (s), TRUE);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (s), value);
  gtk_orientable_set_orientation (GTK_ORIENTABLE (s), GTK_ORIENTATION_VERTICAL);
  g_signal_connect (s, "output", G_CALLBACK (on_spin_output), NULL);
  return GTK_SPIN_BUTTON (s);
}

GtkWidget *
rc_datetime_popover_new (gboolean     allow_clear,
                         RcPickedTime func,
                         gpointer     user_data)
{
  static const char *presets[] = { "In 20 minutes", "In 1 hour", "In 3 hours",
                                   "Tomorrow at 9:00 AM", "Next Monday at 9:00 AM" };
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *time_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *btns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *set = gtk_button_new_with_label ("Set");
  g_autoptr(GDateTime) now = g_date_time_new_now_local ();
  g_autoptr(GDateTime) later = g_date_time_add_hours (now, 1);
  Picker *pk = g_new0 (Picker, 1);

  pk->func = func;
  pk->data = user_data;
  g_object_set_data_full (G_OBJECT (pop), "picker", pk, g_free);

  for (guint i = 0; i < G_N_ELEMENTS (presets); i++)
    {
      GtkWidget *b = gtk_button_new_with_label (presets[i]);
      gtk_widget_add_css_class (b, "flat");
      gtk_widget_set_halign (gtk_button_get_child (GTK_BUTTON (b)), GTK_ALIGN_START);
      g_object_set_data (G_OBJECT (b), "preset", GINT_TO_POINTER (i));
      g_signal_connect (b, "clicked", G_CALLBACK (on_preset), pk);
      gtk_box_append (GTK_BOX (box), b);
    }
  gtk_box_append (GTK_BOX (box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

  pk->cal = GTK_CALENDAR (gtk_calendar_new ());
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (pk->cal));

  pk->hour = time_spin (23, g_date_time_get_hour (later));
  pk->min = time_spin (59, 0);
  gtk_widget_set_halign (time_box, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (time_box), GTK_WIDGET (pk->hour));
  gtk_box_append (GTK_BOX (time_box), gtk_label_new (":"));
  gtk_box_append (GTK_BOX (time_box), GTK_WIDGET (pk->min));
  gtk_box_append (GTK_BOX (box), time_box);

  gtk_widget_set_halign (btns, GTK_ALIGN_END);
  if (allow_clear)
    {
      GtkWidget *clear = gtk_button_new_with_label ("Clear");
      g_signal_connect (clear, "clicked", G_CALLBACK (on_pick_clear), pk);
      gtk_box_append (GTK_BOX (btns), clear);
    }
  gtk_widget_add_css_class (set, "suggested-action");
  g_signal_connect (set, "clicked", G_CALLBACK (on_pick_set), pk);
  gtk_box_append (GTK_BOX (btns), set);
  gtk_box_append (GTK_BOX (box), btns);

  gtk_popover_set_child (GTK_POPOVER (pop), box);
  return pop;
}

/* ------------------------------------------------------------------------ */
/* shared row helpers                                                        */

static void
ignore_reply (RcClient *c, JsonObject *r, const GError *e, gpointer d)
{
}

static void
on_item_delete (GtkButton *b, RcPlanner *pl)
{
  Item *it = g_object_get_data (G_OBJECT (b), "item");

  if (it->server_id && pl->client)
    {
      g_autoptr(JsonBuilder) jb = json_builder_new ();
      json_builder_begin_object (jb);
      json_builder_set_member_name (jb, "eventId");
      json_builder_add_string_value (jb, it->server_id);
      json_builder_end_object (jb);
      rc_client_post (pl->client, "/api/v1/calendar-events.delete",
                      json_builder_get_root (jb), ignore_reply, NULL);
    }
  g_ptr_array_remove (pl->items, it);
  save (pl);
  refresh_all (pl);
}

static GtkWidget *
delete_button (RcPlanner *pl, Item *it)
{
  GtkWidget *b = gtk_button_new_from_icon_name ("user-trash-symbolic");
  gtk_widget_add_css_class (b, "flat");
  gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (b, "Delete");
  g_object_set_data (G_OBJECT (b), "item", it);
  g_signal_connect (b, "clicked", G_CALLBACK (on_item_delete), pl);
  return b;
}

static void
clear_list (GtkListBox *list)
{
  gtk_list_box_remove_all (list);
}

static void
add_placeholder (GtkListBox *list, const char *text)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_widget_add_css_class (l, "dim-label");
  gtk_widget_set_margin_top (l, 18);
  gtk_widget_set_margin_bottom (l, 18);
  gtk_list_box_set_placeholder (list, l);
}

static gint
by_time (gconstpointer a, gconstpointer b)
{
  const Item *x = *(Item **) a, *y = *(Item **) b;
  gint64 tx = x->at ? x->at : G_MAXINT64, ty = y->at ? y->at : G_MAXINT64;
  return tx < ty ? -1 : tx > ty ? 1 : 0;
}

/* ------------------------------------------------------------------------ */
/* tasks                                                                     */

static void
on_task_toggled (GtkCheckButton *c, RcPlanner *pl)
{
  Item *it = g_object_get_data (G_OBJECT (c), "item");
  it->done = gtk_check_button_get_active (c);
  save (pl);
  refresh_all (pl);
}

static void
refresh_tasks (RcPlanner *pl)
{
  g_autoptr(GPtrArray) sorted = g_ptr_array_new ();
  gint64 now = now_unix ();

  clear_list (pl->task_list);
  for (guint i = 0; i < pl->items->len; i++)
    {
      Item *it = g_ptr_array_index (pl->items, i);
      if (it->kind == ITEM_TASK)
        g_ptr_array_add (sorted, it);
    }
  g_ptr_array_sort (sorted, by_time);

  for (int pass = 0; pass < 2; pass++)  /* open tasks first, then done */
    for (guint i = 0; i < sorted->len; i++)
      {
        Item *it = g_ptr_array_index (sorted, i);
        GtkWidget *row, *box, *check, *col, *title;

        if (it->done != (pass == 1))
          continue;
        row = gtk_list_box_row_new ();
        box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class (box, "planner-row");
        check = gtk_check_button_new ();
        gtk_check_button_set_active (GTK_CHECK_BUTTON (check), it->done);
        g_object_set_data (G_OBJECT (check), "item", it);
        g_signal_connect (check, "toggled", G_CALLBACK (on_task_toggled), pl);
        gtk_box_append (GTK_BOX (box), check);

        col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand (col, TRUE);
        gtk_widget_set_valign (col, GTK_ALIGN_CENTER);
        title = gtk_label_new (it->title);
        gtk_label_set_xalign (GTK_LABEL (title), 0);
        gtk_label_set_wrap (GTK_LABEL (title), TRUE);
        if (it->done)
          {
            g_autofree char *m = g_markup_printf_escaped ("<s>%s</s>", it->title);
            gtk_label_set_markup (GTK_LABEL (title), m);
            gtk_widget_add_css_class (title, "dim-label");
          }
        gtk_box_append (GTK_BOX (col), title);
        if (it->at)
          {
            g_autofree char *w = rc_format_when (it->at);
            g_autofree char *s = g_strdup_printf ("⏰ %s", w);
            GtkWidget *due = gtk_label_new (s);
            gtk_label_set_xalign (GTK_LABEL (due), 0);
            gtk_widget_add_css_class (due, "caption");
            if (!it->done && it->at < now)
              gtk_widget_add_css_class (due, "error");
            else
              gtk_widget_add_css_class (due, "dim-label");
            gtk_box_append (GTK_BOX (col), due);
          }
        gtk_box_append (GTK_BOX (box), col);
        gtk_box_append (GTK_BOX (box), delete_button (pl, it));
        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
        gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
        gtk_list_box_append (pl->task_list, row);
      }
}

static void
update_due_button (RcPlanner *pl)
{
  g_autofree char *w = rc_format_when (pl->task_due);
  gtk_menu_button_set_label (pl->task_due_button, pl->task_due ? w : "No due date");
}

static void
on_task_due_picked (GDateTime *dt, gpointer data)
{
  RcPlanner *pl = data;
  pl->task_due = dt ? g_date_time_to_unix (dt) : 0;
  update_due_button (pl);
}

static void
on_task_add (GtkWidget *w, RcPlanner *pl)
{
  g_autofree char *t = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (pl->task_entry))));

  if (*t == '\0')
    return;
  g_ptr_array_add (pl->items, item_new (ITEM_TASK, t, pl->task_due));
  gtk_editable_set_text (GTK_EDITABLE (pl->task_entry), "");
  pl->task_due = 0;
  update_due_button (pl);
  save (pl);
  refresh_all (pl);
}

static GtkWidget *
build_tasks (RcPlanner *pl)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *add = gtk_button_new_from_icon_name ("list-add-symbolic");
  GtkWidget *scroller = gtk_scrolled_window_new ();

  gtk_widget_add_css_class (box, "planner-page");
  pl->task_entry = GTK_ENTRY (gtk_entry_new ());
  gtk_entry_set_placeholder_text (pl->task_entry, "Add a task…");
  gtk_widget_set_hexpand (GTK_WIDGET (pl->task_entry), TRUE);
  g_signal_connect (pl->task_entry, "activate", G_CALLBACK (on_task_add), pl);
  gtk_box_append (GTK_BOX (row), GTK_WIDGET (pl->task_entry));
  gtk_widget_add_css_class (add, "suggested-action");
  gtk_widget_set_tooltip_text (add, "Add Task");
  g_signal_connect (add, "clicked", G_CALLBACK (on_task_add), pl);
  gtk_box_append (GTK_BOX (row), add);
  gtk_box_append (GTK_BOX (box), row);

  pl->task_due_button = GTK_MENU_BUTTON (gtk_menu_button_new ());
  gtk_menu_button_set_always_show_arrow (pl->task_due_button, TRUE);
  gtk_menu_button_set_popover (pl->task_due_button, rc_datetime_popover_new (TRUE, on_task_due_picked, pl));
  gtk_widget_set_halign (GTK_WIDGET (pl->task_due_button), GTK_ALIGN_START);
  gtk_widget_add_css_class (GTK_WIDGET (pl->task_due_button), "flat");
  update_due_button (pl);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (pl->task_due_button));

  pl->task_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (pl->task_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (pl->task_list), "boxed-list");
  gtk_widget_set_valign (GTK_WIDGET (pl->task_list), GTK_ALIGN_START);
  add_placeholder (pl->task_list, "No tasks yet");
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), GTK_WIDGET (pl->task_list));
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_box_append (GTK_BOX (box), scroller);
  return box;
}

/* ------------------------------------------------------------------------ */
/* reminders / timers                                                        */

static void
on_goto (GtkButton *b, RcPlanner *pl)
{
  Item *it = g_object_get_data (G_OBJECT (b), "item");
  if (pl->open_room && it->rid)
    pl->open_room (it->rid, it->msg_id, pl->data);
}

static void
refresh_reminders (RcPlanner *pl)
{
  g_autoptr(GPtrArray) sorted = g_ptr_array_new ();
  gint64 now = now_unix ();

  g_ptr_array_set_size (pl->countdowns, 0);
  clear_list (pl->rem_list);
  for (guint i = 0; i < pl->items->len; i++)
    {
      Item *it = g_ptr_array_index (pl->items, i);
      if (it->kind == ITEM_REMINDER)
        g_ptr_array_add (sorted, it);
    }
  g_ptr_array_sort (sorted, by_time);

  for (int pass = 0; pass < 2; pass++)  /* pending first, then past */
    for (guint i = 0; i < sorted->len; i++)
      {
        Item *it = g_ptr_array_index (sorted, i);
        GtkWidget *row, *box, *col, *title, *sub;
        g_autofree char *when = NULL;

        if (it->fired != (pass == 1))
          continue;
        row = gtk_list_box_row_new ();
        box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class (box, "planner-row");
        gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name (it->fired ? "alarm-symbolic" : "preferences-system-time-symbolic"));

        col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand (col, TRUE);
        title = gtk_label_new (it->title);
        gtk_label_set_xalign (GTK_LABEL (title), 0);
        gtk_label_set_wrap (GTK_LABEL (title), TRUE);
        if (it->fired)
          gtk_widget_add_css_class (title, "dim-label");
        gtk_box_append (GTK_BOX (col), title);

        when = rc_format_when (it->at);
        sub = gtk_label_new (when);
        gtk_label_set_xalign (GTK_LABEL (sub), 0);
        gtk_widget_add_css_class (sub, "caption");
        gtk_widget_add_css_class (sub, "dim-label");
        gtk_box_append (GTK_BOX (col), sub);
        gtk_box_append (GTK_BOX (box), col);

        if (!it->fired)
          {
            g_autofree char *cd = format_countdown (it->at - now);
            GtkWidget *count = gtk_label_new (cd);
            gtk_widget_add_css_class (count, "numeric");
            gtk_widget_add_css_class (count, "accent");
            g_object_set_data (G_OBJECT (count), "item", it);
            g_ptr_array_add (pl->countdowns, count);
            gtk_box_append (GTK_BOX (box), count);
          }
        if (it->rid)
          {
            GtkWidget *go = gtk_button_new_from_icon_name ("go-jump-symbolic");
            gtk_widget_add_css_class (go, "flat");
            gtk_widget_set_valign (go, GTK_ALIGN_CENTER);
            gtk_widget_set_tooltip_text (go, "Go to Message");
            g_object_set_data (G_OBJECT (go), "item", it);
            g_signal_connect (go, "clicked", G_CALLBACK (on_goto), pl);
            gtk_box_append (GTK_BOX (box), go);
          }
        gtk_box_append (GTK_BOX (box), delete_button (pl, it));
        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
        gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
        gtk_list_box_append (pl->rem_list, row);
      }
}

static char *
rem_text (RcPlanner *pl)
{
  g_autofree char *t = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (pl->rem_entry))));
  gtk_editable_set_text (GTK_EDITABLE (pl->rem_entry), "");
  return g_strdup (*t ? t : "Timer");
}

static void
on_timer_preset (GtkButton *b, RcPlanner *pl)
{
  int minutes = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (b), "minutes"));
  g_autofree char *t = rem_text (pl);
  rc_planner_add_reminder (pl, t, now_unix () + minutes * 60, NULL, NULL);
}

static void
on_rem_picked (GDateTime *dt, gpointer data)
{
  RcPlanner *pl = data;
  g_autofree char *t = NULL;

  if (!dt)
    return;
  t = rem_text (pl);
  rc_planner_add_reminder (pl, t, g_date_time_to_unix (dt), NULL, NULL);
}

static GtkWidget *
build_reminders (RcPlanner *pl)
{
  static const int presets[] = { 1, 5, 15, 30, 60 };
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *quick = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *at = gtk_menu_button_new ();
  GtkWidget *scroller = gtk_scrolled_window_new ();

  gtk_widget_add_css_class (box, "planner-page");
  pl->rem_entry = GTK_ENTRY (gtk_entry_new ());
  gtk_entry_set_placeholder_text (pl->rem_entry, "Remind me to…");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (pl->rem_entry));

  gtk_widget_add_css_class (quick, "linked");
  gtk_widget_set_halign (quick, GTK_ALIGN_CENTER);
  for (guint i = 0; i < G_N_ELEMENTS (presets); i++)
    {
      g_autofree char *lbl = presets[i] < 60 ? g_strdup_printf ("%d min", presets[i]) : g_strdup ("1 h");
      GtkWidget *b = gtk_button_new_with_label (lbl);
      g_autofree char *tip = g_strdup_printf ("Start a %s timer", lbl);
      gtk_widget_set_tooltip_text (b, tip);
      g_object_set_data (G_OBJECT (b), "minutes", GINT_TO_POINTER (presets[i]));
      g_signal_connect (b, "clicked", G_CALLBACK (on_timer_preset), pl);
      gtk_box_append (GTK_BOX (quick), b);
    }
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (at), "x-office-calendar-symbolic");
  gtk_widget_set_tooltip_text (at, "Remind at a specific time…");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (at), rc_datetime_popover_new (FALSE, on_rem_picked, pl));
  gtk_box_append (GTK_BOX (quick), at);
  gtk_box_append (GTK_BOX (box), quick);

  pl->rem_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (pl->rem_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (pl->rem_list), "boxed-list");
  gtk_widget_set_valign (GTK_WIDGET (pl->rem_list), GTK_ALIGN_START);
  add_placeholder (pl->rem_list, "No reminders – start a timer above");
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), GTK_WIDGET (pl->rem_list));
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_box_append (GTK_BOX (box), scroller);
  return box;
}

/* ------------------------------------------------------------------------ */
/* calendar                                                                  */

typedef struct {
  RcPlanner *pl;
  guint      serial;
} CalReq;

static GtkWidget *
event_row (RcPlanner *pl, const char *time_text, const char *title, const char *subtitle, Item *it)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *t = gtk_label_new (time_text);
  GtkWidget *col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *tl = gtk_label_new (title);

  gtk_widget_add_css_class (box, "planner-row");
  gtk_widget_add_css_class (t, "numeric");
  gtk_widget_add_css_class (t, "accent");
  gtk_label_set_width_chars (GTK_LABEL (t), 8);
  gtk_label_set_xalign (GTK_LABEL (t), 0);
  gtk_box_append (GTK_BOX (box), t);
  gtk_label_set_xalign (GTK_LABEL (tl), 0);
  gtk_label_set_wrap (GTK_LABEL (tl), TRUE);
  gtk_box_append (GTK_BOX (col), tl);
  if (subtitle && *subtitle)
    {
      GtkWidget *s = gtk_label_new (subtitle);
      gtk_label_set_xalign (GTK_LABEL (s), 0);
      gtk_widget_add_css_class (s, "caption");
      gtk_widget_add_css_class (s, "dim-label");
      gtk_box_append (GTK_BOX (col), s);
    }
  gtk_widget_set_hexpand (col, TRUE);
  gtk_box_append (GTK_BOX (box), col);
  if (it)
    gtk_box_append (GTK_BOX (box), delete_button (pl, it));
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  return row;
}

static const char *
remind_label (int minutes)
{
  switch (minutes)
    {
    case -1: return "";
    case 0: return "🔔 at start";
    case 5: return "🔔 5 min before";
    case 15: return "🔔 15 min before";
    case 30: return "🔔 30 min before";
    case 60: return "🔔 1 hour before";
    case 1440: return "🔔 1 day before";
    default: return "🔔";
    }
}

static gboolean
has_server_id (RcPlanner *pl, const char *sid)
{
  for (guint i = 0; i < pl->items->len; i++)
    {
      Item *it = g_ptr_array_index (pl->items, i);
      if (it->server_id && g_strcmp0 (it->server_id, sid) == 0)
        return TRUE;
    }
  return FALSE;
}

static void
on_server_events (RcClient *client, JsonObject *result, const GError *error, gpointer data)
{
  CalReq *q = data;
  RcPlanner *pl = q->pl;
  JsonArray *arr;

  if (rc_error_is_cancelled (error) || q->serial != pl->cal_serial)
    goto out;
  if (error)
    {
      /* endpoint missing (older server / not enabled): local calendar only */
      if (g_error_matches (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_NOT_FOUND) ||
          g_error_matches (error, RC_CLIENT_ERROR, RC_CLIENT_ERROR_HTTP))
        {
          pl->server_calendar = FALSE;
          gtk_widget_set_visible (pl->event_sync_row, FALSE);
        }
      goto out;
    }
  pl->server_calendar = TRUE;
  gtk_widget_set_visible (pl->event_sync_row, TRUE);

  arr = json_object_has_member (result, "data") ? json_object_get_array_member (result, "data") : NULL;
  for (guint i = 0; arr && i < json_array_get_length (arr); i++)
    {
      JsonObject *e = json_array_get_object_element (arr, i);
      const char *sid, *subject, *start;
      g_autoptr(GDateTime) dt = NULL, local = NULL;
      g_autofree char *tt = NULL;

      if (!e)
        continue;
      sid = json_object_get_string_member_with_default (e, "_id", NULL);
      if (sid && has_server_id (pl, sid))
        continue;
      subject = json_object_get_string_member_with_default (e, "subject", "(no title)");
      start = json_object_get_string_member_with_default (e, "startTime", NULL);
      dt = start ? g_date_time_new_from_iso8601 (start, NULL) : NULL;
      if (dt)
        {
          local = g_date_time_to_local (dt);
          tt = g_date_time_format (local, "%-I:%M %p");
        }
      gtk_list_box_append (pl->event_list,
                           event_row (pl, tt ? tt : "", subject, "Rocket.Chat calendar", NULL));
    }
out:
  g_free (q);
}

static void
refresh_calendar (RcPlanner *pl)
{
  g_autoptr(GDateTime) sel = gtk_calendar_get_date (pl->calendar);
  g_autoptr(GPtrArray) day_items = g_ptr_array_new ();
  g_autofree char *heading = g_date_time_format (sel, "%A, %-d %B %Y");
  int y = g_date_time_get_year (sel), m = g_date_time_get_month (sel), d = g_date_time_get_day_of_month (sel);

  gtk_label_set_text (pl->day_label, heading);
  gtk_calendar_clear_marks (pl->calendar);
  clear_list (pl->event_list);

  for (guint i = 0; i < pl->items->len; i++)
    {
      Item *it = g_ptr_array_index (pl->items, i);
      g_autoptr(GDateTime) dt = NULL;

      if (it->at <= 0 || it->kind == ITEM_REMINDER)
        continue;
      dt = g_date_time_new_from_unix_local (it->at);
      if (g_date_time_get_year (dt) == y && g_date_time_get_month (dt) == m)
        {
          gtk_calendar_mark_day (pl->calendar, g_date_time_get_day_of_month (dt));
          if (g_date_time_get_day_of_month (dt) == d)
            g_ptr_array_add (day_items, it);
        }
    }
  g_ptr_array_sort (day_items, by_time);
  for (guint i = 0; i < day_items->len; i++)
    {
      Item *it = g_ptr_array_index (day_items, i);
      g_autoptr(GDateTime) dt = g_date_time_new_from_unix_local (it->at);
      g_autofree char *tt = g_date_time_format (dt, "%-I:%M %p");
      g_autofree char *sub = it->kind == ITEM_TASK
                             ? g_strdup (it->done ? "Task · done" : "Task due")
                             : g_strdup_printf ("%s%s", remind_label (it->remind_before),
                                                it->server_id ? "  · synced to Rocket.Chat" : "");
      gtk_list_box_append (pl->event_list, event_row (pl, tt, it->title, sub, it));
    }

  if (pl->client)
    {
      g_autoptr(GDateTime) start = g_date_time_new_local (y, m, d, 0, 0, 0);
      g_autoptr(GDateTime) utc = g_date_time_to_utc (start);
      g_autofree char *iso = g_date_time_format (utc, "%Y-%m-%dT%H:%M:%S.000Z");
      g_autofree char *esc = g_uri_escape_string (iso, NULL, FALSE);
      g_autofree char *path = g_strdup_printf ("/api/v1/calendar-events.list?date=%s", esc);
      CalReq *q = g_new0 (CalReq, 1);

      q->pl = pl;
      q->serial = ++pl->cal_serial;
      rc_client_get (pl->client, path, on_server_events, q);
    }
}

static void
on_calendar_changed (GtkCalendar *c, RcPlanner *pl)
{
  refresh_calendar (pl);
}

typedef struct {
  RcPlanner *pl;
  char      *item_id;
} SyncReq;

static void
on_event_synced (RcClient *client, JsonObject *result, const GError *error, gpointer data)
{
  SyncReq *q = data;

  if (!rc_error_is_cancelled (error) && !error)
    {
      const char *sid = json_object_get_string_member_with_default (result, "id", NULL);
      for (guint i = 0; sid && i < q->pl->items->len; i++)
        {
          Item *it = g_ptr_array_index (q->pl->items, i);
          if (g_str_equal (it->id, q->item_id))
            {
              g_free (it->server_id);
              it->server_id = g_strdup (sid);
              save (q->pl);
              refresh_calendar (q->pl);
            }
        }
    }
  g_free (q->item_id);
  g_free (q);
}

static void
on_event_add (GtkWidget *w, RcPlanner *pl)
{
  g_autofree char *t = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (pl->event_entry))));
  g_autoptr(GDateTime) sel = gtk_calendar_get_date (pl->calendar);
  g_autoptr(GDateTime) dt = NULL;
  Item *it;

  if (*t == '\0')
    return;
  dt = g_date_time_new_local (g_date_time_get_year (sel), g_date_time_get_month (sel),
                              g_date_time_get_day_of_month (sel),
                              gtk_spin_button_get_value_as_int (pl->event_hour),
                              gtk_spin_button_get_value_as_int (pl->event_min), 0);
  it = item_new (ITEM_EVENT, t, g_date_time_to_unix (dt));
  it->remind_before = remind_choices[gtk_drop_down_get_selected (pl->event_remind)];
  if (fire_time (it) && fire_time (it) < now_unix ())
    it->fired = TRUE;  /* don't alert for events created in the past */
  g_ptr_array_add (pl->items, it);
  gtk_editable_set_text (GTK_EDITABLE (pl->event_entry), "");
  save (pl);

  if (pl->client && pl->server_calendar && gtk_switch_get_active (pl->event_sync))
    {
      g_autoptr(JsonBuilder) b = json_builder_new ();
      g_autoptr(GDateTime) utc = g_date_time_to_utc (dt);
      g_autofree char *iso = g_date_time_format (utc, "%Y-%m-%dT%H:%M:%S.000Z");
      SyncReq *q = g_new0 (SyncReq, 1);

      json_builder_begin_object (b);
      json_builder_set_member_name (b, "startTime");
      json_builder_add_string_value (b, iso);
      json_builder_set_member_name (b, "subject");
      json_builder_add_string_value (b, t);
      json_builder_set_member_name (b, "description");
      json_builder_add_string_value (b, "");
      if (it->remind_before >= 0)
        {
          json_builder_set_member_name (b, "reminderMinutesBeforeStart");
          json_builder_add_int_value (b, it->remind_before);
        }
      json_builder_end_object (b);
      q->pl = pl;
      q->item_id = g_strdup (it->id);
      rc_client_post (pl->client, "/api/v1/calendar-events.create", json_builder_get_root (b),
                      on_event_synced, q);
    }
  refresh_all (pl);
}

static GtkWidget *
build_calendar (RcPlanner *pl)
{
  const char *choices[] = { "No reminder", "At start", "5 min before", "15 min before",
                            "30 min before", "1 hour before", "1 day before", NULL };
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *inner = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *add_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *time_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *add = gtk_button_new_from_icon_name ("list-add-symbolic");
  GtkWidget *sync_label = gtk_label_new ("Add to Rocket.Chat calendar");

  gtk_widget_add_css_class (inner, "planner-page");
  pl->calendar = GTK_CALENDAR (gtk_calendar_new ());
  g_signal_connect (pl->calendar, "day-selected", G_CALLBACK (on_calendar_changed), pl);
  g_signal_connect (pl->calendar, "next-month", G_CALLBACK (on_calendar_changed), pl);
  g_signal_connect (pl->calendar, "prev-month", G_CALLBACK (on_calendar_changed), pl);
  g_signal_connect (pl->calendar, "next-year", G_CALLBACK (on_calendar_changed), pl);
  g_signal_connect (pl->calendar, "prev-year", G_CALLBACK (on_calendar_changed), pl);
  gtk_box_append (GTK_BOX (inner), GTK_WIDGET (pl->calendar));

  pl->day_label = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_xalign (pl->day_label, 0);
  gtk_widget_add_css_class (GTK_WIDGET (pl->day_label), "heading");
  gtk_box_append (GTK_BOX (inner), GTK_WIDGET (pl->day_label));

  pl->event_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (pl->event_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (pl->event_list), "boxed-list");
  add_placeholder (pl->event_list, "Nothing planned");
  gtk_box_append (GTK_BOX (inner), GTK_WIDGET (pl->event_list));

  pl->event_entry = GTK_ENTRY (gtk_entry_new ());
  gtk_entry_set_placeholder_text (pl->event_entry, "New event on this day…");
  gtk_widget_set_hexpand (GTK_WIDGET (pl->event_entry), TRUE);
  g_signal_connect (pl->event_entry, "activate", G_CALLBACK (on_event_add), pl);
  gtk_box_append (GTK_BOX (add_row), GTK_WIDGET (pl->event_entry));
  gtk_widget_add_css_class (add, "suggested-action");
  gtk_widget_set_tooltip_text (add, "Add Event");
  g_signal_connect (add, "clicked", G_CALLBACK (on_event_add), pl);
  gtk_box_append (GTK_BOX (add_row), add);
  gtk_box_append (GTK_BOX (inner), add_row);

  pl->event_hour = time_spin (23, 9);
  pl->event_min = time_spin (59, 0);
  gtk_orientable_set_orientation (GTK_ORIENTABLE (pl->event_hour), GTK_ORIENTATION_HORIZONTAL);
  gtk_orientable_set_orientation (GTK_ORIENTABLE (pl->event_min), GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append (GTK_BOX (time_row), GTK_WIDGET (pl->event_hour));
  gtk_box_append (GTK_BOX (time_row), gtk_label_new (":"));
  gtk_box_append (GTK_BOX (time_row), GTK_WIDGET (pl->event_min));
  pl->event_remind = GTK_DROP_DOWN (gtk_drop_down_new_from_strings (choices));
  gtk_drop_down_set_selected (pl->event_remind, 3);
  gtk_widget_set_hexpand (GTK_WIDGET (pl->event_remind), TRUE);
  gtk_box_append (GTK_BOX (time_row), GTK_WIDGET (pl->event_remind));
  gtk_box_append (GTK_BOX (inner), time_row);

  pl->event_sync_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_label_set_xalign (GTK_LABEL (sync_label), 0);
  gtk_widget_set_hexpand (sync_label, TRUE);
  gtk_box_append (GTK_BOX (pl->event_sync_row), sync_label);
  pl->event_sync = GTK_SWITCH (gtk_switch_new ());
  gtk_switch_set_active (pl->event_sync, TRUE);
  gtk_box_append (GTK_BOX (pl->event_sync_row), GTK_WIDGET (pl->event_sync));
  gtk_widget_set_visible (pl->event_sync_row, FALSE);
  gtk_box_append (GTK_BOX (inner), pl->event_sync_row);

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), inner);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_box_append (GTK_BOX (box), scroller);
  return box;
}

/* ------------------------------------------------------------------------ */

static void
refresh_all (RcPlanner *pl)
{
  if (!pl->widget)
    return;
  refresh_tasks (pl);
  refresh_reminders (pl);
  refresh_calendar (pl);
}

static void
on_close (GtkButton *b, RcPlanner *pl)
{
  if (pl->close_cb)
    ((void (*) (gpointer)) pl->close_cb) (pl->data);
}

GtkWidget *
rc_planner_get_widget (RcPlanner *pl)
{
  GtkWidget *tv, *header, *switcher, *close;
  AdwViewStack *vs;

  if (pl->widget)
    return pl->widget;

  tv = adw_toolbar_view_new ();
  header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);

  vs = ADW_VIEW_STACK (adw_view_stack_new ());
  adw_view_stack_add_titled_with_icon (vs, build_tasks (pl), "tasks", "Tasks", "object-select-symbolic");
  adw_view_stack_add_titled_with_icon (vs, build_reminders (pl), "reminders", "Timers", "alarm-symbolic");
  adw_view_stack_add_titled_with_icon (vs, build_calendar (pl), "calendar", "Calendar", "x-office-calendar-symbolic");
  pl->stack = (GtkStack *) vs;

  switcher = adw_view_switcher_new ();
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher), vs);
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_SWITCHER_POLICY_NARROW);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), switcher);

  close = gtk_button_new_from_icon_name ("window-close-symbolic");
  gtk_widget_set_tooltip_text (close, "Close Planner");
  g_signal_connect (close, "clicked", G_CALLBACK (on_close), pl);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), close);

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), GTK_WIDGET (vs));
  pl->widget = g_object_ref_sink (tv);
  refresh_all (pl);
  return pl->widget;
}

void
rc_planner_show_page (RcPlanner  *pl,
                      const char *page)
{
  rc_planner_get_widget (pl);
  adw_view_stack_set_visible_child_name (ADW_VIEW_STACK (pl->stack), page);
}

RcPlanner *
rc_planner_new (const char        *data_file,
                RcPlannerAlert     alert,
                RcPlannerOpenRoom  open_room,
                GCallback          close_cb,
                gpointer           user_data)
{
  RcPlanner *pl = g_new0 (RcPlanner, 1);

  pl->file = g_strdup (data_file);
  pl->items = g_ptr_array_new_with_free_func ((GDestroyNotify) item_free);
  pl->countdowns = g_ptr_array_new ();
  pl->alert = alert;
  pl->open_room = open_room;
  pl->close_cb = close_cb;
  pl->data = user_data;
  load (pl);
  pl->tick_id = g_timeout_add_seconds (5, tick, pl);
  pl->ui_tick_id = g_timeout_add_seconds (1, ui_tick, pl);
  /* deliver anything that came due while we were closed */
  g_idle_add_once (tick_once, pl);
  return pl;
}

void
rc_planner_free (RcPlanner *pl)
{
  g_clear_handle_id (&pl->tick_id, g_source_remove);
  g_clear_handle_id (&pl->ui_tick_id, g_source_remove);
  if (pl->client)
    rc_client_unref (pl->client);
  g_clear_object (&pl->widget);
  g_ptr_array_unref (pl->countdowns);
  g_ptr_array_unref (pl->items);
  g_free (pl->file);
  g_free (pl);
}

void
rc_planner_set_client (RcPlanner *pl,
                       RcClient  *client)
{
  if (pl->client)
    rc_client_unref (pl->client);
  pl->client = client ? rc_client_ref (client) : NULL;
  pl->server_calendar = FALSE;
  if (pl->widget)
    refresh_calendar (pl);
}

void
rc_planner_add_reminder (RcPlanner  *pl,
                         const char *text,
                         gint64      at_unix,
                         const char *rid,
                         const char *msg_id)
{
  Item *it = item_new (ITEM_REMINDER, text, at_unix);

  it->rid = g_strdup (rid);
  it->msg_id = g_strdup (msg_id);
  g_ptr_array_add (pl->items, it);
  save (pl);
  refresh_all (pl);
}

gboolean
rc_planner_has_pending (RcPlanner *pl)
{
  for (guint i = 0; i < pl->items->len; i++)
    {
      Item *it = g_ptr_array_index (pl->items, i);
      if (!it->fired && fire_time (it) > 0 && !(it->kind == ITEM_TASK && it->done))
        return TRUE;
    }
  return FALSE;
}
