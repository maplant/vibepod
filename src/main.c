/* iPod Uploader: sync ~/Music to the connected iPod and manage its songs. */
#include <adwaita.h>
#include <glib/gi18n.h>
#include "ipod.h"
#include "library.h"
#include "node.h"
#include "fuzzy.h"

#define APP_ID "dev.map.IpodUploader"

typedef struct _App App;

/* How a list is grouped: up to two levels of these, in order. */
typedef enum { GROUP_ARTIST, GROUP_ALBUM, N_GROUP_KEYS } GroupKey;
static const gchar *const group_key_ids[N_GROUP_KEYS] = { "artist", "album" };

/* One of the two list pages (Upload / Manage). */
typedef struct {
    App              *app;
    gboolean          manage;
    GroupKey          groups[N_GROUP_KEYS];
    gint              n_groups;
    GtkDropDown      *dropdown;          /* grouping selector */
    gboolean          updating_control;
    GtkStack         *stack;             /* "loading" / "empty" / "list" */
    AdwStatusPage    *empty;
    GtkLabel         *loading_label;
    GtkSearchEntry   *search;
    GtkListView      *list;
    GtkTreeListModel *tree;
    GtkCustomFilter  *filter;
    IuNode           *root;
    gchar            *query;             /* fuzzy_prepare'd search text */
    guint             query_gen;
    GtkLabel         *summary;
    GtkProgressBar   *progress;
    GtkLabel         *current;
    GtkButton        *action_btn;
    GtkButton        *cancel_btn;
} Page;

struct _App {
    GtkApplication  *gapp;
    GtkWindow       *window;
    GVolumeMonitor  *monitor;

    IpodInfo        *ipod;
    gchar           *library_root;
    GList           *songs;              /* LibrarySong, from the last scan */
    GList           *ipod_tracks;        /* IpodTrack, from the last scan */
    GHashTable      *by_key;             /* track key -> IpodTrack */
    GKeyFile        *settings;
    gchar           *settings_path;

    AdwToastOverlay *toasts;
    AdwBanner       *banner;
    GtkLabel        *device_label;
    GtkLabel        *library_label;
    GtkButton       *refresh_btn;
    GtkButton       *eject_btn;
    gboolean         ejecting;
    AdwViewStack    *stack;
    Page             upload;
    Page             manage;

    /* background work */
    guint            plan_seq;
    gboolean         planning;
    IpodJob         *job;
    Page            *job_page;
    GPtrArray       *job_nodes;          /* IuNode refs in job order */
    GPtrArray       *job_failures;       /* strings */
    gboolean         close_after_job;
};

static void refresh (App *a);
static void update_summary (Page *pg);
static void set_all_expanded (Page *pg, gboolean expanded);

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void
toast (App *a, const gchar *text)
{
    adw_toast_overlay_add_toast (a->toasts, adw_toast_new (text));
}

static void
on_copy_details (GtkButton *btn, gpointer user_data)
{
    App *a = user_data;
    const gchar *text = g_object_get_data (G_OBJECT (btn), "text");
    gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (btn)), text);
    toast (a, _("Copied to clipboard"));
}

/* Error/warning dialog: @heading, a one-line @summary and optional
 * @details shown left-aligned in a selectable, scrollable box with a
 * Copy button. */
static void
show_error (App *a, const gchar *heading, const gchar *summary, const gchar *details)
{
    AdwDialog *dlg = adw_alert_dialog_new (heading, summary);
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dlg), "ok", _("Close"));
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "ok");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "ok");

    if (details && *details) {
        GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);

        GtkWidget *label = gtk_label_new (details);
        gtk_label_set_xalign (GTK_LABEL (label), 0);
        gtk_label_set_yalign (GTK_LABEL (label), 0);
        gtk_label_set_wrap (GTK_LABEL (label), TRUE);
        gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_selectable (GTK_LABEL (label), TRUE);
        gtk_widget_add_css_class (label, "monospace");
        gtk_widget_add_css_class (label, "caption");
        gtk_widget_set_margin_start (label, 12);
        gtk_widget_set_margin_end (label, 12);
        gtk_widget_set_margin_top (label, 10);
        gtk_widget_set_margin_bottom (label, 10);

        GtkWidget *scroller = gtk_scrolled_window_new ();
        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroller), TRUE);
        gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroller), 280);
        gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), label);
        gtk_widget_add_css_class (scroller, "card");
        gtk_widget_add_css_class (scroller, "frame");
        gtk_box_append (GTK_BOX (box), scroller);

        GtkWidget *copy = gtk_button_new ();
        GtkWidget *content = adw_button_content_new ();
        adw_button_content_set_icon_name (ADW_BUTTON_CONTENT (content), "edit-copy-symbolic");
        adw_button_content_set_label (ADW_BUTTON_CONTENT (content), _("Copy"));
        gtk_button_set_child (GTK_BUTTON (copy), content);
        gtk_widget_add_css_class (copy, "flat");
        gtk_widget_set_halign (copy, GTK_ALIGN_END);
        g_object_set_data_full (G_OBJECT (copy), "text", g_strdup (details), g_free);
        g_signal_connect (copy, "clicked", G_CALLBACK (on_copy_details), a);
        gtk_box_append (GTK_BOX (box), copy);

        adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dlg), box);
        adw_dialog_set_content_width (dlg, 560);
    }
    adw_dialog_present (dlg, GTK_WIDGET (a->window));
}

/* Joins detail lines for show_error. */
static gchar *
join_lines (GPtrArray *lines)
{
    GString *s = g_string_new (NULL);
    for (guint i = 0; i < lines->len; i++) {
        if (i)
            g_string_append_c (s, '\n');
        g_string_append (s, g_ptr_array_index (lines, i));
    }
    return g_string_free (s, FALSE);
}

static gchar *
format_duration (gint ms)
{
    gint s = (ms + 500) / 1000;
    if (s >= 3600)
        return g_strdup_printf ("%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    return g_strdup_printf ("%d:%02d", s / 60, s % 60);
}

static const gchar *
nonempty (const gchar *s, const gchar *fallback)
{
    return (s && *s) ? s : fallback;
}

static const gchar *
group_key_label (GroupKey k, gboolean plural)
{
    if (k == GROUP_ARTIST)
        return plural ? _("Artists") : _("Artist");
    return plural ? _("Albums") : _("Album");
}

/* "Artist › Album", "Albums", "Songs"… */
static gchar *
groups_label (const GroupKey *groups, gint n)
{
    if (n == 0)
        return g_strdup (_("Songs"));
    if (n == 1)
        return g_strdup (group_key_label (groups[0], TRUE));
    return g_strdup_printf ("%s › %s", group_key_label (groups[0], FALSE),
                            group_key_label (groups[1], FALSE));
}

typedef struct {
    const gchar *artist, *albumartist, *album, *title;
    gint track_nr, disc_nr;
    gboolean compilation;
} SongMeta;

/* Albums without an album artist whose tracks have different artists are
 * treated as compilations. Keyed by @scope (e.g. the directory) + album. */
static gchar *
album_scope_key (const gchar *scope, const gchar *album)
{
    gchar *folded = g_utf8_casefold (nonempty (album, ""), -1);
    gchar *key = g_strconcat (scope ? scope : "", "\x1f", folded, NULL);
    g_free (folded);
    return key;
}

static void
note_album_artist (GHashTable *albums, const gchar *scope, const gchar *album,
                   const gchar *artist, const gchar *albumartist)
{
    if (albumartist && *albumartist)
        return;
    gchar *key = album_scope_key (scope, album);
    const gchar *seen = g_hash_table_lookup (albums, key);
    if (!seen)
        g_hash_table_insert (albums, key, g_strdup (nonempty (artist, "")));
    else {
        if (*seen && g_strcmp0 (seen, nonempty (artist, "")) != 0)
            g_hash_table_insert (albums, key, g_strdup (""));   /* "" = several artists */
        else
            g_free (key);
    }
}

static gboolean
album_has_many_artists (GHashTable *albums, const gchar *scope, const gchar *album)
{
    gchar *key = album_scope_key (scope, album);
    const gchar *seen = g_hash_table_lookup (albums, key);
    g_free (key);
    return seen && !*seen;
}

/* Files a song under the page's group levels, creating group rows as
 * needed, and returns the new song node. */
static IuNode *
add_song (Page *pg, IuNode *root, const SongMeta *m)
{
    const gchar *artist = nonempty (m->albumartist,
                                    m->compilation ? _("Various Artists")
                                                   : nonempty (m->artist, _("Unknown Artist")));
    const gchar *album = nonempty (m->album, _("Unknown Album"));
    gboolean have_artist = FALSE, have_album = FALSE;
    IuNode *parent = root;

    for (gint i = 0; i < pg->n_groups; i++) {
        GroupKey k = pg->groups[i];
        const gchar *name = (k == GROUP_ARTIST) ? artist : album;
        /* An album row that is not under its artist must not merge with a
         * same-named album by someone else, and shows the artist itself. */
        gboolean qualify = (k == GROUP_ALBUM && !have_artist);
        gchar *id = qualify ? g_strconcat (album, "\x1f", artist, NULL) : g_strdup (name);
        IuNode *g = iu_node_find_child (parent, id);
        if (!g) {
            g = iu_node_new (k == GROUP_ARTIST ? IU_NODE_ARTIST : IU_NODE_ALBUM, name);
            g->group_id = g_steal_pointer (&id);
            if (qualify) {
                g->detail = g_strdup (artist);
                gchar *ak = g_utf8_collate_key_for_filename (artist, -1);
                gchar *sk = g_strconcat (g->sort_key, "\x1f", ak, NULL);
                g_free (g->sort_key);
                g_free (ak);
                g->sort_key = sk;
            }
            gchar *txt = (k == GROUP_ARTIST) ? g_strdup (artist) : g_strconcat (artist, " ", album, NULL);
            g->search_text = fuzzy_prepare (txt);
            g_free (txt);
            iu_node_add_child (parent, g);
        }
        g_free (id);
        parent = g;
        have_artist |= (k == GROUP_ARTIST);
        have_album |= (k == GROUP_ALBUM);
    }

    IuNode *s = iu_node_new (IU_NODE_SONG, nonempty (m->title, _("Untitled")));
    gchar *txt = g_strconcat (artist, " ", nonempty (m->artist, ""), " ", album, " ", s->name, NULL);
    s->search_text = fuzzy_prepare (txt);
    g_free (txt);
    s->track_nr = m->track_nr;
    s->disc_nr = m->disc_nr;

    /* Whatever the group rows do not say goes into the subtitle. */
    const gchar *track_artist = nonempty (m->artist, artist);
    if (!have_artist && !have_album)
        s->detail = g_strdup_printf ("%s · %s", track_artist, album);
    else if (!have_artist)
        s->detail = g_strdup (track_artist);
    else if (!have_album)
        s->detail = g_strdup (album);

    /* Sort by what the groups leave open, then disc, track, title. */
    gchar *ak = have_artist ? g_strdup ("") : g_utf8_collate_key_for_filename (artist, -1);
    gchar *lk = have_album ? g_strdup ("") : g_utf8_collate_key_for_filename (album, -1);
    gchar *sk = g_strdup_printf ("%s\x1f%s\x1f%04d\x1f%04d\x1f%s", ak, lk,
                                 m->disc_nr, m->track_nr, s->sort_key);
    g_free (ak);
    g_free (lk);
    g_free (s->sort_key);
    s->sort_key = sk;

    iu_node_add_child (parent, s);
    return s;
}

/* ------------------------------------------------------------------ */
/* Rows                                                                */
/* ------------------------------------------------------------------ */

static void
on_check_toggled (GtkCheckButton *check, gpointer user_data)
{
    Page *pg = user_data;
    IuNode *node = g_object_get_data (G_OBJECT (check), "node");
    if (!node || pg->app->job)
        return;
    iu_node_set_selected (node, gtk_check_button_get_active (check));
}

static GtkWidget *
row_new (Page *pg)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top (box, 4);
    gtk_widget_set_margin_bottom (box, 4);
    gtk_widget_set_margin_start (box, 6);
    gtk_widget_set_margin_end (box, 6);

    GtkWidget *check = gtk_check_button_new ();
    gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
    g_signal_connect (check, "toggled", G_CALLBACK (on_check_toggled), pg);
    gtk_box_append (GTK_BOX (box), check);

    GtkWidget *icon = gtk_image_new ();
    gtk_widget_add_css_class (icon, "dim-label");
    gtk_box_append (GTK_BOX (box), icon);

    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (vbox, TRUE);
    gtk_widget_set_valign (vbox, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (box), vbox);

    GtkWidget *title = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (title), 0);
    gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (vbox), title);

    GtkWidget *subtitle = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (subtitle), 0);
    gtk_label_set_ellipsize (GTK_LABEL (subtitle), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class (subtitle, "caption");
    gtk_box_append (GTK_BOX (vbox), subtitle);

    GtkWidget *bar = gtk_progress_bar_new ();
    gtk_widget_set_margin_top (bar, 3);
    gtk_widget_set_visible (bar, FALSE);
    gtk_box_append (GTK_BOX (vbox), bar);

    GtkWidget *status = gtk_image_new ();
    gtk_widget_set_visible (status, FALSE);
    gtk_box_append (GTK_BOX (box), status);

    g_object_set_data (G_OBJECT (box), "check", check);
    g_object_set_data (G_OBJECT (box), "icon", icon);
    g_object_set_data (G_OBJECT (box), "title", title);
    g_object_set_data (G_OBJECT (box), "subtitle", subtitle);
    g_object_set_data (G_OBJECT (box), "bar", bar);
    g_object_set_data (G_OBJECT (box), "status", status);

    GtkWidget *expander = gtk_tree_expander_new ();
    gtk_tree_expander_set_child (GTK_TREE_EXPANDER (expander), box);
    return expander;
}

static void
set_css_class (GtkWidget *w, const gchar *cls, gboolean on)
{
    if (on)
        gtk_widget_add_css_class (w, cls);
    else
        gtk_widget_remove_css_class (w, cls);
}

static gchar *
song_subtitle (Page *pg, IuNode *n)
{
    GString *s = g_string_new (NULL);
    gchar *len = format_duration (n->length_ms);
    if (n->detail)
        g_string_append_printf (s, "%s · ", n->detail);
    if (n->track_nr > 0)
        g_string_append_printf (s, _("Track %d · "), n->track_nr);
    g_string_append (s, len);
    g_free (len);

    if (pg->manage) {
        gchar *size = g_format_size (n->size);
        g_string_append_printf (s, " · %s", size);
        g_free (size);
    } else if (!n->on_ipod) {
        g_string_append (s, _(" · New"));
    } else if (n->length_differs) {
        gchar *old = format_duration (n->ipod_length_ms);
        g_string_append_printf (s, _(" · On iPod with wrong length (%s)"), old);
        g_free (old);
    } else {
        g_string_append (s, _(" · Already on iPod"));
    }

    switch (n->state) {
    case IU_STATE_ACTIVE:
        g_string_append (s, pg->manage ? _(" · Removing…") : _(" · Uploading…"));
        break;
    case IU_STATE_DONE:
        g_string_append (s, pg->manage ? _(" · Removed") : _(" · Uploaded"));
        break;
    case IU_STATE_SKIPPED:
        g_string_append (s, _(" · Skipped, already on iPod"));
        break;
    case IU_STATE_FAILED:
        g_string_append_printf (s, _(" · Failed: %s"), n->error ? n->error : "?");
        break;
    default:
        break;
    }
    return g_string_free (s, FALSE);
}

static gchar *
group_subtitle (Page *pg, IuNode *n)
{
    guint64 bytes = 0;
    guint songs = iu_node_count_songs (n, FALSE, &bytes);
    GString *s = g_string_new (NULL);
    if (n->detail)
        g_string_append_printf (s, "%s · ", n->detail);
    if (n->kind == IU_NODE_ARTIST && iu_node_n_children (n) > 0 &&
        iu_node_child (n, 0)->kind == IU_NODE_ALBUM) {
        guint albums = iu_node_n_children (n);
        g_string_append_printf (s, ngettext ("%u album", "%u albums", albums), albums);
        g_string_append (s, " · ");
    }
    g_string_append_printf (s, ngettext ("%u song", "%u songs", songs), songs);
    if (pg->manage) {
        gchar *size = g_format_size (bytes);
        g_string_append_printf (s, " · %s", size);
        g_free (size);
    } else {
        gchar *len = format_duration (iu_node_total_length (n));
        g_string_append_printf (s, " · %s", len);
        g_free (len);
        guint on = iu_node_count_on_ipod (n);
        if (on == songs)
            g_string_append (s, _(" · all on iPod"));
        else if (on > 0)
            g_string_append_printf (s, _(" · %u on iPod"), on);
        else
            g_string_append (s, _(" · new"));
    }
    return g_string_free (s, FALSE);
}

static void
render_row (Page *pg, GtkWidget *box, IuNode *n)
{
    App *a = pg->app;
    GtkCheckButton *check = g_object_get_data (G_OBJECT (box), "check");
    GtkImage *icon = g_object_get_data (G_OBJECT (box), "icon");
    GtkLabel *title = g_object_get_data (G_OBJECT (box), "title");
    GtkLabel *subtitle = g_object_get_data (G_OBJECT (box), "subtitle");
    GtkProgressBar *bar = g_object_get_data (G_OBJECT (box), "bar");
    GtkImage *status = g_object_get_data (G_OBJECT (box), "status");
    gboolean job_here = a->job && a->job_page == pg;

    /* Checkbox (suppress toggled while we set it). */
    g_object_set_data (G_OBJECT (check), "node", NULL);
    gint cs = iu_node_check_state (n);
    gtk_check_button_set_inconsistent (check, cs == 2);
    gtk_check_button_set_active (check, cs == 1);
    gtk_widget_set_sensitive (GTK_WIDGET (check), !a->job);
    g_object_set_data (G_OBJECT (check), "node", n);

    const gchar *icon_name = n->kind == IU_NODE_ARTIST ? "avatar-default-symbolic"
                           : n->kind == IU_NODE_ALBUM ? "media-optical-symbolic"
                           : "audio-x-generic-symbolic";
    gtk_image_set_from_icon_name (icon, icon_name);

    gtk_label_set_text (title, n->name);
    set_css_class (GTK_WIDGET (title), "heading", n->kind == IU_NODE_ARTIST);

    gchar *sub = n->kind == IU_NODE_SONG ? song_subtitle (pg, n) : group_subtitle (pg, n);
    gtk_label_set_text (subtitle, sub);
    g_free (sub);
    gboolean failed = n->state == IU_STATE_FAILED;
    gboolean warn = n->kind == IU_NODE_SONG && n->length_differs && !failed && !pg->manage;
    set_css_class (GTK_WIDGET (subtitle), "error", failed);
    set_css_class (GTK_WIDGET (subtitle), "warning", warn);
    set_css_class (GTK_WIDGET (subtitle), "dim-label", !failed && !warn);

    /* Progress bar while a job runs on this page. */
    gboolean show_bar = FALSE;
    gdouble frac = 0.0;
    if (job_here) {
        if (n->kind == IU_NODE_SONG) {
            show_bar = n->selected;
            frac = n->state == IU_STATE_ACTIVE ? n->progress
                 : n->state == IU_STATE_QUEUED || n->state == IU_STATE_IDLE ? 0.0 : 1.0;
        } else {
            show_bar = iu_node_count_songs (n, TRUE, NULL) > 0;
            frac = iu_node_progress (n);
        }
    }
    gtk_widget_set_visible (GTK_WIDGET (bar), show_bar);
    gtk_progress_bar_set_fraction (bar, frac);

    if (n->state == IU_STATE_DONE) {
        gtk_image_set_from_icon_name (status, "object-select-symbolic");
        gtk_widget_add_css_class (GTK_WIDGET (status), "success");
        gtk_widget_set_tooltip_text (GTK_WIDGET (status), NULL);
        gtk_widget_set_visible (GTK_WIDGET (status), TRUE);
    } else if (failed) {
        gtk_image_set_from_icon_name (status, "dialog-error-symbolic");
        gtk_widget_remove_css_class (GTK_WIDGET (status), "success");
        gtk_widget_set_tooltip_text (GTK_WIDGET (status), n->error);
        gtk_widget_set_visible (GTK_WIDGET (status), TRUE);
    } else {
        gtk_widget_set_visible (GTK_WIDGET (status), FALSE);
    }

    gtk_widget_set_opacity (box, job_here && n->kind == IU_NODE_SONG && !n->selected ? 0.45 : 1.0);
}

static GtkWidget *
row_box (Page *pg, GtkListItem *li)
{
    return gtk_tree_expander_get_child (GTK_TREE_EXPANDER (gtk_list_item_get_child (li)));
}

static void
on_node_changed (IuNode *node, gpointer user_data)
{
    GtkListItem *li = user_data;
    Page *pg = g_object_get_data (G_OBJECT (li), "page");
    render_row (pg, row_box (pg, li), node);
}

static void
on_setup (GtkSignalListItemFactory *f, GtkListItem *li, gpointer user_data)
{
    Page *pg = user_data;
    g_object_set_data (G_OBJECT (li), "page", pg);
    gtk_list_item_set_child (li, row_new (pg));
    gtk_list_item_set_activatable (li, FALSE);
    gtk_list_item_set_selectable (li, FALSE);
}

static void
on_bind (GtkSignalListItemFactory *f, GtkListItem *li, gpointer user_data)
{
    Page *pg = user_data;
    GtkTreeListRow *row = gtk_list_item_get_item (li);
    IuNode *node = gtk_tree_list_row_get_item (row);
    GtkWidget *w = gtk_list_item_get_child (li);

    gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (w), row);

    gulong id = g_signal_connect (node, "changed", G_CALLBACK (on_node_changed), li);
    g_object_set_data (G_OBJECT (li), "changed-id", GSIZE_TO_POINTER (id));
    render_row (pg, row_box (pg, li), node);
    g_object_unref (node);
}

static void
on_unbind (GtkSignalListItemFactory *f, GtkListItem *li, gpointer user_data)
{
    Page *pg = user_data;
    GtkTreeListRow *row = gtk_list_item_get_item (li);
    IuNode *node = gtk_tree_list_row_get_item (row);
    gulong id = GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (li), "changed-id"));
    if (id)
        g_signal_handler_disconnect (node, id);
    g_object_set_data (G_OBJECT (li), "changed-id", NULL);
    GtkCheckButton *check = g_object_get_data (G_OBJECT (row_box (pg, li)), "check");
    g_object_set_data (G_OBJECT (check), "node", NULL);
    gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (gtk_list_item_get_child (li)), NULL);
    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/* Tree model                                                          */
/* ------------------------------------------------------------------ */

static gboolean
filter_func (gpointer item, gpointer user_data)
{
    Page *pg = user_data;
    return iu_node_matches (IU_NODE (item), pg->query, pg->query_gen);
}

static GListModel *
create_child_model (gpointer item, gpointer user_data)
{
    Page *pg = user_data;
    IuNode *n = item;
    if (!n->children)
        return NULL;
    return G_LIST_MODEL (gtk_filter_list_model_new (g_object_ref (G_LIST_MODEL (n->children)),
                                                    g_object_ref (GTK_FILTER (pg->filter))));
}

static void
on_root_changed (IuNode *root, gpointer user_data)
{
    update_summary (user_data);
}

static void
page_set_root (Page *pg, IuNode *root)
{
    g_clear_object (&pg->tree);
    g_clear_object (&pg->root);
    pg->root = root;
    g_signal_connect (root, "changed", G_CALLBACK (on_root_changed), pg);

    GtkFilterListModel *fm = gtk_filter_list_model_new (
        g_object_ref (G_LIST_MODEL (root->children)), g_object_ref (GTK_FILTER (pg->filter)));
    pg->tree = gtk_tree_list_model_new (G_LIST_MODEL (fm), FALSE, FALSE,
                                        create_child_model, pg, NULL);
    GtkNoSelection *sel = gtk_no_selection_new (G_LIST_MODEL (g_object_ref (pg->tree)));
    gtk_list_view_set_model (pg->list, GTK_SELECTION_MODEL (sel));
    g_object_unref (sel);

    gboolean any = iu_node_n_children (root) > 0;
    gtk_stack_set_visible_child_name (pg->stack, any ? "list" : "empty");
    if (pg->query && *pg->query)
        set_all_expanded (pg, TRUE);
    update_summary (pg);
}

static void
set_all_expanded (Page *pg, gboolean expanded)
{
    if (!pg->tree)
        return;
    GListModel *m = G_LIST_MODEL (pg->tree);
    for (guint i = 0; i < g_list_model_get_n_items (m); i++) {
        GtkTreeListRow *row = gtk_tree_list_model_get_row (pg->tree, i);
        if (!row)
            continue;
        if (expanded || gtk_tree_list_row_get_depth (row) == 0)
            gtk_tree_list_row_set_expanded (row, expanded);
        g_object_unref (row);
    }
}

static void
on_search_changed (GtkSearchEntry *entry, gpointer user_data)
{
    Page *pg = user_data;
    g_free (pg->query);
    pg->query = fuzzy_prepare (gtk_editable_get_text (GTK_EDITABLE (entry)));
    pg->query_gen++;
    gtk_filter_changed (GTK_FILTER (pg->filter), GTK_FILTER_CHANGE_DIFFERENT);
    /* Show matches in full; fold the list up again when the search is cleared. */
    set_all_expanded (pg, *pg->query != '\0');
}

/* Position of @node in the visible list, or -1. */
static gint
find_position (Page *pg, IuNode *node)
{
    if (!pg->tree)
        return -1;
    GListModel *m = G_LIST_MODEL (pg->tree);
    guint n = g_list_model_get_n_items (m);
    for (guint i = 0; i < n; i++) {
        GtkTreeListRow *row = gtk_tree_list_model_get_row (pg->tree, i);
        if (!row)
            continue;
        IuNode *item = gtk_tree_list_row_get_item (row);
        gboolean hit = item == node;
        g_object_unref (item);
        g_object_unref (row);
        if (hit)
            return (gint) i;
    }
    return -1;
}

/* Re-render every bound row (e.g. when a job starts or ends). */
static void
emit_changed_cb (gpointer item, gpointer data)
{
    iu_node_changed (item, FALSE);
}

static void
rerender_all (Page *pg)
{
    if (pg->root)
        iu_node_foreach (pg->root, emit_changed_cb, NULL);
    update_summary (pg);
}

/* ------------------------------------------------------------------ */
/* Summary / buttons                                                   */
/* ------------------------------------------------------------------ */

static void
count_wrong_cb (gpointer item, gpointer data)
{
    if (((IuNode *) item)->length_differs)
        (*(guint *) data)++;
}

static void
update_summary (Page *pg)
{
    App *a = pg->app;
    gboolean busy = a->job != NULL || a->planning;
    if (pg->dropdown)
        gtk_widget_set_sensitive (GTK_WIDGET (pg->dropdown), !busy);
    if (!pg->root) {
        gtk_label_set_text (pg->summary, "");
        gtk_widget_set_sensitive (GTK_WIDGET (pg->action_btn), FALSE);
        return;
    }

    guint64 sel_bytes = 0;
    guint total = iu_node_count_songs (pg->root, FALSE, NULL);
    guint selected = iu_node_count_songs (pg->root, TRUE, &sel_bytes);
    gchar *size = g_format_size (sel_bytes);
    GString *s = g_string_new (NULL);
    gchar *label;

    if (pg->manage) {
        g_string_append_printf (s, ngettext ("%u song on the iPod", "%u songs on the iPod", total), total);
        if (selected > 0)
            g_string_append_printf (s, _(" · %u selected (%s)"), selected, size);
        label = g_strdup_printf (ngettext ("Remove %u Song", "Remove %u Songs", selected), selected);
        gtk_widget_set_sensitive (GTK_WIDGET (pg->action_btn), selected > 0 && !busy);
    } else {
        guint on_ipod = iu_node_count_on_ipod (pg->root);
        guint wrong = 0;
        iu_node_foreach_song (pg->root, count_wrong_cb, &wrong);
        g_string_append_printf (s, ngettext ("%u new song", "%u new songs", total - on_ipod), total - on_ipod);
        g_string_append_printf (s, _(" · %u already on iPod"), on_ipod);
        if (wrong > 0)
            g_string_append_printf (s, ngettext (" (%u with wrong length)", " (%u with wrong length)", wrong), wrong);
        g_string_append_printf (s, _(" · %u selected (%s)"), selected, size);
        label = g_strdup_printf (ngettext ("Upload %u Song", "Upload %u Songs", selected), selected);
        gboolean ok = a->ipod && a->ipod->has_sysinfo_extended && selected > 0 && !busy;
        gtk_widget_set_sensitive (GTK_WIDGET (pg->action_btn), ok);
    }
    gtk_label_set_text (pg->summary, s->str);
    gtk_button_set_label (pg->action_btn, label);
    g_string_free (s, TRUE);
    g_free (label);
    g_free (size);
}

static void
update_device_labels (App *a)
{
    IpodInfo *ip = a->ipod;
    gboolean busy = a->job != NULL;
    if (!ip) {
        gtk_label_set_text (a->device_label, _("No iPod connected. Connect one in disk mode and make sure it is mounted."));
        adw_banner_set_revealed (a->banner, FALSE);
    } else {
        GString *s = g_string_new (ip->name);
        if (ip->model) {
            g_string_append_printf (s, " · %s", ip->model);
            if (ip->capacity_gb > 0)
                g_string_append_printf (s, " (%g GB)", ip->capacity_gb);
        }
        gchar *freestr = g_format_size (ip->free_bytes);
        gchar *totalstr = g_format_size (ip->total_bytes);
        g_string_append_printf (s, _(" · %s free of %s"), freestr, totalstr);
        g_free (freestr);
        g_free (totalstr);
        gtk_label_set_text (a->device_label, s->str);
        g_string_free (s, TRUE);
        adw_banner_set_revealed (a->banner, !ip->has_sysinfo_extended && !busy);
    }
    gchar *lib = g_strdup_printf (_("Library: %s"), a->library_root);
    gtk_label_set_text (a->library_label, lib);
    g_free (lib);
    gtk_widget_set_sensitive (GTK_WIDGET (a->refresh_btn), !busy && !a->planning && !a->ejecting);
    gtk_widget_set_sensitive (GTK_WIDGET (a->eject_btn), ip && !busy && !a->planning && !a->ejecting);
}

/* ------------------------------------------------------------------ */
/* Planning: read the iPod database and the library in a thread       */
/* ------------------------------------------------------------------ */

typedef struct {
    App     *app;
    guint    seq;
    gchar   *mountpoint;      /* NULL when no iPod */
    gchar   *library_root;
} PlanRequest;

typedef struct {
    GList     *ipod_tracks;   /* IpodTrack */
    GList     *songs;         /* LibrarySong */
    GPtrArray *failures;      /* strings */
    gchar     *ipod_error;
} PlanResult;

typedef struct {
    App   *app;
    guint  seq;
    gint   done, total;
} PlanTick;

static void
plan_request_free (gpointer p)
{
    PlanRequest *r = p;
    g_free (r->mountpoint);
    g_free (r->library_root);
    g_free (r);
}

static void
plan_result_free (gpointer p)
{
    PlanResult *r = p;
    g_list_free_full (r->ipod_tracks, (GDestroyNotify) ipod_track_free);
    g_list_free_full (r->songs, (GDestroyNotify) library_song_free);
    g_ptr_array_unref (r->failures);
    g_free (r->ipod_error);
    g_free (r);
}

static gboolean
plan_tick_main (gpointer data)
{
    PlanTick *t = data;
    App *a = t->app;
    if (t->seq == a->plan_seq) {
        gchar *txt = g_strdup_printf (_("Reading tags… %d of %d"), t->done, t->total);
        gtk_label_set_text (a->upload.loading_label, txt);
        g_free (txt);
    }
    g_free (t);
    return G_SOURCE_REMOVE;
}

static void
plan_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *c)
{
    PlanRequest *req = task_data;
    PlanResult *res = g_new0 (PlanResult, 1);
    res->failures = g_ptr_array_new_with_free_func (g_free);

    if (req->mountpoint) {
        GError *err = NULL;
        res->ipod_tracks = ipod_load_tracks (req->mountpoint, &err);
        if (err) {
            res->ipod_error = g_strdup (err->message);
            g_clear_error (&err);
        }
    }

    GList *files = library_scan (req->library_root);
    gint total = (gint) g_list_length (files), done = 0;
    gint64 last = 0;
    for (GList *l = files; l; l = l->next) {
        GError *err = NULL;
        LibrarySong *s = library_read_song (l->data, &err);
        if (s) {
            res->songs = g_list_prepend (res->songs, s);
        } else {
            g_ptr_array_add (res->failures, g_strdup_printf ("%s\n    %s", (gchar *) l->data,
                                                             err ? err->message : "?"));
            g_clear_error (&err);
        }
        done++;
        gint64 now = g_get_monotonic_time ();
        if (now - last > 100 * 1000 || done == total) {
            last = now;
            PlanTick *t = g_new0 (PlanTick, 1);
            t->app = req->app;
            t->seq = req->seq;
            t->done = done;
            t->total = total;
            g_idle_add (plan_tick_main, t);
        }
    }
    res->songs = g_list_reverse (res->songs);
    g_list_free_full (files, g_free);
    g_task_return_pointer (task, res, plan_result_free);
}

/* Identity of a song across rebuilds: local path or iPod track id. */
static gchar *
song_identity (IuNode *n)
{
    return n->path ? g_strdup (n->path) : g_strdup_printf ("id:%u", n->ipod_id);
}

static void
collect_identity_cb (gpointer item, gpointer data)
{
    IuNode *n = item;
    if (n->selected)
        g_hash_table_add (data, song_identity (n));
}

/* Builds the page's tree from the last scan using its current grouping.
 * With @keep_selection the ticks of the previous tree are carried over,
 * otherwise the defaults apply. */
static void
rebuild_page (Page *pg, gboolean keep_selection)
{
    App *a = pg->app;
    GHashTable *keep = NULL;
    if (keep_selection && pg->root) {
        keep = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        iu_node_foreach_song (pg->root, collect_identity_cb, keep);
    }

    IuNode *root = iu_node_new (IU_NODE_ARTIST, "");
    GHashTable *albums = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    if (pg->manage) {
        for (GList *l = a->ipod_tracks; l; l = l->next) {
            IpodTrack *t = l->data;
            note_album_artist (albums, NULL, t->album, t->artist, t->albumartist);
        }
        for (GList *l = a->ipod_tracks; l; l = l->next) {
            IpodTrack *t = l->data;
            SongMeta m = { t->artist, t->albumartist, t->album, t->title, t->track_nr, t->disc_nr,
                           t->compilation || album_has_many_artists (albums, NULL, t->album) };
            IuNode *s = add_song (pg, root, &m);
            s->ipod_id = t->id;
            s->size = t->size;
            s->length_ms = t->length_ms;
            s->on_ipod = TRUE;
            if (keep) {
                gchar *id = song_identity (s);
                s->selected = g_hash_table_contains (keep, id);
                g_free (id);
            }
        }
    } else {
        for (GList *l = a->songs; l; l = l->next) {
            LibrarySong *ls = l->data;
            gchar *dir = g_path_get_dirname (ls->path);
            note_album_artist (albums, dir, ls->album, ls->artist, ls->albumartist);
            g_free (dir);
        }
        for (GList *l = a->songs; l; l = l->next) {
            LibrarySong *ls = l->data;
            gchar *dir = g_path_get_dirname (ls->path);
            gboolean comp = ls->compilation || album_has_many_artists (albums, dir, ls->album);
            g_free (dir);
            SongMeta m = { ls->artist, ls->albumartist, ls->album, ls->title, ls->track_nr, ls->disc_nr, comp };
            IuNode *s = add_song (pg, root, &m);
            s->path = g_strdup (ls->path);
            s->size = ls->size;
            s->length_ms = ls->length_ms;
            gchar *key = ipod_track_key (ls->artist, ls->album, ls->title, ls->track_nr, (guint32) ls->size);
            IpodTrack *t = a->by_key ? g_hash_table_lookup (a->by_key, key) : NULL;
            g_free (key);
            if (t) {
                s->on_ipod = TRUE;
                s->ipod_id = t->id;
                s->ipod_length_ms = t->length_ms;
                s->length_differs = ls->exact_length && ABS (ls->length_ms - t->length_ms) > 2000;
            }
            if (keep)
                s->selected = g_hash_table_contains (keep, ls->path);
            else
                s->selected = !s->on_ipod || s->length_differs;
        }
    }
    if (keep)
        g_hash_table_destroy (keep);
    g_hash_table_destroy (albums);
    page_set_root (pg, root);
}

/* Takes the scan results and rebuilds both pages. */
static void
build_trees (App *a, PlanResult *res)
{
    g_list_free_full (a->songs, (GDestroyNotify) library_song_free);
    g_list_free_full (a->ipod_tracks, (GDestroyNotify) ipod_track_free);
    a->songs = g_steal_pointer (&res->songs);
    a->ipod_tracks = g_steal_pointer (&res->ipod_tracks);

    g_clear_pointer (&a->by_key, g_hash_table_destroy);
    a->by_key = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (GList *l = a->ipod_tracks; l; l = l->next) {
        IpodTrack *t = l->data;
        g_hash_table_insert (a->by_key,
                             ipod_track_key (t->artist, t->album, t->title, t->track_nr, t->size), t);
    }
    rebuild_page (&a->manage, FALSE);
    rebuild_page (&a->upload, FALSE);
}

static void
plan_done (GObject *src, GAsyncResult *result, gpointer user_data)
{
    App *a = user_data;
    PlanRequest *req = g_task_get_task_data (G_TASK (result));
    PlanResult *res = g_task_propagate_pointer (G_TASK (result), NULL);
    if (req->seq != a->plan_seq) {
        plan_result_free (res);
        return;                          /* superseded by a newer refresh */
    }
    a->planning = FALSE;
    build_trees (a, res);
    update_device_labels (a);
    if (!a->ipod)
        adw_status_page_set_description (a->manage.empty, _("Connect an iPod to see the songs on it."));
    else if (res->ipod_error)
        adw_status_page_set_description (a->manage.empty, res->ipod_error);
    else
        adw_status_page_set_description (a->manage.empty, _("There is no music on this iPod yet."));

    if (res->ipod_error)
        show_error (a, _("Could Not Read iPod"),
                    _("The iPod's music database could not be read."), res->ipod_error);
    if (res->failures->len > 0) {
        gchar *body = g_strdup_printf (ngettext ("%u file in the library could not be read and will not be uploaded.",
                                                 "%u files in the library could not be read and will not be uploaded.",
                                                 res->failures->len), res->failures->len);
        gchar *details = join_lines (res->failures);
        show_error (a, _("Some Files Could Not Be Read"), body, details);
        g_free (details);
        g_free (body);
    }
    plan_result_free (res);
}

static void
refresh (App *a)
{
    if (a->job || a->ejecting)
        return;
    ipod_info_free (a->ipod);
    a->ipod = ipod_find (a->monitor);
    a->planning = TRUE;
    a->plan_seq++;
    update_device_labels (a);

    gtk_label_set_text (a->upload.loading_label, _("Scanning library…"));
    gtk_label_set_text (a->manage.loading_label, _("Reading the iPod…"));
    gtk_stack_set_visible_child_name (a->upload.stack, "loading");
    gtk_stack_set_visible_child_name (a->manage.stack, "loading");
    update_summary (&a->upload);
    update_summary (&a->manage);

    PlanRequest *req = g_new0 (PlanRequest, 1);
    req->app = a;
    req->seq = a->plan_seq;
    req->mountpoint = a->ipod ? g_strdup (a->ipod->mountpoint) : NULL;
    req->library_root = g_strdup (a->library_root);
    GTask *task = g_task_new (NULL, NULL, plan_done, a);
    g_task_set_task_data (task, req, plan_request_free);
    g_task_run_in_thread (task, plan_thread);
    g_object_unref (task);
}

/* ------------------------------------------------------------------ */
/* Jobs                                                                */
/* ------------------------------------------------------------------ */

static void
job_finished_ui (App *a, const IpodProgress *p)
{
    Page *pg = a->job_page;
    gboolean manage = pg->manage;
    gchar *summary;
    if (manage)
        summary = g_strdup_printf (ngettext ("Removed %d song from the iPod.",
                                             "Removed %d songs from the iPod.", p->removed), p->removed);
    else
        summary = g_strdup_printf (_("Uploaded %d, skipped %d, failed %d."),
                                   p->added, p->skipped, p->failed);

    if (p->error) {
        gchar *details = a->job_failures->len > 0 ? join_lines (a->job_failures) : NULL;
        gchar *all = details ? g_strconcat (p->error, "\n\n", details, NULL) : g_strdup (p->error);
        show_error (a, manage ? _("Removing Songs Failed") : _("Upload Failed"), summary, all);
        g_free (all);
        g_free (details);
    } else if (a->job_failures->len > 0) {
        gchar *details = join_lines (a->job_failures);
        show_error (a, manage ? _("Some Songs Could Not Be Removed")
                              : _("Some Songs Could Not Be Uploaded"), summary, details);
        g_free (details);
    } else {
        toast (a, summary);
    }
    g_free (summary);
}

static void
on_job_progress (const IpodProgress *p, gpointer user_data)
{
    App *a = user_data;
    Page *pg = a->job_page;
    if (!a->job || !pg)
        return;

    if (p->index >= 0 && (guint) p->index < a->job_nodes->len) {
        IuNode *n = g_ptr_array_index (a->job_nodes, p->index);
        IuState st = p->item_state == IPOD_ITEM_ACTIVE ? IU_STATE_ACTIVE
                   : p->item_state == IPOD_ITEM_DONE ? IU_STATE_DONE
                   : p->item_state == IPOD_ITEM_SKIPPED ? IU_STATE_SKIPPED
                   : p->item_state == IPOD_ITEM_FAILED ? IU_STATE_FAILED : IU_STATE_QUEUED;
        gboolean starting = st == IU_STATE_ACTIVE && n->state != IU_STATE_ACTIVE;
        iu_node_set_state (n, st, p->fraction, p->item_error);
        if (st == IU_STATE_FAILED) {
            gchar *line = g_strdup_printf ("%s – %s\n    %s",
                                           n->parent && n->parent->parent ? n->parent->parent->name : "",
                                           n->name, p->item_error ? p->item_error : "?");
            g_ptr_array_add (a->job_failures, line);
        }
        if (starting) {
            gchar *txt = g_strdup_printf (pg->manage ? _("Removing %s – %s") : _("Uploading %s – %s"),
                                          n->parent && n->parent->parent ? n->parent->parent->name : "",
                                          n->name);
            gtk_label_set_text (pg->current, txt);
            g_free (txt);
            gint pos = find_position (pg, n);
            if (pos >= 0)
                gtk_list_view_scroll_to (pg->list, pos, GTK_LIST_SCROLL_NONE, NULL);
        }
        if (p->total > 0) {
            gdouble frac = (p->done + (st == IU_STATE_ACTIVE ? p->fraction : 0.0)) / p->total;
            gtk_progress_bar_set_fraction (pg->progress, CLAMP (frac, 0.0, 1.0));
        }
    }
    if (p->total > 0) {
        gchar *txt = pg->manage
            ? g_strdup_printf (_("%d of %d removed"), p->done, p->total)
            : g_strdup_printf (_("%d of %d · %d uploaded, %d skipped, %d failed"),
                               p->done, p->total, p->added, p->skipped, p->failed);
        gtk_progress_bar_set_text (pg->progress, txt);
        g_free (txt);
    }
    if (p->warning)
        toast (a, p->warning);

    if (p->finished) {
        gtk_progress_bar_set_fraction (pg->progress, p->error ? 0.0 : 1.0);
        gtk_label_set_text (pg->current, "");
        job_finished_ui (a, p);

        ipod_job_free (a->job);
        a->job = NULL;
        a->job_page = NULL;
        g_clear_pointer (&a->job_nodes, g_ptr_array_unref);
        g_clear_pointer (&a->job_failures, g_ptr_array_unref);
        gtk_widget_set_visible (GTK_WIDGET (pg->cancel_btn), FALSE);
        gtk_widget_set_visible (GTK_WIDGET (pg->progress), FALSE);
        gtk_widget_set_visible (GTK_WIDGET (pg->current), FALSE);
        gtk_widget_set_visible (GTK_WIDGET (pg->action_btn), TRUE);
        rerender_all (pg);
        update_device_labels (a);

        if (a->close_after_job)
            gtk_window_destroy (a->window);
        else
            refresh (a);
    }
}

static void
collect_selected_cb (gpointer item, gpointer data)
{
    IuNode *n = item;
    if (n->selected)
        g_ptr_array_add (data, g_object_ref (n));
}

static void
job_begin_ui (App *a, Page *pg)
{
    a->job_page = pg;
    a->job_failures = g_ptr_array_new_with_free_func (g_free);
    for (guint i = 0; i < a->job_nodes->len; i++)
        iu_node_set_state (g_ptr_array_index (a->job_nodes, i), IU_STATE_QUEUED, 0.0, NULL);
    gtk_progress_bar_set_fraction (pg->progress, 0.0);
    gtk_progress_bar_set_text (pg->progress, _("Starting…"));
    gtk_widget_set_visible (GTK_WIDGET (pg->progress), TRUE);
    gtk_widget_set_visible (GTK_WIDGET (pg->current), TRUE);
    gtk_widget_set_visible (GTK_WIDGET (pg->cancel_btn), TRUE);
    gtk_widget_set_sensitive (GTK_WIDGET (pg->cancel_btn), TRUE);
    gtk_widget_set_visible (GTK_WIDGET (pg->action_btn), FALSE);
    rerender_all (pg);
    update_summary (&a->upload);
    update_summary (&a->manage);
    update_device_labels (a);
}

static void
on_upload_clicked (GtkButton *btn, gpointer user_data)
{
    App *a = user_data;
    Page *pg = &a->upload;
    if (a->job || a->planning || !a->ipod || !pg->root)
        return;

    a->job_nodes = g_ptr_array_new_with_free_func (g_object_unref);
    iu_node_foreach_song (pg->root, collect_selected_cb, a->job_nodes);
    if (a->job_nodes->len == 0) {
        g_clear_pointer (&a->job_nodes, g_ptr_array_unref);
        return;
    }
    GPtrArray *items = g_ptr_array_new ();
    for (guint i = 0; i < a->job_nodes->len; i++) {
        IuNode *n = g_ptr_array_index (a->job_nodes, i);
        IpodUploadItem *it = g_new0 (IpodUploadItem, 1);
        it->path = g_strdup (n->path);
        it->replace_id = n->on_ipod ? n->ipod_id : 0;
        g_ptr_array_add (items, it);
    }
    a->job = ipod_upload_start (a->ipod->mountpoint, items, on_job_progress, a);
    job_begin_ui (a, pg);
}

static void
on_remove_response (AdwAlertDialog *dlg, const gchar *response, gpointer user_data)
{
    App *a = user_data;
    Page *pg = &a->manage;
    if (g_strcmp0 (response, "remove") != 0 || a->job || a->planning || !a->ipod || !pg->root)
        return;

    a->job_nodes = g_ptr_array_new_with_free_func (g_object_unref);
    iu_node_foreach_song (pg->root, collect_selected_cb, a->job_nodes);
    if (a->job_nodes->len == 0) {
        g_clear_pointer (&a->job_nodes, g_ptr_array_unref);
        return;
    }
    GArray *ids = g_array_new (FALSE, FALSE, sizeof (guint32));
    for (guint i = 0; i < a->job_nodes->len; i++) {
        IuNode *n = g_ptr_array_index (a->job_nodes, i);
        g_array_append_val (ids, n->ipod_id);
    }
    a->job = ipod_remove_start (a->ipod->mountpoint, ids, on_job_progress, a);
    job_begin_ui (a, pg);
}

static void
on_remove_clicked (GtkButton *btn, gpointer user_data)
{
    App *a = user_data;
    if (a->job || a->planning || !a->manage.root)
        return;
    guint64 bytes = 0;
    guint n = iu_node_count_songs (a->manage.root, TRUE, &bytes);
    if (n == 0)
        return;
    gchar *size = g_format_size (bytes);
    gchar *heading = g_strdup_printf (ngettext ("Remove %u Song From the iPod?",
                                                "Remove %u Songs From the iPod?", n), n);
    gchar *body = g_strdup_printf (_("The files (%s) will be deleted from the iPod. Your local library is not affected."), size);
    AdwDialog *dlg = adw_alert_dialog_new (heading, body);
    adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg), "cancel", _("Cancel"),
                                    "remove", _("Remove"), NULL);
    adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg), "remove", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "cancel");
    adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "cancel");
    g_signal_connect (dlg, "response", G_CALLBACK (on_remove_response), a);
    adw_dialog_present (dlg, GTK_WIDGET (a->window));
    g_free (size);
    g_free (heading);
    g_free (body);
}

static void
on_cancel_clicked (GtkButton *btn, gpointer user_data)
{
    App *a = user_data;
    if (a->job) {
        ipod_job_cancel (a->job);
        gtk_widget_set_sensitive (GTK_WIDGET (a->job_page->cancel_btn), FALSE);
        gtk_label_set_text (a->job_page->current, _("Cancelling after the current song…"));
    }
}

/* ------------------------------------------------------------------ */
/* Fetching SysInfoExtended via pkexec                                 */
/* ------------------------------------------------------------------ */

static void
on_sysinfo_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    App *a = user_data;
    GError *err = NULL;
    if (g_subprocess_wait_check_finish (G_SUBPROCESS (src), res, &err)) {
        toast (a, _("Device information read successfully."));
    } else {
        show_error (a, _("Could Not Read Device Information"),
                    _("ipod-read-sysinfo-extended did not succeed."), err->message);
        g_clear_error (&err);
    }
    gtk_widget_set_sensitive (GTK_WIDGET (a->banner), TRUE);
    refresh (a);
}

static void
on_fetch_sysinfo (AdwBanner *banner, gpointer user_data)
{
    App *a = user_data;
    if (!a->ipod)
        return;
    if (!a->ipod->device_node) {
        show_error (a, _("Could Not Read Device Information"),
                    _("The iPod's device node could not be determined."), NULL);
        return;
    }
    gchar *tool = g_find_program_in_path ("ipod-read-sysinfo-extended");
    if (!tool) {
        show_error (a, _("Tool Missing"),
                    _("ipod-read-sysinfo-extended (part of libgpod) is not installed."), NULL);
        return;
    }
    GError *err = NULL;
    GSubprocess *proc = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, &err,
                                          "pkexec", tool, a->ipod->device_node,
                                          a->ipod->mountpoint, NULL);
    g_free (tool);
    if (!proc) {
        show_error (a, _("Could Not Read Device Information"),
                    _("pkexec could not be started."), err->message);
        g_clear_error (&err);
        return;
    }
    gtk_widget_set_sensitive (GTK_WIDGET (a->banner), FALSE);
    g_subprocess_wait_check_async (proc, NULL, on_sysinfo_done, a);
    g_object_unref (proc);
}

/* ------------------------------------------------------------------ */
/* Ejecting                                                            */
/* ------------------------------------------------------------------ */

static void
eject_finished (App *a, gboolean ok, GError *err)
{
    a->ejecting = FALSE;
    if (ok)
        toast (a, _("The iPod was ejected and can be unplugged."));
    else if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
        show_error (a, _("Could Not Eject iPod"), _("The iPod could not be unmounted."),
                    err ? err->message : NULL);
    g_clear_error (&err);
    refresh (a);
}

static void
on_eject_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    GError *err = NULL;
    gboolean ok = g_mount_eject_with_operation_finish (G_MOUNT (src), res, &err);
    eject_finished (user_data, ok, err);
}

static void
on_unmount_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
    GError *err = NULL;
    gboolean ok = g_mount_unmount_with_operation_finish (G_MOUNT (src), res, &err);
    eject_finished (user_data, ok, err);
}

static void
on_eject_clicked (GtkButton *btn, gpointer user_data)
{
    App *a = user_data;
    if (!a->ipod || a->job || a->planning || a->ejecting)
        return;

    GError *err = NULL;
    GFile *f = g_file_new_for_path (a->ipod->mountpoint);
    GMount *mount = g_file_find_enclosing_mount (f, NULL, &err);
    g_object_unref (f);
    if (!mount) {
        show_error (a, _("Could Not Eject iPod"), _("The iPod's mount could not be found."),
                    err ? err->message : NULL);
        g_clear_error (&err);
        return;
    }

    a->ejecting = TRUE;
    update_device_labels (a);
    GMountOperation *op = gtk_mount_operation_new (a->window);
    if (g_mount_can_eject (mount))
        g_mount_eject_with_operation (mount, G_MOUNT_UNMOUNT_NONE, op, NULL, on_eject_done, a);
    else
        g_mount_unmount_with_operation (mount, G_MOUNT_UNMOUNT_NONE, op, NULL, on_unmount_done, a);
    g_object_unref (op);
    g_object_unref (mount);
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */

static void
on_mounts_changed (GVolumeMonitor *m, GMount *mount, gpointer user_data)
{
    App *a = user_data;
    if (a->job) {
        if (a->ipod) {
            GFile *root = g_mount_get_root (mount);
            gchar *path = g_file_get_path (root);
            if (path && g_strcmp0 (path, a->ipod->mountpoint) == 0)
                toast (a, _("Warning: the iPod's mount changed while working on it."));
            g_free (path);
            g_object_unref (root);
        }
        return;
    }
    refresh (a);
}

static gboolean
on_close_request (GtkWindow *win, gpointer user_data)
{
    App *a = user_data;
    if (a->job) {
        a->close_after_job = TRUE;
        ipod_job_cancel (a->job);
        gtk_label_set_text (a->job_page->current, _("Finishing the current song before closing…"));
        return TRUE;              /* keep the window until the DB is written */
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Settings and the grouping controls                                  */
/* ------------------------------------------------------------------ */

static void
settings_load (App *a)
{
    a->settings = g_key_file_new ();
    a->settings_path = g_build_filename (g_get_user_config_dir (), "ipod-uploader.conf", NULL);
    g_key_file_load_from_file (a->settings, a->settings_path, G_KEY_FILE_NONE, NULL);
}

static void
settings_save (App *a)
{
    GError *err = NULL;
    if (!g_key_file_save_to_file (a->settings, a->settings_path, &err)) {
        g_warning ("Could not save settings: %s", err->message);
        g_clear_error (&err);
    }
}

/* "artist,album" <-> groups. */
static void
parse_groups (const gchar *text, GroupKey *groups, gint *n)
{
    *n = 0;
    gchar **parts = g_strsplit (text ? text : "", ",", -1);
    for (gchar **p = parts; *p && *n < N_GROUP_KEYS; p++) {
        for (gint k = 0; k < N_GROUP_KEYS; k++) {
            gboolean dup = FALSE;
            for (gint i = 0; i < *n; i++)
                dup |= groups[i] == (GroupKey) k;
            if (!dup && g_strcmp0 (g_strstrip (*p), group_key_ids[k]) == 0)
                groups[(*n)++] = k;
        }
    }
    g_strfreev (parts);
}

static gchar *
groups_to_string (const GroupKey *groups, gint n)
{
    GString *s = g_string_new (NULL);
    for (gint i = 0; i < n; i++) {
        if (i)
            g_string_append_c (s, ',');
        g_string_append (s, group_key_ids[groups[i]]);
    }
    return g_string_free (s, FALSE);
}

/* The arrangements offered, in dropdown order. Album rows carry their
 * artist, so there is no Album › Artist. */
static const struct { GroupKey g[2]; gint n; } arrangements[] = {
    { { GROUP_ARTIST, GROUP_ALBUM }, 2 },
    { { GROUP_ARTIST, 0 }, 1 },
    { { GROUP_ALBUM, 0 }, 1 },
    { { 0, 0 }, 0 },
};

static gint
groups_to_index (const GroupKey *groups, gint n)
{
    for (guint i = 0; i < G_N_ELEMENTS (arrangements); i++) {
        if (arrangements[i].n != n)
            continue;
        gboolean same = TRUE;
        for (gint j = 0; j < n; j++)
            same &= arrangements[i].g[j] == groups[j];
        if (same)
            return (gint) i;
    }
    return 0;
}

/* Applies a grouping: rebuilds the tree (keeping ticks), syncs the
 * control and remembers the choice. */
static void
page_set_groups (Page *pg, const GroupKey *groups, gint n)
{
    App *a = pg->app;
    if (n == 2 && groups[0] == GROUP_ALBUM)
        n = 1;                           /* Albums already show their artist */
    pg->n_groups = n;
    for (gint i = 0; i < n; i++)
        pg->groups[i] = groups[i];

    pg->updating_control = TRUE;
    if (pg->dropdown)
        gtk_drop_down_set_selected (pg->dropdown, groups_to_index (groups, n));
    pg->updating_control = FALSE;

    gchar *text = groups_to_string (groups, n);
    g_key_file_set_string (a->settings, "grouping", pg->manage ? "manage" : "upload", text);
    g_free (text);
    settings_save (a);

    if (pg->root)
        rebuild_page (pg, TRUE);
}

static void
on_dropdown_changed (GtkDropDown *dd, GParamSpec *pspec, gpointer user_data)
{
    Page *pg = user_data;
    if (pg->updating_control)
        return;
    guint i = gtk_drop_down_get_selected (dd);
    if (i < G_N_ELEMENTS (arrangements))
        page_set_groups (pg, arrangements[i].g, arrangements[i].n);
}

static GtkWidget *
build_dropdown (Page *pg)
{
    GtkStringList *items = gtk_string_list_new (NULL);
    for (guint i = 0; i < G_N_ELEMENTS (arrangements); i++) {
        gchar *label = groups_label (arrangements[i].g, arrangements[i].n);
        gtk_string_list_append (items, label);
        g_free (label);
    }
    GtkWidget *dd = gtk_drop_down_new (G_LIST_MODEL (items), NULL);
    gtk_widget_set_tooltip_text (dd, _("How the list is grouped"));
    gtk_widget_set_valign (dd, GTK_ALIGN_CENTER);
    g_signal_connect (dd, "notify::selected", G_CALLBACK (on_dropdown_changed), pg);
    pg->dropdown = GTK_DROP_DOWN (dd);
    return dd;
}

static GtkWidget *
build_page (App *a, Page *pg, gboolean manage)
{
    pg->app = a;
    pg->manage = manage;
    pg->filter = gtk_custom_filter_new (filter_func, pg, NULL);
    pg->query = g_strdup ("");

    GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *toprow = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top (toprow, 8);
    gtk_widget_set_margin_bottom (toprow, 8);
    gtk_widget_set_margin_start (toprow, 12);
    gtk_widget_set_margin_end (toprow, 12);
    gtk_box_append (GTK_BOX (outer), toprow);

    GtkWidget *search = gtk_search_entry_new ();
    gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (search),
        manage ? _("Search songs on the iPod") : _("Search the library"));
    gtk_widget_set_hexpand (search, TRUE);
    g_signal_connect (search, "search-changed", G_CALLBACK (on_search_changed), pg);
    pg->search = GTK_SEARCH_ENTRY (search);
    gtk_box_append (GTK_BOX (toprow), search);

    gtk_box_append (GTK_BOX (toprow), build_dropdown (pg));

    gchar *saved = g_key_file_get_string (a->settings, "grouping", manage ? "manage" : "upload", NULL);
    GroupKey groups[N_GROUP_KEYS];
    gint n;
    parse_groups (saved ? saved : "artist,album", groups, &n);
    g_free (saved);
    page_set_groups (pg, groups, n);

    GtkWidget *stack = gtk_stack_new ();
    gtk_widget_set_vexpand (stack, TRUE);
    pg->stack = GTK_STACK (stack);
    gtk_box_append (GTK_BOX (outer), stack);

    /* loading */
    GtkWidget *loading = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign (loading, GTK_ALIGN_CENTER);
    GtkWidget *spinner = adw_spinner_new ();
    gtk_widget_set_size_request (spinner, 48, 48);
    gtk_widget_set_halign (spinner, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (loading), spinner);
    GtkWidget *loading_label = gtk_label_new ("");
    gtk_widget_add_css_class (loading_label, "dim-label");
    pg->loading_label = GTK_LABEL (loading_label);
    gtk_box_append (GTK_BOX (loading), loading_label);
    gtk_stack_add_named (GTK_STACK (stack), loading, "loading");

    /* empty */
    GtkWidget *empty = adw_status_page_new ();
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty),
        manage ? "multimedia-player-symbolic" : "folder-music-symbolic");
    adw_status_page_set_title (ADW_STATUS_PAGE (empty),
        manage ? _("No Songs on the iPod") : _("No Songs Found"));
    if (!manage)
        adw_status_page_set_description (ADW_STATUS_PAGE (empty),
            _("Put mp3, m4a, wav or aiff files in your music folder."));
    pg->empty = ADW_STATUS_PAGE (empty);
    gtk_stack_add_named (GTK_STACK (stack), empty, "empty");

    /* list */
    GtkWidget *scroller = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (on_setup), pg);
    g_signal_connect (factory, "bind", G_CALLBACK (on_bind), pg);
    g_signal_connect (factory, "unbind", G_CALLBACK (on_unbind), pg);
    GtkWidget *list = gtk_list_view_new (NULL, factory);
    gtk_widget_add_css_class (list, "navigation-sidebar");
    pg->list = GTK_LIST_VIEW (list);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), list);
    gtk_stack_add_named (GTK_STACK (stack), scroller, "list");
    gtk_stack_set_visible_child_name (GTK_STACK (stack), "loading");

    /* bottom bar */
    GtkWidget *bottom = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class (bottom, "toolbar");
    gtk_widget_set_margin_top (bottom, 8);
    gtk_widget_set_margin_bottom (bottom, 12);
    gtk_widget_set_margin_start (bottom, 12);
    gtk_widget_set_margin_end (bottom, 12);
    gtk_box_append (GTK_BOX (outer), bottom);

    GtkWidget *summary = gtk_label_new ("");
    gtk_label_set_wrap (GTK_LABEL (summary), TRUE);
    gtk_label_set_justify (GTK_LABEL (summary), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class (summary, "dim-label");
    pg->summary = GTK_LABEL (summary);
    gtk_box_append (GTK_BOX (bottom), summary);

    GtkWidget *progress = gtk_progress_bar_new ();
    gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (progress), TRUE);
    gtk_widget_set_visible (progress, FALSE);
    pg->progress = GTK_PROGRESS_BAR (progress);
    gtk_box_append (GTK_BOX (bottom), progress);

    GtkWidget *current = gtk_label_new ("");
    gtk_label_set_ellipsize (GTK_LABEL (current), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class (current, "dim-label");
    gtk_widget_set_visible (current, FALSE);
    pg->current = GTK_LABEL (current);
    gtk_box_append (GTK_BOX (bottom), current);

    GtkWidget *btns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign (btns, GTK_ALIGN_CENTER);
    GtkWidget *action = gtk_button_new_with_label (manage ? _("Remove From iPod") : _("Upload to iPod"));
    gtk_widget_add_css_class (action, manage ? "destructive-action" : "suggested-action");
    gtk_widget_add_css_class (action, "pill");
    g_signal_connect (action, "clicked", G_CALLBACK (manage ? on_remove_clicked : on_upload_clicked), a);
    pg->action_btn = GTK_BUTTON (action);
    gtk_box_append (GTK_BOX (btns), action);
    GtkWidget *cancel = gtk_button_new_with_label (_("Cancel"));
    gtk_widget_add_css_class (cancel, "pill");
    gtk_widget_set_visible (cancel, FALSE);
    g_signal_connect (cancel, "clicked", G_CALLBACK (on_cancel_clicked), a);
    pg->cancel_btn = GTK_BUTTON (cancel);
    gtk_box_append (GTK_BOX (btns), cancel);
    gtk_box_append (GTK_BOX (bottom), btns);

    return outer;
}

static void
activate (GtkApplication *gapp, gpointer user_data)
{
    App *a = user_data;
    if (a->window) {
        gtk_window_present (a->window);
        return;
    }

    GtkWidget *win = adw_application_window_new (gapp);
    a->window = GTK_WINDOW (win);
    gtk_window_set_title (a->window, _("iPod Uploader"));
    gtk_window_set_default_size (a->window, 640, 760);
    g_signal_connect (win, "close-request", G_CALLBACK (on_close_request), a);

    GtkWidget *toolbar = adw_toolbar_view_new ();
    GtkWidget *header = adw_header_bar_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);

    GtkWidget *refresh_btn = gtk_button_new_from_icon_name ("view-refresh-symbolic");
    gtk_widget_set_tooltip_text (refresh_btn, _("Rescan iPod and library"));
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), refresh_btn);
    a->refresh_btn = GTK_BUTTON (refresh_btn);
    g_signal_connect_swapped (refresh_btn, "clicked", G_CALLBACK (refresh), a);

    GtkWidget *eject_btn = gtk_button_new_from_icon_name ("media-eject-symbolic");
    gtk_widget_set_tooltip_text (eject_btn, _("Eject iPod"));
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), eject_btn);
    a->eject_btn = GTK_BUTTON (eject_btn);
    g_signal_connect (eject_btn, "clicked", G_CALLBACK (on_eject_clicked), a);

    GtkWidget *stack = adw_view_stack_new ();
    a->stack = ADW_VIEW_STACK (stack);
    GtkWidget *switcher = adw_view_switcher_new ();
    adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher), a->stack);
    adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), switcher);

    GtkWidget *overlay = adw_toast_overlay_new ();
    a->toasts = ADW_TOAST_OVERLAY (overlay);
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), overlay);
    adw_application_window_set_content (ADW_APPLICATION_WINDOW (win), toolbar);

    GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    adw_toast_overlay_set_child (a->toasts, outer);

    GtkWidget *banner = adw_banner_new (
        _("Device information is missing. The iPod will not show songs without it."));
    adw_banner_set_button_label (ADW_BANNER (banner), _("Read device info"));
    g_signal_connect (banner, "button-clicked", G_CALLBACK (on_fetch_sysinfo), a);
    a->banner = ADW_BANNER (banner);
    gtk_box_append (GTK_BOX (outer), banner);

    GtkWidget *info = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top (info, 10);
    gtk_widget_set_margin_start (info, 12);
    gtk_widget_set_margin_end (info, 12);
    GtkWidget *device = gtk_label_new ("");
    gtk_label_set_ellipsize (GTK_LABEL (device), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class (device, "heading");
    a->device_label = GTK_LABEL (device);
    gtk_box_append (GTK_BOX (info), device);
    GtkWidget *library = gtk_label_new ("");
    gtk_label_set_ellipsize (GTK_LABEL (library), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class (library, "dim-label");
    gtk_widget_add_css_class (library, "caption");
    a->library_label = GTK_LABEL (library);
    gtk_box_append (GTK_BOX (info), library);
    gtk_box_append (GTK_BOX (outer), info);

    gtk_widget_set_vexpand (stack, TRUE);
    gtk_box_append (GTK_BOX (outer), stack);

    AdwViewStackPage *p;
    p = adw_view_stack_add_titled (a->stack, build_page (a, &a->upload, FALSE), "upload", _("Upload"));
    adw_view_stack_page_set_icon_name (p, "document-send-symbolic");
    p = adw_view_stack_add_titled (a->stack, build_page (a, &a->manage, TRUE), "manage", _("Manage"));
    adw_view_stack_page_set_icon_name (p, "multimedia-player-symbolic");

    a->monitor = g_volume_monitor_get ();
    g_signal_connect (a->monitor, "mount-added", G_CALLBACK (on_mounts_changed), a);
    g_signal_connect (a->monitor, "mount-removed", G_CALLBACK (on_mounts_changed), a);

    refresh (a);
    gtk_window_present (a->window);
}

int
main (int argc, char **argv)
{
    App a = { 0 };
    a.library_root = library_default_root ();
    settings_load (&a);

    AdwApplication *app = adw_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    a.gapp = GTK_APPLICATION (app);
    g_signal_connect (app, "activate", G_CALLBACK (activate), &a);
    int status = g_application_run (G_APPLICATION (app), argc, argv);

    if (a.job)
        ipod_job_free (a.job);
    ipod_info_free (a.ipod);
    g_list_free_full (a.songs, (GDestroyNotify) library_song_free);
    g_list_free_full (a.ipod_tracks, (GDestroyNotify) ipod_track_free);
    g_clear_pointer (&a.by_key, g_hash_table_destroy);
    g_key_file_free (a.settings);
    g_free (a.settings_path);
    g_free (a.library_root);
    g_clear_object (&a.monitor);
    g_object_unref (app);
    return status;
}
