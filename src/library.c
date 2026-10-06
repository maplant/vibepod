#include "library.h"
#include "mp3.h"
#include <taglib/tag_c.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

static const gchar *const audio_exts[] = {
    "mp3", "m4a", "m4b", "aac", "wav", "aif", "aiff", NULL
};

gboolean
library_is_audio_file (const gchar *path)
{
    const gchar *dot = strrchr (path, '.');
    if (!dot || strchr (dot, G_DIR_SEPARATOR))
        return FALSE;
    for (int i = 0; audio_exts[i]; i++)
        if (g_ascii_strcasecmp (dot + 1, audio_exts[i]) == 0)
            return TRUE;
    return FALSE;
}

static void
scan_dir (const gchar *dir, GList **out, GHashTable *seen_dirs)
{
    /* Guard against symlink loops by remembering canonical directories. */
    gchar *canon = realpath (dir, NULL);
    if (!canon)
        return;
    if (g_hash_table_contains (seen_dirs, canon)) {
        g_free (canon);
        return;
    }
    g_hash_table_add (seen_dirs, canon);

    GDir *d = g_dir_open (dir, 0, NULL);
    if (!d)
        return;

    const gchar *name;
    while ((name = g_dir_read_name (d))) {
        gchar *path = g_build_filename (dir, name, NULL);
        if (g_file_test (path, G_FILE_TEST_IS_DIR)) {
            scan_dir (path, out, seen_dirs);
            g_free (path);
        } else if (g_file_test (path, G_FILE_TEST_IS_REGULAR) &&
                   library_is_audio_file (path)) {
            *out = g_list_prepend (*out, path);
        } else {
            g_free (path);
        }
    }
    g_dir_close (d);
}

GList *
library_scan (const gchar *root)
{
    GList *files = NULL;
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, free, NULL);
    scan_dir (root, &files, seen);
    g_hash_table_destroy (seen);
    return g_list_sort (files, (GCompareFunc) g_utf8_collate);
}

gchar *
library_default_root (void)
{
    const gchar *xdg = g_get_user_special_dir (G_USER_DIRECTORY_MUSIC);
    if (xdg && g_file_test (xdg, G_FILE_TEST_IS_DIR))
        return g_strdup (xdg);
    return g_build_filename (g_get_home_dir (), "Music", NULL);
}

/* ------------------------------------------------------------------ */
/* Tag reading with cache                                              */
/* ------------------------------------------------------------------ */

void
library_song_free (LibrarySong *s)
{
    if (!s)
        return;
    g_free (s->path);
    g_free (s->artist);
    g_free (s->album);
    g_free (s->albumartist);
    g_free (s->title);
    g_free (s);
}

static LibrarySong *
library_song_copy (const LibrarySong *s)
{
    LibrarySong *c = g_memdup2 (s, sizeof *s);
    c->path = g_strdup (s->path);
    c->artist = g_strdup (s->artist);
    c->album = g_strdup (s->album);
    c->albumartist = g_strdup (s->albumartist);
    c->title = g_strdup (s->title);
    return c;
}

typedef struct {
    time_t       mtime;
    guint64      size;
    LibrarySong *song;
} CacheEntry;

static void
cache_entry_free (gpointer p)
{
    CacheEntry *e = p;
    library_song_free (e->song);
    g_free (e);
}

static GMutex      cache_lock;
static GHashTable *cache;          /* path -> CacheEntry */
/* TagLib's C API keeps a global string list; serialise its use. */
static GMutex      taglib_lock;

static gchar *
dup_nonempty (const gchar *s)
{
    return (s && *s) ? g_strdup (s) : NULL;
}

static gchar *
first_property (TagLib_File *tf, const gchar *name)
{
    gchar **vals = taglib_property_get (tf, name);
    gchar *ret = NULL;
    if (vals && vals[0] && *vals[0])
        ret = g_strdup (vals[0]);
    taglib_property_free (vals);
    return ret;
}

static gboolean
has_ext (const gchar *path, const gchar *ext)
{
    const gchar *dot = strrchr (path, '.');
    return dot && g_ascii_strcasecmp (dot + 1, ext) == 0;
}

static LibrarySong *
read_uncached (const gchar *path, const struct stat *st, GError **error)
{
    g_mutex_lock (&taglib_lock);
    TagLib_File *tf = taglib_file_new (path);
    if (!tf || !taglib_file_is_valid (tf)) {
        if (tf)
            taglib_file_free (tf);
        g_mutex_unlock (&taglib_lock);
        g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                     "unreadable or unsupported audio file");
        return NULL;
    }

    LibrarySong *s = g_new0 (LibrarySong, 1);
    s->path = g_strdup (path);
    s->size = st->st_size;

    TagLib_Tag *tag = taglib_file_tag (tf);
    const TagLib_AudioProperties *ap = taglib_file_audioproperties (tf);
    if (tag) {
        s->title = dup_nonempty (taglib_tag_title (tag));
        s->artist = dup_nonempty (taglib_tag_artist (tag));
        s->album = dup_nonempty (taglib_tag_album (tag));
        s->track_nr = (gint) taglib_tag_track (tag);
    }
    s->albumartist = first_property (tf, "ALBUMARTIST");
    gchar *disc = first_property (tf, "DISCNUMBER");
    if (disc) {
        s->disc_nr = (gint) g_ascii_strtoll (disc, NULL, 10);
        g_free (disc);
    }
    gchar *comp = first_property (tf, "COMPILATION");
    if (comp) {
        s->compilation = g_ascii_strtoll (comp, NULL, 10) != 0;
        g_free (comp);
    }
    if (ap) {
        s->length_ms = taglib_audioproperties_length (ap) * 1000;
        s->bitrate = taglib_audioproperties_bitrate (ap);
    }
    taglib_tag_free_strings ();
    taglib_file_free (tf);
    g_mutex_unlock (&taglib_lock);

    if (!s->title) {
        gchar *base = g_path_get_basename (path);
        gchar *dot = strrchr (base, '.');
        if (dot)
            *dot = '\0';
        s->title = base;
    }

    if (has_ext (path, "mp3")) {
        gint ms, br;
        gboolean hdr;
        if (mp3_scan (path, &ms, &br, &hdr) && ms > 0) {
            s->length_ms = ms;
            s->bitrate = br;
            s->exact_length = TRUE;
        }
    }
    return s;
}

LibrarySong *
library_read_song (const gchar *path, GError **error)
{
    struct stat st;
    if (stat (path, &st) != 0) {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "%s", g_strerror (errno));
        return NULL;
    }

    g_mutex_lock (&cache_lock);
    if (!cache)
        cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, cache_entry_free);
    CacheEntry *e = g_hash_table_lookup (cache, path);
    if (e && e->mtime == st.st_mtime && e->size == (guint64) st.st_size) {
        LibrarySong *copy = library_song_copy (e->song);
        g_mutex_unlock (&cache_lock);
        return copy;
    }
    g_mutex_unlock (&cache_lock);

    LibrarySong *s = read_uncached (path, &st, error);
    if (!s)
        return NULL;

    g_mutex_lock (&cache_lock);
    e = g_new0 (CacheEntry, 1);
    e->mtime = st.st_mtime;
    e->size = st.st_size;
    e->song = library_song_copy (s);
    g_hash_table_insert (cache, g_strdup (path), e);
    g_mutex_unlock (&cache_lock);
    return s;
}
