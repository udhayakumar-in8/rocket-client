#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

typedef enum {
  RC_FILE_IMAGE,
  RC_FILE_VIDEO,
  RC_FILE_AUDIO,
  RC_FILE_DIFF,
  RC_FILE_TEXT,
  RC_FILE_PDF,
  RC_FILE_OTHER,
} RcFileKind;

/* detect by mime type and file name; @bytes (optional) lets unknown types be
 * sniffed for text */
RcFileKind  rc_file_kind        (const char *name,
                                 const char *mime,
                                 GBytes     *bytes);
const char *rc_file_kind_label  (RcFileKind kind);
const char *rc_file_kind_icon   (RcFileKind kind);

/* side-column file viewer */
GtkWidget  *rc_preview_new          (GCallback close_cb, gpointer user_data);
void        rc_preview_set_loading  (GtkWidget *preview, const char *name);
void        rc_preview_set_error    (GtkWidget *preview, const char *name, const char *message);
void        rc_preview_set_content  (GtkWidget  *preview,
                                     const char *name,
                                     const char *mime,
                                     GBytes     *bytes);

G_END_DECLS
