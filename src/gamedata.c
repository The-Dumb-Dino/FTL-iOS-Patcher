// gamedata.c
//
// The iPad mod fix-up for ftl.dat, at run time. The patcher applies it when it packs
// the game (patcher/src/ftldat.cpp), but the copy in Documents can be replaced later
// (a mod manager, a hand edit), and the iPad build needs it:
//  - it looks for its own x.tex texture before x.png, so a mod's x.png is never used, and
//  - it keeps small images in atlases, listed in atlas/*.txt: an image listed there is
//    drawn from the atlas, the iPad original, whatever the mod put in.
// So this hides x.tex where a mod added x.png (renamed in place to x.te_; the index stays
// as it is) and rewrites the atlas lists without the images mods replaced (the new lists
// are appended to the file, and their index entries point there).
//
// The same as ftldat::Fix, with zlib in place of miniz, and no exceptions: a broken file
// is left as it was.
//
// ftl.dat ("PKG\n", big-endian): 16-byte header (magic, header size, entry size, entry
// count, path table size), 20-byte entries (name hash, flags << 24 | path offset, data
// offset, stored size, real size), the NUL-separated path table, then the files,
// zlib-compressed when flag 1 is set.

#include "gamedata.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#define MAX_ENTRIES (4u << 20)  // far more than FTL's ~30,000
#define MAX_TABLE   (256u << 20)
#define MAX_FILE    (64u << 20)  // an atlas list, or a name

static uint32_t Rd32Be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void Wr32Be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

typedef struct
{
    uint32_t i;  // its place in the index
    uint32_t po, flags, off, size, length;
} Entry;

static int CompareNames(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static bool HasName(const char *const *names, size_t n, const char *want)
{
    return n && bsearch(&want, names, n, sizeof *names, CompareNames);
}

static bool IsSpace(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

// The reference drops a line whose first whitespace-separated word is one of the
// replaced images.
static bool LineDropped(const uint8_t *line, size_t n, const char *const *names, size_t nNames)
{
    size_t a = 0;
    while (a < n && IsSpace(line[a])) a++;
    size_t b = a;
    while (b < n && !IsSpace(line[b])) b++;
    if (b == a || b - a >= 4096) return false;
    char low[4096];
    for (size_t i = 0; i < b - a; i++) low[i] = (char)tolower(line[a + i]);
    low[b - a] = 0;
    return HasName(names, nNames, low);
}

int GameDataFix(const char *path)
{
    FILE *f = fopen(path, "r+b");
    if (!f) return GAME_DATA_ERROR;

    int result = GAME_DATA_ERROR;
    uint8_t *index = NULL, *table = NULL, *lower = NULL;
    Entry *entries = NULL;
    const char **names = NULL, **replaced = NULL;
    off_t end = -1, originalEnd = -1;
    uint32_t count = 0, psize = 0;
    uint64_t indexSize = 0;

    uint8_t head[16];
    if (fread(head, 1, sizeof head, f) != sizeof head || memcmp(head, "PKG\n\0\x10\0\x14", 8)) goto done;
    count = Rd32Be(head + 8);
    psize = Rd32Be(head + 12);
    if (count > MAX_ENTRIES || psize > MAX_TABLE) goto done;
    indexSize = (uint64_t)count * 20;
    if (indexSize > SIZE_MAX) goto done;

    index = malloc(indexSize ? (size_t)indexSize : 1);
    table = malloc(psize ? psize : 1);
    lower = malloc((size_t)psize + 1);
    entries = malloc((count ? count : 1) * sizeof *entries);
    names = malloc((count ? count : 1) * sizeof *names);
    replaced = malloc((count ? count : 1) * sizeof *replaced);
    if (!index || !table || !lower || !entries || !names || !replaced) goto done;
    if (fread(index, 1, (size_t)indexSize, f) != (size_t)indexSize) goto done;
    if (fread(table, 1, psize, f) != psize) goto done;

    // Names are looked up in the path table a lot: a lower-case copy, sorted.
    memcpy(lower, table, psize);
    lower[psize] = 0;
    for (uint32_t k = 0; k < psize; k++) lower[k] = (uint8_t)tolower(lower[k]);
    for (uint32_t k = 0; k < count; k++)
    {
        uint8_t *e = index + (size_t)k * 20;
        uint32_t fp = Rd32Be(e + 4), po = fp & 0xFFFFFF;
        if (po >= psize) goto done;
        const char *name = (const char *)table + po;
        if (!memchr(name, 0, psize - po)) goto done;
        entries[k] = (Entry){k, po, fp >> 24, Rd32Be(e + 8), Rd32Be(e + 12), Rd32Be(e + 16)};
        names[k] = (const char *)lower + po;
    }
    qsort(names, count, sizeof *names, CompareNames);

    if (fseeko(f, 0, SEEK_END)) goto done;
    end = originalEnd = ftello(f);
    if (end < 0) goto done;
    for (uint32_t k = 0; k < count; k++)
        if ((uint64_t)entries[k].off + entries[k].size > (uint64_t)end) goto done;

    // 1. x.tex hidden where a mod added x.png
    int hidden = 0;
    for (uint32_t k = 0; k < count; k++)
    {
        Entry *e = &entries[k];
        const char *low = (const char *)lower + e->po;
        size_t n = strlen(low);
        if (n <= 4 || n >= 4096 || strcmp(low + n - 4, ".tex")) continue;
        char png[4096];
        memcpy(png, low, n - 4);
        memcpy(png + n - 4, ".png", 5);
        if (!HasName(names, count, png)) continue;
        table[e->po + n - 1] = '_';  // x.tex -> x.te_
        hidden++;
    }

    // 2. atlas lists without the images mods put in: the iPad's own art is all .tex, so
    //    any img/*.png in ftl.dat came from a mod
    size_t nReplaced = 0;
    for (uint32_t k = 0; k < count; k++)
    {
        const char *n = names[k];
        size_t len = strlen(n);
        if (len > 4 && !strncmp(n, "img/", 4) && !strcmp(n + len - 4, ".png")) replaced[nReplaced++] = n;
    }
    int rewritten = 0;
    for (uint32_t k = 0; k < count; k++)
    {
        Entry *e = &entries[k];
        const char *low = (const char *)lower + e->po;
        size_t n = strlen(low);
        if (n <= 6 || strncmp(low, "atlas/", 6) || strcmp(low + n - 4, ".txt")) continue;
        if (e->size > MAX_FILE || e->length > MAX_FILE) goto done;

        uint8_t *raw = malloc(e->size ? e->size : 1);
        if (!raw) goto done;
        if (fseeko(f, e->off, SEEK_SET) || fread(raw, 1, e->size, f) != e->size)
        {
            free(raw);
            goto done;
        }
        uint8_t *text = raw;
        size_t textLen = e->size;
        if (e->flags & 1)
        {
            text = malloc(e->length ? e->length : 1);
            uLongf out = e->length;
            if (!text || uncompress(text, &out, raw, e->size) != Z_OK || out != e->length)
            {
                free(raw);
                free(text);
                goto done;
            }
            free(raw);
            textLen = e->length;
        }

        uint8_t *data = malloc(textLen ? textLen : 1);
        if (!data)
        {
            free(text);
            goto done;
        }
        size_t outLen = 0, kept = 0, line = 0;
        bool dropped = false;
        for (;;)
        {
            size_t lineEnd = line;
            while (lineEnd < textLen && text[lineEnd] != '\n') lineEnd++;
            if (LineDropped(text + line, lineEnd - line, replaced, nReplaced))
            {
                dropped = true;
            }
            else
            {
                if (kept++) data[outLen++] = '\n';
                memcpy(data + outLen, text + line, lineEnd - line);
                outLen += lineEnd - line;
            }
            if (lineEnd == textLen) break;
            line = lineEnd + 1;
        }
        free(text);
        if (!dropped)
        {
            free(data);
            continue;
        }

        uLongf zlen = compressBound(outLen);
        uint8_t *z = malloc(zlen ? zlen : 1);
        if (!z || compress2(z, &zlen, data, outLen, 6) != Z_OK)
        {
            free(z);
            free(data);
            goto done;
        }
        free(data);
        if ((uint64_t)end + zlen > 0xFFFFFFFFu || fseeko(f, end, SEEK_SET) || fwrite(z, 1, zlen, f) != zlen)
        {
            free(z);
            goto done;
        }
        uint8_t *ie = index + (size_t)e->i * 20;
        Wr32Be(ie + 8, (uint32_t)end);
        Wr32Be(ie + 12, (uint32_t)zlen);
        Wr32Be(ie + 16, (uint32_t)outLen);
        ie[4] |= 1;  // compressed
        end += (off_t)zlen;
        free(z);
        rewritten++;
    }

    if (hidden || rewritten)
    {
        if (fseeko(f, 16, SEEK_SET) || fwrite(index, 1, (size_t)indexSize, f) != (size_t)indexSize ||
            fseeko(f, 16 + (off_t)count * 20, SEEK_SET) || fwrite(table, 1, psize, f) != psize)
            goto done;
        result = GAME_DATA_CHANGED;
    }
    else
    {
        result = GAME_DATA_UNCHANGED;
    }

done:
    // A failed fix-up leaves the appended lists unreachable; cut them off, so the file
    // is as it was.
    if (result == GAME_DATA_ERROR && originalEnd >= 0)
    {
        fflush(f);
        if (ftello(f) > originalEnd) ftruncate(fileno(f), originalEnd);
    }
    fclose(f);
    free(index);
    free(table);
    free(lower);
    free(entries);
    free(names);
    free(replaced);
    return result;
}
