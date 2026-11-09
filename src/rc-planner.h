#pragma once

#include <adwaita.h>
#include "rc-client.h"

G_BEGIN_DECLS

typedef struct _RcPlanner RcPlanner;

/* fired when a task is due, a reminder/timer elapses or an event starts */
typedef void (*RcPlannerAlert) (const char *title,
                                const char *body,
                                const char *rid,
                                gpointer    user_data);
typedef void (*RcPlannerOpenRoom) (const char *rid,
                                   const char *msg_id,
                                   gpointer    user_data);

RcPlanner *rc_planner_new             (const char        *data_file,
                                       RcPlannerAlert     alert,
                                       RcPlannerOpenRoom  open_room,
                                       GCallback          close_cb,
                                       gpointer           user_data);
void       rc_planner_free            (RcPlanner *planner);
GtkWidget *rc_planner_get_widget      (RcPlanner *planner);
/* optional: enables Rocket.Chat server calendar (calendar-events.*) */
void       rc_planner_set_client      (RcPlanner *planner,
                                       RcClient  *client);
void       rc_planner_add_reminder    (RcPlanner  *planner,
                                       const char *text,
                                       gint64      at_unix,
                                       const char *rid,
                                       const char *msg_id);
gboolean   rc_planner_has_pending     (RcPlanner *planner);
void       rc_planner_show_page       (RcPlanner  *planner,
                                       const char *page); /* tasks|reminders|calendar */

/* popover with presets + calendar + time; @func gets NULL for "Clear" */
typedef void (*RcPickedTime) (GDateTime *when, gpointer user_data);
GtkWidget *rc_datetime_popover_new    (gboolean      allow_clear,
                                       RcPickedTime  func,
                                       gpointer      user_data);

char      *rc_format_when             (gint64 unix_time);

G_END_DECLS
