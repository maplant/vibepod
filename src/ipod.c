#include "ipod.h"
#include "library.h"
#include "mp3.h"

#include <gpod/itdb.h>
#include <taglib/tag_c.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

void
ipod_info_free (IpodInfo *info)
{
    if (!info)
        return;
    g_free (info->mountpoint);
    g_free (info->device_node);
    g_free (info->name);
    g_free (info->model);
    g_free (info->model_number);
    g_free (info);
}

gchar *
ipod_sysinfo_extended_path (const gchar *mountpoint)
{
    gchar *ctl = itdb_get_control_dir (mountpoint);
    gchar *path;
    if (!ctl)
        ctl = g_build_filename (mountpoint, "iPod_Control", NULL);
    path = g_build_filename (ctl, "Device", "SysInfoExtended", NULL);
    g_free (ctl);
    return path;
}

static gboolean
looks_like_ipod (const gchar *mountpoint)
{
    gchar *ctl = g_build_filename (mountpoint, "iPod_Control", NULL);
    gboolean ok = g_file_test (ctl, G_FILE_TEST_IS_DIR);
    g_free (ctl);
    return ok;
}

/* "/dev/sda1" -> "/dev/sda"; "/dev/mmcblk0p1" -> "/dev/mmcblk0". */
static gchar *
whole_disk_node (const gchar *partition)
{
    if (!partition)
        return NULL;
    gchar *disk = g_strdup (partition);
    gsize len = strlen (disk);
    while (len > 0 && g_ascii_isdigit (disk[len - 1]))
        len--;
    if (len > 1 && disk[len - 1] == 'p' && g_ascii_isdigit (disk[len - 2]))
        len--;
    disk[len] = '\0';
    return disk;
}

static gchar *
itunesdb_path (const gchar *mountpoint)
{
    gchar *ctl = itdb_get_control_dir (mountpoint);
    gchar *db = ctl ? g_build_filename (ctl, "iTunes", "iTunesDB", NULL) : NULL;
    g_free (ctl);
    return db;
}

static void
fill_db_details (IpodInfo *info)
{
    gchar *db = itunesdb_path (info->mountpoint);
    gchar *ext = ipod_sysinfo_extended_path (info->mountpoint);

    info->has_itunesdb = db && g_file_test (db, G_FILE_TEST_IS_REGULAR);
    info->has_sysinfo_extended = g_file_test (ext, G_FILE_TEST_IS_REGULAR);

    /* Model information comes from SysInfo / SysInfoExtended and is
     * available even without a database. */
    Itdb_Device *dev = itdb_device_new ();
    itdb_device_set_mountpoint (dev, info->mountpoint);
    const Itdb_IpodInfo *ii = itdb_device_get_ipod_info (dev);
    if (ii && ii->ipod_model != ITDB_IPOD_MODEL_INVALID &&
        ii->ipod_model != ITDB_IPOD_MODEL_UNKNOWN) {
        info->model = g_strdup (itdb_info_get_ipod_model_name_string (ii->ipod_model));
        info->model_number = g_strdup (ii->model_number);
        info->capacity_gb = ii->capacity;
    }
    itdb_device_free (dev);

    GFile *f = g_file_new_for_path (info->mountpoint);
    GFileInfo *fi = g_file_query_filesystem_info (f,
        G_FILE_ATTRIBUTE_FILESYSTEM_FREE "," G_FILE_ATTRIBUTE_FILESYSTEM_SIZE,
        NULL, NULL);
    if (fi) {
        info->free_bytes = g_file_info_get_attribute_uint64 (fi, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
        info->total_bytes = g_file_info_get_attribute_uint64 (fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
        g_object_unref (fi);
    }
    g_object_unref (f);

    g_free (db);
    g_free (ext);
}

IpodInfo *
ipod_find (GVolumeMonitor *monitor)
{
    IpodInfo *info = NULL;
    GList *mounts = g_volume_monitor_get_mounts (monitor);

    for (GList *l = mounts; l && !info; l = l->next) {
        GMount *mount = l->data;
        GFile *root = g_mount_get_root (mount);
        gchar *path = g_file_get_path (root);
        if (path && looks_like_ipod (path)) {
            info = g_new0 (IpodInfo, 1);
            info->mountpoint = g_strdup (path);
            info->name = g_mount_get_name (mount);
            GVolume *vol = g_mount_get_volume (mount);
            if (vol) {
                gchar *part = g_volume_get_identifier (vol, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
                info->device_node = whole_disk_node (part);
                g_free (part);
                g_object_unref (vol);
            }
        }
        g_free (path);
        g_object_unref (root);
    }
    g_list_free_full (mounts, g_object_unref);

    /* Fallback for mounts GIO doesn't report (e.g. manual mounts). */
    if (!info) {
        const gchar *bases[] = { "/run/media", "/media", NULL };
        for (int i = 0; bases[i] && !info; i++) {
            gchar *userdir = g_build_filename (bases[i], g_get_user_name (), NULL);
            const gchar *dirs[] = { userdir, bases[i], NULL };
            for (int j = 0; dirs[j] && !info; j++) {
                GDir *d = g_dir_open (dirs[j], 0, NULL);
                if (!d)
                    continue;
                const gchar *name;
                while ((name = g_dir_read_name (d)) && !info) {
                    gchar *mp = g_build_filename (dirs[j], name, NULL);
                    if (looks_like_ipod (mp)) {
                        info = g_new0 (IpodInfo, 1);
                        info->mountpoint = g_strdup (mp);
                        info->name = g_strdup (name);
                    }
                    g_free (mp);
                }
                g_dir_close (d);
            }
            g_free (userdir);
        }
    }

    if (info) {
        if (!info->name || !*info->name) {
            g_free (info->name);
            info->name = g_strdup ("iPod");
        }
        fill_db_details (info);
    }
    return info;
}

/* ------------------------------------------------------------------ */
/* Track listing                                                       */
/* ------------------------------------------------------------------ */

void
ipod_track_free (IpodTrack *t)
{
    if (!t)
        return;
    g_free (t->artist);
    g_free (t->album);
    g_free (t->albumartist);
    g_free (t->title);
    g_free (t);
}

GList *
ipod_load_tracks (const gchar *mountpoint, GError **error)
{
    gchar *db = itunesdb_path (mountpoint);
    gboolean have = db && g_file_test (db, G_FILE_TEST_IS_REGULAR);
    g_free (db);
    if (!have)
        return NULL;

    /* libgpod sets a GError for harmless oddities (e.g. an empty
     * OTGPlaylistInfo) even when parsing succeeds; only report failures. */
    GError *err = NULL;
    Itdb_iTunesDB *itdb = itdb_parse (mountpoint, &err);
    if (!itdb) {
        g_propagate_error (error, err);
        return NULL;
    }
    g_clear_error (&err);

    GList *out = NULL;
    for (GList *l = itdb->tracks; l; l = l->next) {
        Itdb_Track *t = l->data;
        IpodTrack *o = g_new0 (IpodTrack, 1);
        o->id = t->id;
        o->artist = g_strdup (t->artist);
        o->album = g_strdup (t->album);
        o->albumartist = g_strdup (t->albumartist);
        o->title = g_strdup (t->title);
        o->track_nr = t->track_nr;
        o->disc_nr = t->cd_nr;
        o->size = t->size;
        o->length_ms = t->tracklen;
        o->compilation = t->compilation;
        out = g_list_prepend (out, o);
    }
    itdb_free (itdb);
    return g_list_reverse (out);
}

gchar *
ipod_track_key (const gchar *artist, const gchar *album, const gchar *title,
                gint track_nr, guint32 size)
{
    gchar *a = g_utf8_casefold (artist ? artist : "", -1);
    gchar *b = g_utf8_casefold (album ? album : "", -1);
    gchar *t = g_utf8_casefold (title ? title : "", -1);
    gchar *key = g_strdup_printf ("%s\x1f%s\x1f%s\x1f%d\x1f%u", a, b, t, track_nr, size);
    g_free (a);
    g_free (b);
    g_free (t);
    return key;
}

static gchar *
track_key_of (const Itdb_Track *t)
{
    return ipod_track_key (t->artist, t->album, t->title, t->track_nr, t->size);
}

/* ------------------------------------------------------------------ */
/* Jobs                                                                */
/* ------------------------------------------------------------------ */

typedef enum { JOB_UPLOAD, JOB_REMOVE } JobKind;

struct _IpodJob {
    JobKind           kind;
    gchar            *mountpoint;
    GPtrArray        *items;          /* IpodUploadItem */
    GArray           *ids;            /* guint32 */
    IpodProgressFunc  callback;
    gpointer          user_data;
    GThread          *thread;
    gint              cancelled;      /* atomic */
    gint              done, total, added, skipped, failed, removed;
    gint64            last_item_report;
};

static void
upload_item_free (gpointer p)
{
    IpodUploadItem *it = p;
    g_free (it->path);
    g_free (it);
}

static void
progress_free (IpodProgress *p)
{
    g_free (p->item_error);
    g_free (p->warning);
    g_free (p->error);
    g_free (p);
}

typedef struct {
    IpodJob      *job;
    IpodProgress *progress;
} Dispatch;

static gboolean
dispatch_in_main (gpointer data)
{
    Dispatch *d = data;
    d->job->callback (d->progress, d->job->user_data);
    progress_free (d->progress);
    g_free (d);
    return G_SOURCE_REMOVE;
}

static IpodProgress *
progress_new (IpodJob *u)
{
    IpodProgress *p = g_new0 (IpodProgress, 1);
    p->index = -1;
    p->done = u->done;
    p->total = u->total;
    p->added = u->added;
    p->skipped = u->skipped;
    p->failed = u->failed;
    p->removed = u->removed;
    return p;
}

static void
send (IpodJob *u, IpodProgress *p)
{
    Dispatch *d = g_new0 (Dispatch, 1);
    d->job = u;
    d->progress = p;
    g_idle_add_full (G_PRIORITY_DEFAULT, dispatch_in_main, d, NULL);
}

static void
report_item (IpodJob *u, gint index, IpodItemState state, gdouble fraction, const gchar *error)
{
    IpodProgress *p = progress_new (u);
    p->index = index;
    p->item_state = state;
    p->fraction = fraction;
    p->item_error = g_strdup (error);
    send (u, p);
}

static void
report_warning (IpodJob *u, const gchar *warning)
{
    IpodProgress *p = progress_new (u);
    p->warning = g_strdup (warning);
    send (u, p);
}

static void
report_finished (IpodJob *u, const gchar *error)
{
    IpodProgress *p = progress_new (u);
    p->finished = TRUE;
    p->error = g_strdup (error);
    send (u, p);
}

/* ------------------------------------------------------------------ */
/* Building an Itdb_Track from a local file                            */
/* ------------------------------------------------------------------ */

static const gchar *
filetype_for (const gchar *path)
{
    const gchar *dot = strrchr (path, '.');
    if (!dot)
        return "Audio file";
    dot++;
    if (!g_ascii_strcasecmp (dot, "mp3"))
        return "MPEG audio file";
    if (!g_ascii_strcasecmp (dot, "m4a") || !g_ascii_strcasecmp (dot, "aac") ||
        !g_ascii_strcasecmp (dot, "m4b"))
        return "AAC audio file";
    if (!g_ascii_strcasecmp (dot, "wav"))
        return "WAV audio file";
    return "AIFF audio file";
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

static gchar *
dup_nonempty (const gchar *s)
{
    return (s && *s) ? g_strdup (s) : NULL;
}

/* Try embedded picture first, then a cover image in the same directory. */
static void
set_artwork (Itdb_Track *track, TagLib_File *tf, const gchar *path)
{
    gboolean done = FALSE;
    TagLib_Complex_Property_Attribute ***props = taglib_complex_property_get (tf, "PICTURE");
    if (props) {
        TagLib_Complex_Property_Picture_Data pic = { 0 };
        taglib_picture_from_complex_property (props, &pic);
        if (pic.data && pic.size > 0)
            done = itdb_track_set_thumbnails_from_data (track, (const guchar *) pic.data, pic.size);
        taglib_complex_property_free (props);
    }
    if (done)
        return;

    static const gchar *const names[] = {
        "cover.jpg", "cover.png", "folder.jpg", "folder.png",
        "front.jpg", "album.jpg", "Cover.jpg", "Folder.jpg", NULL
    };
    gchar *dir = g_path_get_dirname (path);
    for (int i = 0; names[i] && !done; i++) {
        gchar *img = g_build_filename (dir, names[i], NULL);
        if (g_file_test (img, G_FILE_TEST_IS_REGULAR))
            done = itdb_track_set_thumbnails (track, img);
        g_free (img);
    }
    if (!done) {
        /* Any single image file in the directory. */
        GDir *d = g_dir_open (dir, 0, NULL);
        const gchar *name;
        while (d && !done && (name = g_dir_read_name (d))) {
            const gchar *dot = strrchr (name, '.');
            if (dot && (!g_ascii_strcasecmp (dot, ".jpg") || !g_ascii_strcasecmp (dot, ".jpeg") ||
                        !g_ascii_strcasecmp (dot, ".png"))) {
                gchar *img = g_build_filename (dir, name, NULL);
                done = itdb_track_set_thumbnails (track, img);
                g_free (img);
            }
        }
        if (d)
            g_dir_close (d);
    }
    g_free (dir);
}

static Itdb_Track *
track_from_file (const gchar *path, gboolean artwork, GError **error)
{
    TagLib_File *tf = taglib_file_new (path);
    if (!tf || !taglib_file_is_valid (tf)) {
        if (tf)
            taglib_file_free (tf);
        g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                     "unreadable or unsupported audio file");
        return NULL;
    }

    struct stat st;
    if (stat (path, &st) != 0) {
        taglib_file_free (tf);
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "%s", g_strerror (errno));
        return NULL;
    }

    Itdb_Track *t = itdb_track_new ();
    TagLib_Tag *tag = taglib_file_tag (tf);
    const TagLib_AudioProperties *ap = taglib_file_audioproperties (tf);

    if (tag) {
        t->title = dup_nonempty (taglib_tag_title (tag));
        t->artist = dup_nonempty (taglib_tag_artist (tag));
        t->album = dup_nonempty (taglib_tag_album (tag));
        t->genre = dup_nonempty (taglib_tag_genre (tag));
        t->comment = dup_nonempty (taglib_tag_comment (tag));
        t->year = (gint32) taglib_tag_year (tag);
        t->track_nr = (gint32) taglib_tag_track (tag);
    }
    if (!t->title) {
        gchar *base = g_path_get_basename (path);
        gchar *dot = strrchr (base, '.');
        if (dot)
            *dot = '\0';
        t->title = base;
    }

    t->albumartist = first_property (tf, "ALBUMARTIST");
    t->composer = first_property (tf, "COMPOSER");
    gchar *disc = first_property (tf, "DISCNUMBER");
    if (disc) {
        t->cd_nr = (gint32) g_ascii_strtoll (disc, NULL, 10);
        const gchar *slash = strchr (disc, '/');
        if (slash)
            t->cds = (gint32) g_ascii_strtoll (slash + 1, NULL, 10);
        g_free (disc);
    }
    gchar *total = first_property (tf, "TRACKTOTAL");
    if (total) {
        t->tracks = (gint32) g_ascii_strtoll (total, NULL, 10);
        g_free (total);
    }
    gchar *comp = first_property (tf, "COMPILATION");
    if (comp) {
        t->compilation = (g_ascii_strtoll (comp, NULL, 10) != 0);
        g_free (comp);
    }

    if (ap) {
        t->tracklen = taglib_audioproperties_length (ap) * 1000;
        t->bitrate = taglib_audioproperties_bitrate (ap);
        t->samplerate = (guint16) taglib_audioproperties_samplerate (ap);
    }

    t->size = (guint32) st.st_size;
    t->filetype = g_strdup (filetype_for (path));
    t->mediatype = ITDB_MEDIATYPE_AUDIO;
    t->time_added = time (NULL);
    t->time_modified = st.st_mtime;

    if (artwork)
        set_artwork (t, tf, path);

    taglib_tag_free_strings ();
    taglib_file_free (tf);

    /* TagLib guesses the length of VBR MP3s without a Xing header from the
     * first frame; count the frames instead. */
    const gchar *dot = strrchr (path, '.');
    if (dot && !g_ascii_strcasecmp (dot, ".mp3")) {
        gint ms, br;
        gboolean hdr;
        if (mp3_scan (path, &ms, &br, &hdr) && ms > 0) {
            t->tracklen = ms;
            t->bitrate = br;
        }
    }
    return t;
}

/* ------------------------------------------------------------------ */
/* Copying with progress                                               */
/* ------------------------------------------------------------------ */

static gboolean
copy_with_progress (IpodJob *u, gint index, const gchar *src, const gchar *dst, GError **error)
{
    int in = open (src, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "could not open source: %s", g_strerror (errno));
        return FALSE;
    }
    struct stat st;
    if (fstat (in, &st) != 0) {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno), "%s", g_strerror (errno));
        close (in);
        return FALSE;
    }
    int out = open (dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "could not create file on iPod: %s", g_strerror (errno));
        close (in);
        return FALSE;
    }

    gsize bufsize = 1 << 20;
    gchar *buf = g_malloc (bufsize);
    goffset copied = 0;
    gboolean ok = TRUE;
    while (ok) {
        gssize n = read (in, buf, bufsize);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                         "read error: %s", g_strerror (errno));
            ok = FALSE;
            break;
        }
        if (n == 0)
            break;
        gssize off = 0;
        while (off < n) {
            gssize w = write (out, buf + off, n - off);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                             "write error: %s", g_strerror (errno));
                ok = FALSE;
                break;
            }
            off += w;
        }
        copied += n;
        gint64 now = g_get_monotonic_time ();
        if (st.st_size > 0 && now - u->last_item_report > 80 * 1000) {
            u->last_item_report = now;
            report_item (u, index, IPOD_ITEM_ACTIVE, (gdouble) copied / st.st_size, NULL);
        }
    }
    g_free (buf);
    if (ok && fsync (out) != 0 && errno != EINVAL) {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "flush error: %s", g_strerror (errno));
        ok = FALSE;
    }
    close (in);
    if (close (out) != 0 && ok) {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "close error: %s", g_strerror (errno));
        ok = FALSE;
    }
    if (!ok)
        g_unlink (dst);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Database helpers                                                    */
/* ------------------------------------------------------------------ */

static Itdb_iTunesDB *
open_db (IpodJob *u, gboolean create, gchar **error_out)
{
    GError *err = NULL;
    gchar *db = itunesdb_path (u->mountpoint);
    gboolean have = db && g_file_test (db, G_FILE_TEST_IS_REGULAR);
    g_free (db);

    if (!have) {
        if (!create) {
            *error_out = g_strdup ("The iPod has no music database.");
            return NULL;
        }
        report_warning (u, "No iTunesDB found; initialising a fresh database on the iPod.");
        if (!itdb_init_ipod (u->mountpoint, NULL, "iPod", &err)) {
            *error_out = g_strdup_printf ("Could not initialise the iPod: %s",
                                          err ? err->message : "unknown error");
            g_clear_error (&err);
            return NULL;
        }
    }
    Itdb_iTunesDB *itdb = itdb_parse (u->mountpoint, &err);
    if (!itdb) {
        *error_out = g_strdup_printf ("Could not read the iPod database: %s",
                                      err ? err->message : "unknown error");
        g_clear_error (&err);
    }
    return itdb;
}

static Itdb_Track *
find_track (Itdb_iTunesDB *itdb, guint32 id)
{
    for (GList *l = itdb->tracks; l; l = l->next)
        if (((Itdb_Track *) l->data)->id == id)
            return l->data;
    return NULL;
}

/* Deletes the file, drops the track from every playlist and the DB. */
static void
delete_track (Itdb_iTunesDB *itdb, Itdb_Track *t)
{
    gchar *fn = itdb_filename_on_ipod (t);
    if (fn) {
        g_unlink (fn);
        g_free (fn);
    }
    for (GList *l = itdb->playlists; l; l = l->next) {
        Itdb_Playlist *pl = l->data;
        while (itdb_playlist_contains_track (pl, t))
            itdb_playlist_remove_track (pl, t);
    }
    itdb_track_remove_thumbnails (t);
    itdb_track_remove (t);
}

static gchar *
write_db (Itdb_iTunesDB *itdb)
{
    GError *err = NULL;
    gchar *error = NULL;
    if (!itdb_write (itdb, &err)) {
        error = g_strdup_printf ("Writing the iPod database failed: %s",
                                 err ? err->message : "unknown error");
        g_clear_error (&err);
    }
    sync ();
    return error;
}

/* ------------------------------------------------------------------ */
/* Upload thread                                                       */
/* ------------------------------------------------------------------ */

static gpointer
upload_thread (gpointer data)
{
    IpodJob *u = data;
    GError *err = NULL;
    gchar *fatal = NULL;

    u->total = (gint) u->items->len;
    Itdb_iTunesDB *itdb = open_db (u, TRUE, &fatal);
    if (!itdb) {
        report_finished (u, fatal);
        g_free (fatal);
        return NULL;
    }

    Itdb_Playlist *mpl = itdb_playlist_mpl (itdb);
    if (!mpl) {
        mpl = itdb_playlist_new ("iPod", FALSE);
        itdb_playlist_set_mpl (mpl);
        itdb_playlist_add (itdb, mpl, -1);
    }
    gboolean artwork = itdb_device_supports_artwork (itdb->device);

    /* Index what is already there so unexpected duplicates are skipped. */
    GHashTable *existing = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (GList *l = itdb->tracks; l; l = l->next)
        g_hash_table_insert (existing, track_key_of (l->data), l->data);

    itdb_start_sync (itdb);
    gboolean dirty = FALSE;

    for (guint i = 0; i < u->items->len; i++) {
        IpodUploadItem *item = g_ptr_array_index (u->items, i);
        const gchar *path = item->path;

        if (g_atomic_int_get (&u->cancelled)) {
            report_warning (u, "Cancelled; saving what has been copied so far.");
            break;
        }

        report_item (u, i, IPOD_ITEM_ACTIVE, 0.0, NULL);

        Itdb_Track *t = track_from_file (path, artwork, &err);
        if (!t) {
            u->failed++;
            u->done++;
            report_item (u, i, IPOD_ITEM_FAILED, 0.0, err ? err->message : "unknown error");
            g_clear_error (&err);
            continue;
        }

        gchar *key = track_key_of (t);
        Itdb_Track *old = item->replace_id ? find_track (itdb, item->replace_id) : NULL;
        if (!old)
            old = g_hash_table_lookup (existing, key);
        if (old && !item->replace_id) {
            /* Already there and no replacement requested. */
            u->skipped++;
            u->done++;
            itdb_track_free (t);
            g_free (key);
            report_item (u, i, IPOD_ITEM_SKIPPED, 1.0, NULL);
            continue;
        }

        gchar *dest = itdb_cp_get_dest_filename (NULL, u->mountpoint, path, &err);
        gboolean ok = dest != NULL;
        if (ok)
            ok = copy_with_progress (u, i, path, dest, &err);
        if (ok)
            ok = itdb_cp_finalize (t, u->mountpoint, dest, &err) != NULL;
        if (!ok) {
            u->failed++;
            u->done++;
            gchar *msg = g_strdup (err ? err->message : "unknown error");
            g_clear_error (&err);
            if (dest)
                g_unlink (dest);
            g_free (dest);
            itdb_track_free (t);
            g_free (key);
            report_item (u, i, IPOD_ITEM_FAILED, 0.0, msg);
            g_free (msg);
            /* A copy failure usually means the disk is full or gone;
             * keep going only if the mountpoint still exists. */
            if (!g_file_test (u->mountpoint, G_FILE_TEST_IS_DIR)) {
                report_warning (u, "The iPod disappeared; stopping.");
                break;
            }
            continue;
        }
        g_free (dest);

        if (old) {
            /* Keep listening history when replacing a track. */
            t->rating = old->rating;
            t->playcount = old->playcount;
            t->playcount2 = old->playcount2;
            t->time_played = old->time_played;
            t->time_added = old->time_added;
            gchar *okey = track_key_of (old);
            g_hash_table_remove (existing, okey);
            g_free (okey);
            delete_track (itdb, old);
        }

        itdb_track_add (itdb, t, -1);
        itdb_playlist_add_track (mpl, t, -1);
        g_hash_table_insert (existing, key, t);
        u->added++;
        u->done++;
        dirty = TRUE;
        report_item (u, i, IPOD_ITEM_DONE, 1.0, NULL);
    }

    if (dirty) {
        report_warning (u, NULL);          /* flushes counters to the UI */
        fatal = write_db (itdb);
    }
    itdb_stop_sync (itdb);
    g_hash_table_destroy (existing);
    itdb_free (itdb);

    report_finished (u, fatal);
    g_free (fatal);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Remove thread                                                       */
/* ------------------------------------------------------------------ */

static gpointer
remove_thread (gpointer data)
{
    IpodJob *u = data;
    gchar *fatal = NULL;

    u->total = (gint) u->ids->len;
    Itdb_iTunesDB *itdb = open_db (u, FALSE, &fatal);
    if (!itdb) {
        report_finished (u, fatal);
        g_free (fatal);
        return NULL;
    }

    itdb_start_sync (itdb);
    gboolean dirty = FALSE;
    for (guint i = 0; i < u->ids->len; i++) {
        if (g_atomic_int_get (&u->cancelled)) {
            report_warning (u, "Cancelled; saving what has been removed so far.");
            break;
        }
        guint32 id = g_array_index (u->ids, guint32, i);
        report_item (u, i, IPOD_ITEM_ACTIVE, 0.0, NULL);
        Itdb_Track *t = find_track (itdb, id);
        if (!t) {
            u->failed++;
            u->done++;
            report_item (u, i, IPOD_ITEM_FAILED, 0.0, "track is no longer in the database");
            continue;
        }
        delete_track (itdb, t);
        dirty = TRUE;
        u->removed++;
        u->done++;
        report_item (u, i, IPOD_ITEM_DONE, 1.0, NULL);
    }

    if (dirty)
        fatal = write_db (itdb);
    itdb_stop_sync (itdb);
    itdb_free (itdb);

    report_finished (u, fatal);
    g_free (fatal);
    return NULL;
}

/* ------------------------------------------------------------------ */

static IpodJob *
job_new (JobKind kind, const gchar *mountpoint, IpodProgressFunc callback, gpointer user_data)
{
    IpodJob *u = g_new0 (IpodJob, 1);
    u->kind = kind;
    u->mountpoint = g_strdup (mountpoint);
    u->callback = callback;
    u->user_data = user_data;
    return u;
}

IpodJob *
ipod_upload_start (const gchar *mountpoint, GPtrArray *items,
                   IpodProgressFunc callback, gpointer user_data)
{
    IpodJob *u = job_new (JOB_UPLOAD, mountpoint, callback, user_data);
    u->items = items;
    g_ptr_array_set_free_func (items, upload_item_free);
    u->thread = g_thread_new ("ipod-upload", upload_thread, u);
    return u;
}

IpodJob *
ipod_remove_start (const gchar *mountpoint, GArray *ids,
                   IpodProgressFunc callback, gpointer user_data)
{
    IpodJob *u = job_new (JOB_REMOVE, mountpoint, callback, user_data);
    u->ids = ids;
    u->thread = g_thread_new ("ipod-remove", remove_thread, u);
    return u;
}

void
ipod_job_cancel (IpodJob *job)
{
    if (job)
        g_atomic_int_set (&job->cancelled, 1);
}

void
ipod_job_free (IpodJob *job)
{
    if (!job)
        return;
    if (job->thread)
        g_thread_join (job->thread);
    if (job->items)
        g_ptr_array_unref (job->items);
    if (job->ids)
        g_array_unref (job->ids);
    g_free (job->mountpoint);
    g_free (job);
}
