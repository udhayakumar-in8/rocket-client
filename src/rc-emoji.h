#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* "smile" or ":smile:" -> "😄" (NULL if unknown) */
const char *rc_emoji_lookup              (const char *shortcode);
/* "😄" -> "smile" (NULL if not in the table) */
const char *rc_emoji_shortcode_for       (const char *emoji);
/* replace every known :shortcode: in @text with the emoji */
char       *rc_emoji_replace_shortcodes  (const char *text);

typedef void (*RcEmojiPicked) (const char *shortcode, const char *emoji, gpointer user_data);

/* popover with a grid of common emoji (all have shortcodes, so they can be
 * used as Rocket.Chat reactions) */
GtkWidget  *rc_emoji_picker_new          (RcEmojiPicked func, gpointer user_data);

G_END_DECLS
