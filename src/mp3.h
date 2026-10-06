/* Exact MP3 duration by counting frames (TagLib only estimates from the
 * first frame's bitrate when there is no Xing/VBRI header, which is wildly
 * wrong for VBR files). */
#pragma once
#include <glib.h>

/* Returns TRUE if @path is an MPEG audio stream. @length_ms receives the
 * exact duration, @bitrate_kbps the average bitrate, @has_vbr_header
 * whether a Xing/Info/VBRI header was present. */
gboolean mp3_scan (const gchar *path, gint *length_ms, gint *bitrate_kbps,
                   gboolean *has_vbr_header);
