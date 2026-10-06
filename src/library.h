/* Scanning the local music library (~/Music) for audio files. */
#pragma once
#include <glib.h>

typedef struct {
    gchar   *path;
    gchar   *artist;
    gchar   *album;
    gchar   *albumartist;
    gchar   *title;
    gint     track_nr;
    gint     disc_nr;
    guint64  size;
    gint     length_ms;
    gint     bitrate;
    gboolean exact_length;    /* TRUE when counted from MP3 frames */
    gboolean compilation;     /* COMPILATION tag set */
} LibrarySong;

void         library_song_free (LibrarySong *song);

/* Returns TRUE if the file extension is something an iPod Classic can play. */
gboolean     library_is_audio_file (const gchar *path);

/* Recursively collects every audio file under @root. The result is a
 * sorted list of newly allocated absolute paths; free with
 * g_list_free_full (list, g_free). */
GList       *library_scan (const gchar *root);

/* Reads tags and the exact length of one file. Results are cached by
 * path/mtime/size for the lifetime of the process. Thread-safe. */
LibrarySong *library_read_song (const gchar *path, GError **error);

/* Default library location: $XDG_MUSIC_DIR, falling back to ~/Music. */
gchar       *library_default_root (void);
