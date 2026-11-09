/* main.c - Rocket Client: a minimal Rocket.Chat client for GNOME */

#include "config.h"
#include "rc-window.h"

static const char *css =
  ".unread-badge {"
  "  background-color: alpha(currentColor, 0.12);"
  "  border-radius: 999px;"
  "  padding: 1px 8px;"
  "  font-size: 0.8em;"
  "  font-weight: bold;"
  "  font-feature-settings: \"tnum\";"
  "}"
  ".room-section { margin: 12px 12px 4px 12px; }"
  ".message-list { padding: 8px 6px 12px 6px; }"
  ".msg-block { margin: 10px 12px 0 12px; }"
  ".msg-continued { margin-top: 6px; margin-right: 12px; margin-left: 12px; }"
  ".msg-author { color: @accent_color; font-weight: bold; }"
  ".msg-author-me { color: @success_color; }"
  ".msg-body { line-height: 1.3; }"
  ".system-message { margin: 10px 12px 0 12px; font-style: italic; }"
  ".composer { padding: 6px; border-top: 1px solid alpha(currentColor, 0.1); }"
  ".composer entry { min-height: 34px; }"
  /* message units & hover toolbar */
  ".msg-unit { border-radius: 8px; }"
  ".msg-unit:hover { background-color: alpha(currentColor, 0.04); }"
  ".msg-actions { opacity: 0; margin: -6px 2px 0 0; padding: 2px; border-radius: 8px;"
  "  background-color: @popover_bg_color; box-shadow: 0 1px 4px alpha(black, 0.18); }"
  ".msg-actions button, .msg-actions menubutton > button { min-height: 26px; min-width: 26px; padding: 2px; }"
  ".msg-unit:hover > .msg-actions { opacity: 1; }"
  ".msg-meta { color: alpha(currentColor, 0.55); }"
  ".msg-meta.read { color: @accent_color; }"
  ".reaction { padding: 1px 8px; min-height: 22px; border-radius: 999px; font-size: 0.9em; }"
  ".reaction.reacted { background-color: alpha(@accent_bg_color, 0.25); box-shadow: inset 0 0 0 1px @accent_bg_color; }"
  ".thread-button { color: @accent_color; padding: 2px 6px; font-size: 0.9em; }"
  ".file-card { padding: 8px 10px; border-radius: 10px; background-color: alpha(currentColor, 0.06); }"
  ".file-snippet { padding: 0; border-radius: 10px; }"
  ".file-snippet textview, .file-snippet text { background: alpha(currentColor, 0.04); font-size: 0.85em; padding: 6px; }"
  ".image-attachment { padding: 0; border-radius: 10px; }"
  ".image-attachment picture { border-radius: 10px; }"
  ".quote { border-left: 3px solid @accent_bg_color; padding: 2px 10px; }"
  ".typing-label { margin: 0 14px; min-height: 16px; }"
  ".edit-bar { padding: 4px 10px; background-color: alpha(@accent_bg_color, 0.12); }"
  ".emoji-button { font-size: 1.3em; min-width: 34px; min-height: 34px; padding: 0; }"
  ".planner-page { padding: 12px; }"
  ".planner-row { padding: 8px 10px; }"
  ".preview-text { font-size: 0.9em; }";

static void
action_about (GSimpleAction *a,
              GVariant      *p,
              gpointer       data)
{
  GtkApplication *app = data;
  AdwDialog *dialog = adw_about_dialog_new ();
  const char *developers[] = { "Rocket Client contributors", NULL };

  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (dialog), "Rocket Client");
  adw_about_dialog_set_application_icon (ADW_ABOUT_DIALOG (dialog), APP_ID);
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (dialog), APP_VERSION);
  adw_about_dialog_set_comments (ADW_ABOUT_DIALOG (dialog),
                                 "A Rocket.Chat messaging client written in C with GTK 4 and libadwaita.");
  adw_about_dialog_set_website (ADW_ABOUT_DIALOG (dialog),
                                "https://github.com/udhayakumar-in8/rocket-client");
  adw_about_dialog_set_developers (ADW_ABOUT_DIALOG (dialog), developers);
  adw_dialog_present (dialog, GTK_WIDGET (gtk_application_get_active_window (app)));
}

static void
action_quit (GSimpleAction *a,
             GVariant      *p,
             gpointer       data)
{
  g_application_quit (G_APPLICATION (data));
}

static void
action_open_room (GSimpleAction *a,
                  GVariant      *p,
                  gpointer       data)
{
  GtkApplication *app = data;
  GtkWindow *win = gtk_application_get_active_window (app);

  if (win == NULL)
    win = rc_window_new (ADW_APPLICATION (app));
  rc_window_open_room (win, g_variant_get_string (p, NULL));
}

static void
on_startup (GApplication *app)
{
  static const GActionEntry entries[] = {
    { .name = "about", .activate = action_about },
    { .name = "quit", .activate = action_quit },
    { .name = "open-room", .activate = action_open_room, .parameter_type = "s" },
  };
  const char *quit_accels[] = { "<Ctrl>q", NULL };
  const char *refresh_accels[] = { "F5", "<Ctrl>r", NULL };
  const char *attach_accels[] = { "<Ctrl>u", NULL };
  GtkCssProvider *provider = gtk_css_provider_new ();

  g_action_map_add_action_entries (G_ACTION_MAP (app), entries, G_N_ELEMENTS (entries), app);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "app.quit", quit_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.refresh", refresh_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.attach", attach_accels);

  gtk_css_provider_load_from_string (provider, css);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                              GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);
}

static void
on_activate (GApplication *app)
{
  GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));

  if (win == NULL)
    win = rc_window_new (ADW_APPLICATION (app));
  gtk_window_present (win);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr(AdwApplication) app = NULL;

  g_set_application_name ("Rocket Client");
  app = adw_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
