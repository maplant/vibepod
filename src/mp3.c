#include "mp3.h"
#include <string.h>

static const int br_tab[2][3][16] = {
    { /* MPEG 1 */
        { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 },
        { 0, 32, 48, 56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 384, 0 },
        { 0, 32, 40, 48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 0 } },
    { /* MPEG 2 / 2.5 */
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 },
        { 0,  8, 16, 24, 32, 40, 48,  56,  64,  80,  96, 112, 128, 144, 160, 0 },
        { 0,  8, 16, 24, 32, 40, 48,  56,  64,  80,  96, 112, 128, 144, 160, 0 } }
};
static const int sr_tab[4][3] = {
    { 11025, 12000, 8000 }, { 0, 0, 0 }, { 22050, 24000, 16000 }, { 44100, 48000, 32000 }
};

typedef struct {
    gsize len;          /* frame length in bytes */
    int   spf;          /* samples per frame */
    int   sr;           /* sample rate */
    int   br;           /* kbps */
    gboolean mono, mpeg1;
} Frame;

static gboolean
parse_header (const guchar *p, Frame *f)
{
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0)
        return FALSE;
    int ver = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3;
    int bri = (p[2] >> 4) & 15, sri = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    int chan = (p[3] >> 6) & 3;
    if (ver == 1 || layer == 0 || bri == 0 || bri == 15 || sri == 3)
        return FALSE;
    gboolean mpeg1 = (ver == 3);
    int lidx = 3 - layer;                     /* 0 = layer I, 2 = layer III */
    int br = br_tab[mpeg1 ? 0 : 1][lidx][bri] * 1000;
    int sr = sr_tab[ver][sri];
    int len, spf;
    if (lidx == 0) {
        len = (12 * br / sr + pad) * 4;
        spf = 384;
    } else if (lidx == 1) {
        len = 144 * br / sr + pad;
        spf = 1152;
    } else {
        spf = mpeg1 ? 1152 : 576;
        len = spf / 8 * br / sr + pad;
    }
    if (len < 24)
        return FALSE;
    f->len = len; f->spf = spf; f->sr = sr; f->br = br / 1000;
    f->mono = (chan == 3); f->mpeg1 = mpeg1;
    return TRUE;
}

/* A header is only trusted if the frame after it also parses. */
static gboolean
valid_at (const guchar *d, gsize pos, gsize end, Frame *f)
{
    Frame next;
    if (pos + 4 > end || !parse_header (d + pos, f))
        return FALSE;
    return pos + f->len + 4 > end || parse_header (d + pos + f->len, &next);
}

static guint32
be32 (const guchar *p)
{
    return ((guint32) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

gboolean
mp3_scan (const gchar *path, gint *length_ms, gint *bitrate_kbps, gboolean *has_vbr_header)
{
    GMappedFile *mf = g_mapped_file_new (path, FALSE, NULL);
    if (!mf)
        return FALSE;
    const guchar *d = (const guchar *) g_mapped_file_get_contents (mf);
    gsize n = g_mapped_file_get_length (mf);
    gsize pos = 0, end = n;

    if (n > 10 && memcmp (d, "ID3", 3) == 0) {
        gsize sz = ((d[6] & 0x7f) << 21) | ((d[7] & 0x7f) << 14) | ((d[8] & 0x7f) << 7) | (d[9] & 0x7f);
        pos = 10 + sz + ((d[5] & 0x10) ? 10 : 0);
    }
    if (n >= 128 && memcmp (d + n - 128, "TAG", 3) == 0)
        end = n - 128;

    Frame f;
    gboolean found = FALSE;
    for (gsize i = pos; i + 4 <= end && i < pos + (1 << 20); i++) {
        if (valid_at (d, i, end, &f)) {
            pos = i;
            found = TRUE;
            break;
        }
    }
    if (!found) {
        g_mapped_file_unref (mf);
        return FALSE;
    }

    *has_vbr_header = FALSE;
    gsize side = f.mpeg1 ? (f.mono ? 17 : 32) : (f.mono ? 9 : 17);
    gsize x = pos + 4 + side;
    guint32 frames = 0;
    if (x + 16 <= end && (memcmp (d + x, "Xing", 4) == 0 || memcmp (d + x, "Info", 4) == 0)) {
        if (be32 (d + x + 4) & 1) {
            frames = be32 (d + x + 8);
            *has_vbr_header = TRUE;
        }
    } else if (pos + 36 + 26 <= end && memcmp (d + pos + 36, "VBRI", 4) == 0) {
        frames = be32 (d + pos + 36 + 14);
        *has_vbr_header = TRUE;
    }
    if (*has_vbr_header && frames > 0) {
        *length_ms = (gint) ((double) frames * f.spf * 1000.0 / f.sr);
        *bitrate_kbps = *length_ms > 0 ? (gint) ((end - pos) * 8.0 / *length_ms) : 0;
        g_mapped_file_unref (mf);
        return TRUE;
    }

    /* No usable header: walk every frame. */
    double samples = 0;
    gsize bytes = 0, i = pos;
    int sr = f.sr, resyncs = 0;
    while (i + 4 <= end) {
        if (parse_header (d + i, &f)) {
            samples += f.spf;
            bytes += f.len;
            sr = f.sr;
            i += f.len;
            continue;
        }
        gsize j = i + 1;
        gboolean ok = FALSE;
        for (; j + 4 <= end && j < i + 65536; j++) {
            if (valid_at (d, j, end, &f)) {
                ok = TRUE;
                break;
            }
        }
        if (!ok || ++resyncs > 50)
            break;
        i = j;
    }
    *length_ms = (gint) (samples * 1000.0 / sr);
    *bitrate_kbps = *length_ms > 0 ? (gint) (bytes * 8.0 / *length_ms) : 0;
    g_mapped_file_unref (mf);
    return TRUE;
}
