/* Tree nodes (artist > album > song) shown in the Upload and Manage lists. */
#pragma once
#include <glib-object.h>
#include <gio/gio.h>

typedef enum { IU_NODE_ARTIST, IU_NODE_ALBUM, IU_NODE_SONG } IuNodeKind;

typedef enum {
    IU_STATE_IDLE,        /* nothing happening */
    IU_STATE_QUEUED,      /* selected, waiting for its turn */
    IU_STATE_ACTIVE,      /* being copied / removed right now */
    IU_STATE_DONE,
    IU_STATE_SKIPPED,
    IU_STATE_FAILED
} IuState;

#define IU_TYPE_NODE (iu_node_get_type ())
G_DECLARE_FINAL_TYPE (IuNode, iu_node, IU, NODE, GObject)

struct _IuNode {
    GObject     parent_instance;
    IuNodeKind  kind;
    gchar      *name;
    gchar      *sort_key;         /* collate key; songs encode disc/track too */
    gchar      *search_text;      /* casefolded "artist album title" */
    gchar      *group_id;         /* identity for find_child (defaults to name) */
    gchar      *detail;           /* context not shown by ancestors, e.g. artist */
    GListStore *children;         /* IuNode items; NULL for songs */
    IuNode     *parent;           /* unowned */

    /* song fields */
    gchar      *path;             /* local file (Upload page) */
    guint32     ipod_id;          /* track id on the iPod, 0 if none */
    guint64     size;
    gint        track_nr;
    gint        disc_nr;
    gint        length_ms;        /* local length (Upload) / iPod length (Manage) */
    gint        ipod_length_ms;   /* Upload page: length stored on the iPod */
    gboolean    on_ipod;
    gboolean    length_differs;

    /* mutable state */
    gboolean    selected;
    IuState     state;
    gdouble     progress;
    gchar      *error;

    /* search memo */
    guint       query_gen;
    gboolean    self_match;
    gboolean    subtree_match;
};

IuNode  *iu_node_new           (IuNodeKind kind, const gchar *name);
void     iu_node_add_child     (IuNode *parent, IuNode *child);   /* sorted; takes ownership */
IuNode  *iu_node_find_child    (IuNode *parent, const gchar *name);
IuNode  *iu_node_child         (IuNode *parent, guint i);
guint    iu_node_n_children    (IuNode *parent);

/* Emits "changed" on @node and, if @bubble, on all its ancestors. */
void     iu_node_changed       (IuNode *node, gboolean bubble);

/* Selection. Groups apply recursively. */
void     iu_node_set_selected  (IuNode *node, gboolean selected);
/* 0 = none, 1 = all, 2 = some (for songs, 0 or 1). */
gint     iu_node_check_state   (IuNode *node);

void     iu_node_set_state     (IuNode *node, IuState state, gdouble progress, const gchar *error);

/* Recursive song statistics. */
guint    iu_node_count_songs   (IuNode *node, gboolean selected_only, guint64 *bytes);
guint    iu_node_count_on_ipod (IuNode *node);
gint     iu_node_total_length  (IuNode *node);
gdouble  iu_node_progress      (IuNode *node);   /* mean over selected songs */
void     iu_node_foreach_song  (IuNode *node, GFunc fn, gpointer data);
void     iu_node_foreach       (IuNode *node, GFunc fn, gpointer data);

/* Search. @query is fuzzy_prepare'd; @gen increments per query. */
gboolean iu_node_matches       (IuNode *node, const gchar *query, guint gen);
