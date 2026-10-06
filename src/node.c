#include "node.h"
#include "fuzzy.h"
#include <string.h>

enum { SIG_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_TYPE (IuNode, iu_node, G_TYPE_OBJECT)

static void
iu_node_finalize (GObject *obj)
{
    IuNode *n = IU_NODE (obj);
    if (n->children) {
        /* Children may outlive us briefly (bound rows); drop back-pointers. */
        guint c = g_list_model_get_n_items (G_LIST_MODEL (n->children));
        for (guint i = 0; i < c; i++) {
            IuNode *ch = g_list_model_get_item (G_LIST_MODEL (n->children), i);
            ch->parent = NULL;
            g_object_unref (ch);
        }
        g_object_unref (n->children);
    }
    g_free (n->name);
    g_free (n->sort_key);
    g_free (n->group_id);
    g_free (n->detail);
    g_free (n->search_text);
    g_free (n->path);
    g_free (n->error);
    G_OBJECT_CLASS (iu_node_parent_class)->finalize (obj);
}

static void
iu_node_class_init (IuNodeClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = iu_node_finalize;
    signals[SIG_CHANGED] = g_signal_new ("changed", IU_TYPE_NODE, G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
iu_node_init (IuNode *n)
{
}

IuNode *
iu_node_new (IuNodeKind kind, const gchar *name)
{
    IuNode *n = g_object_new (IU_TYPE_NODE, NULL);
    n->kind = kind;
    n->name = g_strdup (name ? name : "");
    n->sort_key = g_utf8_collate_key_for_filename (n->name, -1);
    if (kind != IU_NODE_SONG)
        n->children = g_list_store_new (IU_TYPE_NODE);
    return n;
}

static gint
compare_nodes (gconstpointer a, gconstpointer b, gpointer user_data)
{
    const IuNode *x = a, *y = b;
    return strcmp (x->sort_key, y->sort_key);
}

void
iu_node_add_child (IuNode *parent, IuNode *child)
{
    g_return_if_fail (parent->children);
    child->parent = parent;
    g_list_store_insert_sorted (parent->children, child, compare_nodes, NULL);
    g_object_unref (child);
}

guint
iu_node_n_children (IuNode *parent)
{
    return parent->children ? g_list_model_get_n_items (G_LIST_MODEL (parent->children)) : 0;
}

/* Borrowed reference. */
IuNode *
iu_node_child (IuNode *parent, guint i)
{
    IuNode *c = g_list_model_get_item (G_LIST_MODEL (parent->children), i);
    g_object_unref (c);
    return c;
}

IuNode *
iu_node_find_child (IuNode *parent, const gchar *name)
{
    guint c = iu_node_n_children (parent);
    for (guint i = 0; i < c; i++) {
        IuNode *ch = iu_node_child (parent, i);
        if (g_strcmp0 (ch->group_id ? ch->group_id : ch->name, name) == 0)
            return ch;
    }
    return NULL;
}

void
iu_node_changed (IuNode *node, gboolean bubble)
{
    for (IuNode *n = node; n; n = bubble ? n->parent : NULL)
        g_signal_emit (n, signals[SIG_CHANGED], 0);
}

static void
set_selected_rec (IuNode *node, gboolean selected)
{
    if (node->kind == IU_NODE_SONG) {
        node->selected = selected;
    } else {
        guint c = iu_node_n_children (node);
        for (guint i = 0; i < c; i++)
            set_selected_rec (iu_node_child (node, i), selected);
    }
    iu_node_changed (node, FALSE);
}

void
iu_node_set_selected (IuNode *node, gboolean selected)
{
    set_selected_rec (node, selected);
    if (node->parent)
        iu_node_changed (node->parent, TRUE);
}

gint
iu_node_check_state (IuNode *node)
{
    if (node->kind == IU_NODE_SONG)
        return node->selected ? 1 : 0;
    guint total = 0, sel = 0;
    guint c = iu_node_n_children (node);
    for (guint i = 0; i < c; i++) {
        IuNode *ch = iu_node_child (node, i);
        gint s = iu_node_check_state (ch);
        total++;
        if (s == 2)
            return 2;
        sel += s;
    }
    if (total == 0 || sel == 0)
        return 0;
    return sel == total ? 1 : 2;
}

void
iu_node_set_state (IuNode *node, IuState state, gdouble progress, const gchar *error)
{
    node->state = state;
    node->progress = CLAMP (progress, 0.0, 1.0);
    if (error != node->error) {
        g_free (node->error);
        node->error = g_strdup (error);
    }
    iu_node_changed (node, TRUE);
}

void
iu_node_foreach_song (IuNode *node, GFunc fn, gpointer data)
{
    if (node->kind == IU_NODE_SONG) {
        fn (node, data);
        return;
    }
    guint c = iu_node_n_children (node);
    for (guint i = 0; i < c; i++)
        iu_node_foreach_song (iu_node_child (node, i), fn, data);
}

void
iu_node_foreach (IuNode *node, GFunc fn, gpointer data)
{
    fn (node, data);
    guint c = iu_node_n_children (node);
    for (guint i = 0; i < c; i++)
        iu_node_foreach (iu_node_child (node, i), fn, data);
}

typedef struct { gboolean selected_only; guint count; guint64 bytes; guint on_ipod; gint length; double progress; } Stats;

static void
stats_cb (gpointer item, gpointer data)
{
    IuNode *s = item;
    Stats *st = data;
    if (s->on_ipod)
        st->on_ipod++;
    if (st->selected_only && !s->selected)
        return;
    st->count++;
    st->bytes += s->size;
    st->length += s->length_ms;
    switch (s->state) {
    case IU_STATE_ACTIVE:  st->progress += s->progress; break;
    case IU_STATE_DONE:
    case IU_STATE_SKIPPED:
    case IU_STATE_FAILED:  st->progress += 1.0; break;
    default: break;
    }
}

guint
iu_node_count_songs (IuNode *node, gboolean selected_only, guint64 *bytes)
{
    Stats st = { selected_only, 0, 0, 0, 0, 0 };
    iu_node_foreach_song (node, stats_cb, &st);
    if (bytes)
        *bytes = st.bytes;
    return st.count;
}

guint
iu_node_count_on_ipod (IuNode *node)
{
    Stats st = { FALSE, 0, 0, 0, 0, 0 };
    iu_node_foreach_song (node, stats_cb, &st);
    return st.on_ipod;
}

gint
iu_node_total_length (IuNode *node)
{
    Stats st = { FALSE, 0, 0, 0, 0, 0 };
    iu_node_foreach_song (node, stats_cb, &st);
    return st.length;
}

gdouble
iu_node_progress (IuNode *node)
{
    Stats st = { TRUE, 0, 0, 0, 0, 0 };
    iu_node_foreach_song (node, stats_cb, &st);
    return st.count ? st.progress / st.count : 0.0;
}

static gboolean
subtree_matches (IuNode *node, const gchar *query, guint gen)
{
    if (node->query_gen == gen)
        return node->subtree_match;
    node->query_gen = gen;
    node->self_match = fuzzy_score (node->search_text, query) >= 0;
    node->subtree_match = node->self_match;
    if (!node->subtree_match && node->children) {
        guint c = iu_node_n_children (node);
        for (guint i = 0; i < c && !node->subtree_match; i++)
            if (subtree_matches (iu_node_child (node, i), query, gen))
                node->subtree_match = TRUE;
    }
    return node->subtree_match;
}

gboolean
iu_node_matches (IuNode *node, const gchar *query, guint gen)
{
    if (!query || !*query)
        return TRUE;
    /* Everything under a matching artist/album is shown. */
    for (IuNode *p = node->parent; p; p = p->parent) {
        subtree_matches (p, query, gen);
        if (p->self_match)
            return TRUE;
    }
    return subtree_matches (node, query, gen);
}
