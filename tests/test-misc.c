/* Unit tests: emoji shortcodes, file-type detection, cert error text. */
#include <string.h>
#include "src/rc-emoji.h"
#include "src/rc-preview.h"
#include "src/rc-client.h"

static void
test_emoji (void)
{
  g_autofree char *a = rc_emoji_replace_shortcodes ("ship it :rocket: :+1: :not_an_emoji: 10:30:00");
  g_assert_cmpstr (a, ==, "ship it 🚀 👍 :not_an_emoji: 10:30:00");
  g_assert_cmpstr (rc_emoji_lookup (":tada:"), ==, "🎉");
  g_assert_cmpstr (rc_emoji_lookup ("tada"), ==, "🎉");
  g_assert_null (rc_emoji_lookup ("nope"));
  g_assert_cmpstr (rc_emoji_shortcode_for ("🚀"), ==, "rocket");
}

static void
test_file_kind (void)
{
  g_autoptr(GBytes) text = g_bytes_new_static ("hello\nworld\n", 12);
  g_autoptr(GBytes) bin = g_bytes_new_static ("\x89PNG\0\0\0", 8);

  g_assert_cmpint (rc_file_kind ("fix.diff", NULL, NULL), ==, RC_FILE_DIFF);
  g_assert_cmpint (rc_file_kind ("0001-x.patch", "application/octet-stream", NULL), ==, RC_FILE_DIFF);
  g_assert_cmpint (rc_file_kind ("x", "text/x-diff", NULL), ==, RC_FILE_DIFF);
  g_assert_cmpint (rc_file_kind ("shot.PNG", NULL, NULL), ==, RC_FILE_IMAGE);
  g_assert_cmpint (rc_file_kind ("a", "image/webp", NULL), ==, RC_FILE_IMAGE);
  g_assert_cmpint (rc_file_kind ("clip.mp4", NULL, NULL), ==, RC_FILE_VIDEO);
  g_assert_cmpint (rc_file_kind ("voice.ogg", NULL, NULL), ==, RC_FILE_AUDIO);
  g_assert_cmpint (rc_file_kind ("main.c", NULL, NULL), ==, RC_FILE_TEXT);
  g_assert_cmpint (rc_file_kind ("Makefile", NULL, NULL), ==, RC_FILE_TEXT);
  g_assert_cmpint (rc_file_kind ("cfg.json", "application/json", NULL), ==, RC_FILE_TEXT);
  g_assert_cmpint (rc_file_kind ("doc.pdf", "application/pdf", NULL), ==, RC_FILE_PDF);
  /* unknown extension: sniff the content */
  g_assert_cmpint (rc_file_kind ("notes.weird", "application/octet-stream", text), ==, RC_FILE_TEXT);
  g_assert_cmpint (rc_file_kind ("blob.weird", "application/octet-stream", bin), ==, RC_FILE_OTHER);
}

static void
test_cert_errors (void)
{
  g_autofree char *s = rc_cert_describe_errors (G_TLS_CERTIFICATE_UNKNOWN_CA | G_TLS_CERTIFICATE_BAD_IDENTITY);
  g_assert_nonnull (strstr (s, "self-signed"));
  g_assert_nonnull (strstr (s, "different host"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/misc/emoji", test_emoji);
  g_test_add_func ("/misc/file-kind", test_file_kind);
  g_test_add_func ("/misc/cert-errors", test_cert_errors);
  return g_test_run ();
}
