/* rc-preview.c - file preview column: text/diff, images, audio/video */

#include "rc-preview.h"
#include <string.h>

#define MAX_TEXT_BYTES (4 * 1024 * 1024)

typedef struct {
  AdwWindowTitle *title;
  GtkStack       *stack;
  GtkTextView    *text;
  GtkPicture     *picture;
  GtkVideo       *video;
  AdwStatusPage  *other;
  AdwStatusPage  *error;
  GtkWidget      *save_button;
  GtkWidget      *open_button;
  GBytes         *bytes;
  char           *name;
} Preview;

static Preview *
get (GtkWidget *w)
{
  return g_object_get_data (G_OBJECT (w), "rc-preview");
}

static void
preview_free (Preview *p)
{
  g_clear_pointer (&p->bytes, g_bytes_unref);
  g_free (p->name);
  g_free (p);
}

/* ------------------------------------------------------------------------ */
/* type detection                                                            */

static gboolean
has_ext (const char *name, const char * const *exts)
{
  g_autofree char *lower = name ? g_ascii_strdown (name, -1) : NULL;
  const char *dot = lower ? strrchr (lower, '.') : NULL;

  if (lower == NULL)
    return FALSE;
  /* files like "Makefile", "Dockerfile" */
  if (dot == NULL)
    dot = lower;
  else
    dot++;
  for (guint i = 0; exts[i]; i++)
    if (g_str_equal (dot, exts[i]))
      return TRUE;
  return FALSE;
}

static gboolean
looks_like_text (GBytes *bytes)
{
  gsize len = 0;
  const char *data = g_bytes_get_data (bytes, &len);
  gsize check = MIN (len, 8192);
  const char *end = NULL;

  if (len == 0)
    return TRUE;
  if (memchr (data, '\0', check))
    return FALSE;
  /* allow a truncated multibyte sequence at the cut */
  if (!g_utf8_validate (data, check, &end) && (gsize) (end - data) < check - 4)
    return FALSE;
  return TRUE;
}

RcFileKind
rc_file_kind (const char *name,
              const char *mime,
              GBytes     *bytes)
{
  static const char *diff_ext[] = { "diff", "patch", "debdiff", NULL };
  static const char *text_ext[] = {
    "txt", "log", "md", "markdown", "rst", "csv", "tsv", "json", "xml", "yml", "yaml",
    "toml", "ini", "conf", "cfg", "c", "h", "cc", "cpp", "hpp", "cs", "java", "kt", "go",
    "rs", "py", "rb", "pl", "php", "js", "mjs", "ts", "tsx", "jsx", "css", "scss", "html",
    "htm", "sh", "bash", "zsh", "fish", "sql", "lua", "vala", "swift", "m", "r", "dart",
    "makefile", "dockerfile", "cmake", "meson", "build", "gradle", "properties", "env",
    "service", "desktop", "srt", "vtt", "tex", "bib", "ics", "vcf", "pem", "crt", "key",
    "pub", "gitignore", "editorconfig", NULL };
  static const char *img_ext[] = { "png", "jpg", "jpeg", "gif", "webp", "bmp", "svg", "tif", "tiff", "ico", NULL };
  static const char *vid_ext[] = { "mp4", "webm", "mkv", "mov", "avi", "ogv", NULL };
  static const char *aud_ext[] = { "mp3", "ogg", "oga", "opus", "wav", "flac", "m4a", "aac", NULL };
  g_autofree char *guessed = NULL;

  if (mime == NULL || *mime == '\0' || g_str_equal (mime, "application/octet-stream"))
    {
      guessed = g_content_type_guess (name, NULL, 0, NULL);
      if (guessed)
        {
          g_autofree char *m = g_content_type_get_mime_type (guessed);
          g_free (guessed);
          guessed = g_steal_pointer (&m);
        }
      mime = guessed ? guessed : "";
    }

  if (has_ext (name, diff_ext) || g_str_equal (mime, "text/x-diff") || g_str_equal (mime, "text/x-patch"))
    return RC_FILE_DIFF;
  if (g_str_has_prefix (mime, "image/") || has_ext (name, img_ext))
    return RC_FILE_IMAGE;
  if (g_str_has_prefix (mime, "video/") || has_ext (name, vid_ext))
    return RC_FILE_VIDEO;
  if (g_str_has_prefix (mime, "audio/") || has_ext (name, aud_ext))
    return RC_FILE_AUDIO;
  if (g_str_equal (mime, "application/pdf"))
    return RC_FILE_PDF;
  if (g_str_has_prefix (mime, "text/") || has_ext (name, text_ext) ||
      g_str_equal (mime, "application/json") || g_str_equal (mime, "application/xml") ||
      g_str_equal (mime, "application/javascript") || g_str_equal (mime, "application/x-shellscript") ||
      g_str_equal (mime, "application/x-yaml") || g_str_equal (mime, "application/toml"))
    return RC_FILE_TEXT;
  if (bytes && looks_like_text (bytes))
    return RC_FILE_TEXT;
  return RC_FILE_OTHER;
}

const char *
rc_file_kind_label (RcFileKind kind)
{
  switch (kind)
    {
    case RC_FILE_IMAGE: return "Image";
    case RC_FILE_VIDEO: return "Video";
    case RC_FILE_AUDIO: return "Audio";
    case RC_FILE_DIFF:  return "Diff / patch";
    case RC_FILE_TEXT:  return "Text";
    case RC_FILE_PDF:   return "PDF document";
    case RC_FILE_OTHER:
    default:            return "File";
    }
}

const char *
rc_file_kind_icon (RcFileKind kind)
{
  switch (kind)
    {
    case RC_FILE_IMAGE: return "image-x-generic-symbolic";
    case RC_FILE_VIDEO: return "video-x-generic-symbolic";
    case RC_FILE_AUDIO: return "audio-x-generic-symbolic";
    case RC_FILE_DIFF:
    case RC_FILE_TEXT:  return "text-x-generic-symbolic";
    case RC_FILE_PDF:   return "x-office-document-symbolic";
    case RC_FILE_OTHER:
    default:            return "folder-documents-symbolic";
    }
}

/* ------------------------------------------------------------------------ */
/* save / open externally                                                    */

static GFile *
write_cache_file (Preview *p, GError **error)
{
  g_autofree char *dir = g_build_filename (g_get_user_cache_dir (), "rocket-client", "files", NULL);
  g_autofree char *base = g_path_get_basename (p->name ? p->name : "file");
  g_autofree char *path = NULL;
  gsize len = 0;
  const char *data = g_bytes_get_data (p->bytes, &len);

  g_mkdir_with_parents (dir, 0700);
  path = g_build_filename (dir, base, NULL);
  if (!g_file_set_contents (path, data, len, error))
    return NULL;
  return g_file_new_for_path (path);
}

static void
on_open_clicked (GtkButton *b, Preview *p)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GtkFileLauncher) launcher = NULL;

  if (p->bytes == NULL)
    return;
  file = write_cache_file (p, &error);
  if (file == NULL)
    {
      g_warning ("Could not write file: %s", error->message);
      return;
    }
  launcher = gtk_file_launcher_new (file);
  gtk_file_launcher_launch (launcher, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (b))), NULL, NULL, NULL);
}

static void
on_save_done (GObject *source, GAsyncResult *res, gpointer data)
{
  Preview *p = data;
  g_autoptr(GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), res, NULL);

  if (file && p->bytes)
    g_file_replace_contents_bytes_async (file, p->bytes, NULL, FALSE,
                                         G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, NULL);
}

static void
on_save_clicked (GtkButton *b, Preview *p)
{
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new ();

  if (p->bytes == NULL)
    return;
  gtk_file_dialog_set_initial_name (dialog, p->name ? p->name : "file");
  gtk_file_dialog_save (dialog, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (b))), NULL, on_save_done, p);
}

/* ------------------------------------------------------------------------ */

static void
on_close_clicked (GtkButton *b, gpointer data)
{
  GCallback cb = g_object_get_data (G_OBJECT (b), "close-cb");
  if (cb)
    ((void (*) (gpointer)) cb) (data);
}

static void
setup_tags (GtkTextBuffer *buf)
{
  gtk_text_buffer_create_tag (buf, "add", "paragraph-background", "rgba(46,194,126,0.22)", NULL);
  gtk_text_buffer_create_tag (buf, "del", "paragraph-background", "rgba(224,27,36,0.20)", NULL);
  gtk_text_buffer_create_tag (buf, "hunk", "foreground", "#3584e4", "paragraph-background", "rgba(53,132,228,0.10)", NULL);
  gtk_text_buffer_create_tag (buf, "meta", "weight", PANGO_WEIGHT_BOLD, NULL);
}

GtkWidget *
rc_preview_new (GCallback close_cb,
                gpointer  user_data)
{
  Preview *p = g_new0 (Preview, 1);
  GtkWidget *tv = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *close = gtk_button_new_from_icon_name ("window-close-symbolic");
  GtkWidget *text_scroller = gtk_scrolled_window_new ();
  GtkWidget *pic_scroller = gtk_scrolled_window_new ();
  GtkWidget *spinner = gtk_spinner_new ();
  GtkWidget *open_btn;

  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  p->title = ADW_WINDOW_TITLE (adw_window_title_new ("Preview", ""));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (p->title));

  gtk_widget_set_tooltip_text (close, "Close Preview");
  g_object_set_data (G_OBJECT (close), "close-cb", close_cb);
  g_signal_connect (close, "clicked", G_CALLBACK (on_close_clicked), user_data);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), close);

  p->save_button = gtk_button_new_from_icon_name ("document-save-symbolic");
  gtk_widget_set_tooltip_text (p->save_button, "Save As…");
  g_signal_connect (p->save_button, "clicked", G_CALLBACK (on_save_clicked), p);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), p->save_button);

  p->open_button = gtk_button_new_from_icon_name ("document-open-symbolic");
  gtk_widget_set_tooltip_text (p->open_button, "Open With Default App");
  g_signal_connect (p->open_button, "clicked", G_CALLBACK (on_open_clicked), p);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), p->open_button);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);

  p->stack = GTK_STACK (gtk_stack_new ());

  gtk_spinner_start (GTK_SPINNER (spinner));
  gtk_widget_set_size_request (spinner, 32, 32);
  gtk_widget_set_halign (spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (spinner, GTK_ALIGN_CENTER);
  gtk_stack_add_named (p->stack, spinner, "loading");

  p->text = GTK_TEXT_VIEW (gtk_text_view_new ());
  gtk_text_view_set_editable (p->text, FALSE);
  gtk_text_view_set_monospace (p->text, TRUE);
  gtk_text_view_set_left_margin (p->text, 8);
  gtk_text_view_set_top_margin (p->text, 8);
  gtk_text_view_set_bottom_margin (p->text, 8);
  gtk_widget_add_css_class (GTK_WIDGET (p->text), "preview-text");
  setup_tags (gtk_text_view_get_buffer (p->text));
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (text_scroller), GTK_WIDGET (p->text));
  gtk_stack_add_named (p->stack, text_scroller, "text");

  p->picture = GTK_PICTURE (gtk_picture_new ());
  gtk_picture_set_content_fit (p->picture, GTK_CONTENT_FIT_CONTAIN);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (pic_scroller), GTK_WIDGET (p->picture));
  gtk_stack_add_named (p->stack, pic_scroller, "image");

  p->video = GTK_VIDEO (gtk_video_new ());
  gtk_stack_add_named (p->stack, GTK_WIDGET (p->video), "media");

  p->other = ADW_STATUS_PAGE (adw_status_page_new ());
  open_btn = gtk_button_new_with_label ("Open With Default App");
  gtk_widget_add_css_class (open_btn, "pill");
  gtk_widget_add_css_class (open_btn, "suggested-action");
  gtk_widget_set_halign (open_btn, GTK_ALIGN_CENTER);
  g_signal_connect (open_btn, "clicked", G_CALLBACK (on_open_clicked), p);
  adw_status_page_set_child (p->other, open_btn);
  gtk_stack_add_named (p->stack, GTK_WIDGET (p->other), "other");

  p->error = ADW_STATUS_PAGE (adw_status_page_new ());
  adw_status_page_set_icon_name (p->error, "dialog-error-symbolic");
  adw_status_page_set_title (p->error, "Could Not Load File");
  gtk_stack_add_named (p->stack, GTK_WIDGET (p->error), "error");

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), GTK_WIDGET (p->stack));
  g_object_set_data_full (G_OBJECT (tv), "rc-preview", p, (GDestroyNotify) preview_free);
  return tv;
}

static void
reset (Preview *p, const char *name)
{
  g_clear_pointer (&p->bytes, g_bytes_unref);
  g_free (p->name);
  p->name = g_strdup (name);
  gtk_video_set_file (p->video, NULL);
  gtk_picture_set_paintable (p->picture, NULL);
  adw_window_title_set_title (p->title, name ? name : "Preview");
  adw_window_title_set_subtitle (p->title, "");
  gtk_widget_set_sensitive (p->save_button, FALSE);
  gtk_widget_set_sensitive (p->open_button, FALSE);
}

void
rc_preview_set_loading (GtkWidget  *w,
                        const char *name)
{
  Preview *p = get (w);
  reset (p, name);
  adw_window_title_set_subtitle (p->title, "Downloading…");
  gtk_stack_set_visible_child_name (p->stack, "loading");
}

void
rc_preview_set_error (GtkWidget  *w,
                      const char *name,
                      const char *message)
{
  Preview *p = get (w);
  reset (p, name);
  adw_status_page_set_description (p->error, message);
  gtk_stack_set_visible_child_name (p->stack, "error");
}

static void
fill_text (Preview *p, const char *data, gsize len, gboolean diff)
{
  GtkTextBuffer *buf = gtk_text_view_get_buffer (p->text);
  gboolean truncated = len > MAX_TEXT_BYTES;
  g_autofree char *valid = g_utf8_make_valid (data, MIN (len, MAX_TEXT_BYTES));
  GtkTextIter end;

  gtk_text_buffer_set_text (buf, "", 0);
  gtk_text_view_set_wrap_mode (p->text, diff ? GTK_WRAP_NONE : GTK_WRAP_WORD_CHAR);

  if (!diff)
    gtk_text_buffer_set_text (buf, valid, -1);
  else
    {
      g_auto(GStrv) lines = g_strsplit (valid, "\n", -1);
      for (guint i = 0; lines[i]; i++)
        {
          const char *l = lines[i];
          const char *tag = NULL;

          if (g_str_has_prefix (l, "+++") || g_str_has_prefix (l, "---") ||
              g_str_has_prefix (l, "diff ") || g_str_has_prefix (l, "index ") ||
              g_str_has_prefix (l, "From ") || g_str_has_prefix (l, "Subject:"))
            tag = "meta";
          else if (g_str_has_prefix (l, "@@"))
            tag = "hunk";
          else if (l[0] == '+')
            tag = "add";
          else if (l[0] == '-')
            tag = "del";

          gtk_text_buffer_get_end_iter (buf, &end);
          if (tag)
            gtk_text_buffer_insert_with_tags_by_name (buf, &end, l, -1, tag, NULL);
          else
            gtk_text_buffer_insert (buf, &end, l, -1);
          if (lines[i + 1])
            {
              gtk_text_buffer_get_end_iter (buf, &end);
              gtk_text_buffer_insert (buf, &end, "\n", 1);
            }
        }
    }

  if (truncated)
    {
      gtk_text_buffer_get_end_iter (buf, &end);
      gtk_text_buffer_insert (buf, &end, "\n\n… (truncated – save the file to see everything)", -1);
    }
}

void
rc_preview_set_content (GtkWidget  *w,
                        const char *name,
                        const char *mime,
                        GBytes     *bytes)
{
  Preview *p = get (w);
  RcFileKind kind = rc_file_kind (name, mime, bytes);
  gsize len = 0;
  const char *data = g_bytes_get_data (bytes, &len);
  g_autofree char *size = g_format_size (len);
  g_autofree char *subtitle = g_strdup_printf ("%s · %s", rc_file_kind_label (kind), size);

  reset (p, name);
  p->bytes = g_bytes_ref (bytes);
  adw_window_title_set_subtitle (p->title, subtitle);
  gtk_widget_set_sensitive (p->save_button, TRUE);
  gtk_widget_set_sensitive (p->open_button, TRUE);

  switch (kind)
    {
    case RC_FILE_DIFF:
    case RC_FILE_TEXT:
      fill_text (p, data, len, kind == RC_FILE_DIFF);
      gtk_stack_set_visible_child_name (p->stack, "text");
      return;

    case RC_FILE_IMAGE:
      {
        g_autoptr(GdkTexture) tex = gdk_texture_new_from_bytes (bytes, NULL);
        if (tex)
          {
            gtk_picture_set_paintable (p->picture, GDK_PAINTABLE (tex));
            gtk_stack_set_visible_child_name (p->stack, "image");
            return;
          }
        break;
      }

    case RC_FILE_VIDEO:
    case RC_FILE_AUDIO:
      {
        g_autoptr(GFile) file = write_cache_file (p, NULL);
        if (file)
          {
            gtk_video_set_file (p->video, file);
            gtk_stack_set_visible_child_name (p->stack, "media");
            return;
          }
        break;
      }

    case RC_FILE_PDF:
    case RC_FILE_OTHER:
    default:
      break;
    }

  adw_status_page_set_icon_name (p->other, rc_file_kind_icon (kind));
  adw_status_page_set_title (p->other, name ? name : "File");
  adw_status_page_set_description (p->other, "No built-in preview for this file type.");
  gtk_stack_set_visible_child_name (p->stack, "other");
}
