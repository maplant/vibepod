/* iPod discovery, database handling and the background jobs. */
#pragma once
#include <glib.h>
#include <gio/gio.h>

typedef struct {
    gchar   *mountpoint;      /* e.g. /run/media/user/IPOD */
    gchar   *device_node;     /* whole-disk node, e.g. /dev/sda (may be NULL) */
    gchar   *name;            /* volume label or "iPod" */
    gchar   *model;           /* human readable model name (may be NULL) */
    gchar   *model_number;    /* e.g. "MB147" (may be NULL) */
    double   capacity_gb;     /* 0 if unknown */
    guint64  free_bytes;
    guint64  total_bytes;
    gboolean has_itunesdb;
    gboolean has_sysinfo_extended;
} IpodInfo;

void      ipod_info_free (IpodInfo *info);

/* Looks through mounted volumes for one that contains an iPod_Control
 * directory. Returns NULL if no iPod is mounted. */
IpodInfo *ipod_find (GVolumeMonitor *monitor);

/* Path of SysInfoExtended for a mountpoint. */
gchar    *ipod_sysinfo_extended_path (const gchar *mountpoint);

/* A track as stored in the iTunesDB. */
typedef struct {
    guint32  id;
    gchar   *artist;
    gchar   *album;
    gchar   *albumartist;
    gchar   *title;
    gint     track_nr;
    gint     disc_nr;
    guint32  size;
    gint     length_ms;
    gboolean compilation;
} IpodTrack;

void      ipod_track_free (IpodTrack *t);

/* Reads every track from the iPod's database. Returns NULL and sets
 * @error on failure; an iPod without a database yields an empty list.
 * Safe to call from a worker thread. */
GList    *ipod_load_tracks (const gchar *mountpoint, GError **error);

/* Key used to decide whether a local song is already on the iPod. */
gchar    *ipod_track_key (const gchar *artist, const gchar *album,
                          const gchar *title, gint track_nr, guint32 size);

/* ---- Background jobs -------------------------------------------- */

typedef enum {
    IPOD_ITEM_QUEUED,
    IPOD_ITEM_ACTIVE,
    IPOD_ITEM_DONE,
    IPOD_ITEM_SKIPPED,
    IPOD_ITEM_FAILED
} IpodItemState;

typedef struct {
    gint     index;           /* item this refers to, -1 if none */
    IpodItemState item_state;
    gdouble  fraction;        /* progress of the item, 0..1 */
    gchar   *item_error;      /* why the item failed (may be NULL) */

    gint     done, total;     /* items processed so far */
    gint     added, skipped, failed, removed;

    gchar   *warning;         /* non-fatal notice for the user (may be NULL) */
    gboolean finished;
    gchar   *error;           /* fatal error, set when finished && failure */
} IpodProgress;

typedef void (*IpodProgressFunc) (const IpodProgress *progress, gpointer user_data);

typedef struct _IpodJob IpodJob;

typedef struct {
    gchar   *path;            /* local file to copy */
    guint32  replace_id;      /* existing iPod track to replace, 0 for none */
} IpodUploadItem;

/* Starts copying @items (GPtrArray of IpodUploadItem, owned by the job)
 * to the iPod at @mountpoint in a worker thread. @callback is invoked in
 * the main context; the final invocation has finished == TRUE. */
IpodJob    *ipod_upload_start (const gchar *mountpoint,
                               GPtrArray *items,
                               IpodProgressFunc callback,
                               gpointer user_data);

/* Removes the tracks with the given ids (GArray of guint32, owned by the
 * job) from the iPod, deleting their files. */
IpodJob    *ipod_remove_start (const gchar *mountpoint,
                               GArray *ids,
                               IpodProgressFunc callback,
                               gpointer user_data);

/* Ask the job to stop after the current item. Changes made so far are
 * still written to the database. */
void        ipod_job_cancel (IpodJob *job);

/* Waits for the thread and frees the job. Safe to call after finished. */
void        ipod_job_free (IpodJob *job);
