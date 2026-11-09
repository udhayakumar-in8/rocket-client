#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

GtkWindow *rc_window_new       (AdwApplication *app);
/* present the window and switch to @rid (used by notification clicks) */
void       rc_window_open_room (GtkWindow *window, const char *rid);

G_END_DECLS
