/* Small fzf-style fuzzy matcher. */
#pragma once
#include <glib.h>

/* Casefolds and strips a string for matching. Free with g_free. */
gchar *fuzzy_prepare (const gchar *text);

/* Scores @needle (prepared with fuzzy_prepare, may contain several
 * space-separated words that can appear in any order) against @haystack
 * (also prepared). Returns a score >= 0 on a match, -1 otherwise. */
gint fuzzy_score (const gchar *haystack, const gchar *needle);
