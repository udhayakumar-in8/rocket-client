/* rc-emoji.c - emoji shortcodes (Rocket.Chat / emojione names) */

#include "rc-emoji.h"
#include <string.h>

typedef struct { const char *name; const char *emoji; } Emoji;

/* first block = shown in the reaction picker, in this order */
static const Emoji table[] = {
  { "+1", "👍" }, { "-1", "👎" }, { "heart", "❤️" }, { "joy", "😂" },
  { "smile", "😄" }, { "tada", "🎉" }, { "rocket", "🚀" }, { "eyes", "👀" },
  { "fire", "🔥" }, { "white_check_mark", "✅" }, { "x", "❌" }, { "pray", "🙏" },
  { "clap", "👏" }, { "thinking", "🤔" }, { "ok_hand", "👌" }, { "100", "💯" },
  { "grinning", "😀" }, { "smiley", "😃" }, { "grin", "😁" }, { "laughing", "😆" },
  { "sweat_smile", "😅" }, { "rofl", "🤣" }, { "slight_smile", "🙂" }, { "upside_down", "🙃" },
  { "wink", "😉" }, { "blush", "😊" }, { "innocent", "😇" }, { "heart_eyes", "😍" },
  { "kissing_heart", "😘" }, { "yum", "😋" }, { "stuck_out_tongue", "😛" }, { "stuck_out_tongue_winking_eye", "😜" },
  { "sunglasses", "😎" }, { "nerd", "🤓" }, { "smirk", "😏" }, { "unamused", "😒" },
  { "disappointed", "😞" }, { "pensive", "😔" }, { "worried", "😟" }, { "confused", "😕" },
  { "slight_frown", "🙁" }, { "persevere", "😣" }, { "tired_face", "😫" }, { "weary", "😩" },
  { "cry", "😢" }, { "sob", "😭" }, { "triumph", "😤" }, { "angry", "😠" },
  { "rage", "😡" }, { "flushed", "😳" }, { "scream", "😱" }, { "fearful", "😨" },
  { "cold_sweat", "😰" }, { "hugging", "🤗" }, { "shushing_face", "🤫" }, { "zipper_mouth", "🤐" },
  { "neutral_face", "😐" }, { "expressionless", "😑" }, { "no_mouth", "😶" }, { "rolling_eyes", "🙄" },
  { "grimacing", "😬" }, { "relieved", "😌" }, { "sleeping", "😴" }, { "mask", "😷" },
  { "nauseated_face", "🤢" }, { "dizzy_face", "😵" }, { "exploding_head", "🤯" }, { "partying_face", "🥳" },
  { "cowboy", "🤠" }, { "money_mouth", "🤑" }, { "skull", "💀" }, { "ghost", "👻" },
  { "alien", "👽" }, { "robot", "🤖" }, { "poop", "💩" }, { "wave", "👋" },
  { "raised_hand", "✋" }, { "v", "✌️" }, { "crossed_fingers", "🤞" }, { "muscle", "💪" },
  { "point_up", "☝️" }, { "point_right", "👉" }, { "point_left", "👈" }, { "point_down", "👇" },
  { "raised_hands", "🙌" }, { "handshake", "🤝" }, { "punch", "👊" }, { "metal", "🤘" },
  { "writing_hand", "✍️" }, { "brain", "🧠" }, { "see_no_evil", "🙈" }, { "speak_no_evil", "🙊" },
  { "broken_heart", "💔" }, { "sparkling_heart", "💖" }, { "star", "⭐" }, { "star2", "🌟" },
  { "sparkles", "✨" }, { "zap", "⚡" }, { "boom", "💥" }, { "sunny", "☀️" },
  { "cloud", "☁️" }, { "umbrella", "☔" }, { "snowflake", "❄️" }, { "rainbow", "🌈" },
  { "coffee", "☕" }, { "beer", "🍺" }, { "beers", "🍻" }, { "pizza", "🍕" },
  { "cake", "🍰" }, { "birthday", "🎂" }, { "gift", "🎁" }, { "balloon", "🎈" },
  { "trophy", "🏆" }, { "medal", "🏅" }, { "soccer", "⚽" }, { "video_game", "🎮" },
  { "computer", "💻" }, { "keyboard", "⌨️" }, { "desktop", "🖥️" }, { "iphone", "📱" },
  { "bulb", "💡" }, { "wrench", "🔧" }, { "hammer", "🔨" }, { "gear", "⚙️" },
  { "lock", "🔒" }, { "unlock", "🔓" }, { "key", "🔑" }, { "bug", "🐛" },
  { "warning", "⚠️" }, { "no_entry", "⛔" }, { "question", "❓" }, { "exclamation", "❗" },
  { "heavy_check_mark", "✔️" }, { "heavy_plus_sign", "➕" }, { "heavy_minus_sign", "➖" }, { "arrow_right", "➡️" },
  { "arrow_left", "⬅️" }, { "arrow_up", "⬆️" }, { "arrow_down", "⬇️" }, { "repeat", "🔁" },
  { "hourglass", "⌛" }, { "alarm_clock", "⏰" }, { "stopwatch", "⏱️" }, { "calendar", "📅" },
  { "date", "📅" }, { "pushpin", "📌" }, { "paperclip", "📎" }, { "memo", "📝" },
  { "pencil", "📝" }, { "pencil2", "✏️" }, { "page_facing_up", "📄" }, { "file_folder", "📁" },
  { "chart_with_upwards_trend", "📈" }, { "bar_chart", "📊" }, { "mag", "🔍" }, { "link", "🔗" },
  { "email", "📧" }, { "envelope", "✉️" }, { "bell", "🔔" }, { "no_bell", "🔕" },
  { "speech_balloon", "💬" }, { "thought_balloon", "💭" }, { "zzz", "💤" }, { "dog", "🐶" },
  { "cat", "🐱" }, { "penguin", "🐧" }, { "unicorn", "🦄" }, { "turtle", "🐢" },
  { "snake", "🐍" }, { "seedling", "🌱" }, { "evergreen_tree", "🌲" }, { "rose", "🌹" },
  { "earth_africa", "🌍" }, { "globe_with_meridians", "🌐" }, { "airplane", "✈️" }, { "car", "🚗" },
  { "house", "🏠" }, { "office", "🏢" }, { "moneybag", "💰" }, { "dollar", "💵" },
  { "heart_hands", "🫶" }, { "salute", "🫡" }, { "melting_face", "🫠" }, { "face_with_monocle", "🧐" },
  { "slightly_smiling_face", "🙂" }, { "thumbsup", "👍" }, { "thumbsdown", "👎" }, { "smiling_imp", "😈" },
  { "kiss", "💋" }, { "heartpulse", "💗" }, { "green_heart", "💚" }, { "blue_heart", "💙" },
  { "yellow_heart", "💛" }, { "purple_heart", "💜" }, { "black_heart", "🖤" }, { "white_heart", "🤍" },
  { "checkered_flag", "🏁" }, { "triangular_flag_on_post", "🚩" }, { "construction", "🚧" }, { "ambulance", "🚑" },
};

#define PICKER_COUNT 64

const char *
rc_emoji_lookup (const char *shortcode)
{
  g_autofree char *name = NULL;
  gsize len;

  if (shortcode == NULL)
    return NULL;
  len = strlen (shortcode);
  if (len >= 2 && shortcode[0] == ':' && shortcode[len - 1] == ':')
    name = g_strndup (shortcode + 1, len - 2);
  else
    name = g_strdup (shortcode);

  for (guint i = 0; i < G_N_ELEMENTS (table); i++)
    if (g_str_equal (table[i].name, name))
      return table[i].emoji;
  return NULL;
}

const char *
rc_emoji_shortcode_for (const char *emoji)
{
  for (guint i = 0; i < G_N_ELEMENTS (table); i++)
    if (g_str_equal (table[i].emoji, emoji))
      return table[i].name;
  return NULL;
}

char *
rc_emoji_replace_shortcodes (const char *text)
{
  GString *out = g_string_new (NULL);
  const char *p = text;

  while (*p)
    {
      if (*p == ':')
        {
          const char *end = p + 1;
          while (*end && (g_ascii_isalnum (*end) || *end == '_' || *end == '+' || *end == '-'))
            end++;
          if (*end == ':' && end > p + 1)
            {
              g_autofree char *name = g_strndup (p + 1, end - p - 1);
              const char *e = rc_emoji_lookup (name);
              if (e)
                {
                  g_string_append (out, e);
                  p = end + 1;
                  continue;
                }
            }
        }
      g_string_append_c (out, *p);
      p++;
    }
  return g_string_free (out, FALSE);
}

typedef struct {
  RcEmojiPicked func;
  gpointer      data;
} Picker;

static void
on_emoji_clicked (GtkButton *b,
                  Picker    *pk)
{
  const char *name = g_object_get_data (G_OBJECT (b), "shortcode");
  GtkWidget *pop = gtk_widget_get_ancestor (GTK_WIDGET (b), GTK_TYPE_POPOVER);

  if (pop)
    gtk_popover_popdown (GTK_POPOVER (pop));
  pk->func (name, rc_emoji_lookup (name), pk->data);
}

GtkWidget *
rc_emoji_picker_new (RcEmojiPicked func,
                     gpointer      user_data)
{
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *grid = gtk_grid_new ();
  Picker *pk = g_new0 (Picker, 1);

  pk->func = func;
  pk->data = user_data;
  g_object_set_data_full (G_OBJECT (pop), "picker", pk, g_free);

  for (guint i = 0; i < PICKER_COUNT && i < G_N_ELEMENTS (table); i++)
    {
      GtkWidget *b = gtk_button_new_with_label (table[i].emoji);
      g_autofree char *tip = g_strdup_printf (":%s:", table[i].name);

      gtk_widget_add_css_class (b, "flat");
      gtk_widget_add_css_class (b, "emoji-button");
      gtk_widget_set_tooltip_text (b, tip);
      g_object_set_data (G_OBJECT (b), "shortcode", (gpointer) table[i].name);
      g_signal_connect (b, "clicked", G_CALLBACK (on_emoji_clicked), pk);
      gtk_grid_attach (GTK_GRID (grid), b, i % 8, i / 8, 1, 1);
    }
  gtk_popover_set_child (GTK_POPOVER (pop), grid);
  return pop;
}
