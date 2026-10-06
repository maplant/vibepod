#include "fuzzy.h"
#include <string.h>

gchar *
fuzzy_prepare (const gchar *text)
{
    if (!text)
        return g_strdup ("");
    gchar *valid = g_utf8_make_valid (text, -1);
    gchar *folded = g_utf8_casefold (valid, -1);
    gchar *stripped = g_strstrip (folded);          /* in place */
    gchar *ret = g_strdup (stripped);
    g_free (folded);
    g_free (valid);
    return ret;
}

static gboolean
is_boundary (gunichar prev)
{
    return prev == 0 || !g_unichar_isalnum (prev);
}

/* In-order subsequence match of one word with bonuses for consecutive
 * runs and matches at word starts. To stay useful the match may only be
 * interrupted a limited number of times (one break per six characters,
 * plus one), which allows typos and abbreviations but not letters
 * scattered across the whole string. */
static gint
score_word (const gunichar *hay, glong hn, const gunichar *word, glong wn)
{
    gint max_breaks = 1 + (gint) wn / 6;
    gint best = -1;
    /* Try every possible start for the first character and keep the best;
     * cheap enough for the short strings we deal with. */
    for (glong start = 0; start < hn; start++) {
        if (hay[start] != word[0])
            continue;
        gint score = 0, breaks = 0;
        glong h = start;
        glong last = -2;
        gboolean ok = TRUE;
        for (glong w = 0; w < wn; w++) {
            while (h < hn && hay[h] != word[w])
                h++;
            if (h >= hn) {
                ok = FALSE;
                break;
            }
            score += 4;
            if (h == last + 1) {
                score += 6;                              /* consecutive */
            } else if (last >= 0) {
                score -= MIN ((gint) (h - last - 1), 6); /* gap */
                if (++breaks > max_breaks) {
                    ok = FALSE;
                    break;
                }
            }
            if (is_boundary (h > 0 ? hay[h - 1] : 0))
                score += 8;                              /* word start */
            last = h;
            h++;
        }
        if (ok) {
            if (start == 0)
                score += 4;
            if (score > best)
                best = score;
            /* An exact contiguous run cannot be beaten; stop early. */
            if (best >= wn * 10)
                break;
        }
    }
    return best;
}

gint
fuzzy_score (const gchar *haystack, const gchar *needle)
{
    if (!needle || !*needle)
        return 0;
    if (!haystack)
        return -1;

    glong hn;
    gunichar *hay = g_utf8_to_ucs4_fast (haystack, -1, &hn);
    gchar **words = g_strsplit_set (needle, " \t", -1);
    gint total = 0;
    gboolean ok = TRUE;

    for (gchar **w = words; *w && ok; w++) {
        if (!**w)
            continue;
        glong wn;
        gunichar *word = g_utf8_to_ucs4_fast (*w, -1, &wn);
        gint s = score_word (hay, hn, word, wn);
        if (s < 0)
            ok = FALSE;
        else
            total += s;
        g_free (word);
    }
    g_strfreev (words);
    g_free (hay);
    return ok ? total : -1;
}
