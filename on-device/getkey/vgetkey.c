/* vgetkey - read the video stream key out of the running mainapp (READ ONLY).
 *
 * The Vimar K40955's stream key (DeviceKey2) is never written to the monitor's disk; mainapp
 * rebuilds it at boot from the [SECRET] values in /mntfs/wifi/npc_nts_client_config.ini and keeps
 * it only in memory, stored as a parsed field: the name "DeviceKey2" and the 32-character value
 * live near each other but are not written as "name=value" text. This reads the live process
 * memory and prints the value, so Home Assistant's stream_credentials.json can be made without the
 * vendor cloud / quii-helper.
 *
 * It opens /proc/<pid>/mem read-only. It never writes to the process or to the flash.
 *
 *   vgetkey -k [-m] <pid>            find the 32-char key next to a "DeviceKey2" label (the way)
 *   vgetkey [-m] [-a] <pid> [anchor] print the value after a text anchor like "DeviceKey2="
 *   vgetkey -C <needle> <pid>        dump the bytes around <needle> (to see how a value is stored)
 *     -m  masked: print only length and charset, not the value
 *     -a  scan all readable regions, not only writable ones
 *     -l <label>  use another label with -k (default DeviceKey2)
 *
 * Build: arm-linux-gnueabi-gcc -O1 -march=armv6 -marm -mfloat-abi=soft -static -o vgetkey vgetkey.c
 */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>

#define CHUNK (1u << 20)
#define MAXVAL 512
#define MAXUNIQ 64

static int mask = 0, allr = 0, findkey = 0;
static const char *ctx_needle = 0;
static const char *label = "DeviceKey2";
static const int keylen = 32;
static char seen[MAXUNIQ][MAXVAL];
static int nseen = 0;

static int is_an(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

static int take(const char *p, size_t avail, char *out) {
    size_t i = 0;
    while (i < avail && i < MAXVAL - 1) {
        unsigned char c = (unsigned char)p[i];
        if (c == '\r' || c == '\n' || c == '\0') break;
        if (c < 0x20 || c == 0x7f) return 0;
        out[i] = (char)c; i++;
    }
    if (i == 0) return 0;
    out[i] = 0;
    return 1;
}

static void record(const char *anchor, const char *val) {
    int i;
    for (i = 0; i < nseen; i++) if (strcmp(seen[i], val) == 0) return;
    if (nseen < MAXUNIQ) strcpy(seen[nseen++], val);
    if (mask) {
        size_t n = strlen(val); int hex = 1, j;
        for (j = 0; val[j]; j++) if (!isxdigit((unsigned char)val[j])) { hex = 0; break; }
        printf("%s<len=%zu,%s>\n", anchor, n, hex ? "hex" : "mixed");
    } else {
        printf("%s%s\n", anchor, val);
    }
}

/* read a whole region in overlapping windows; call fn(buf,len,region_perms) on each window */
static void each_window(int mem, unsigned long long lo, unsigned long long hi,
                        const char *perms, char *buf, size_t overlap,
                        void (*fn)(char *, size_t, const char *)) {
    unsigned long long cur = lo;
    while (cur < hi) {
        size_t want = (size_t)(hi - cur);
        if (want > CHUNK) want = CHUNK;
        ssize_t got = pread(mem, buf, want, (off_t)cur);
        if (got <= 0) break;
        fn(buf, (size_t)got, perms);
        if (want < CHUNK) break;
        cur += CHUNK - overlap;
    }
}

/* ---- mode state shared with the window callbacks ---- */
static const char *cb_anchor;
static size_t cb_alen;
static const char *cb_needle;
static size_t cb_nlen;
static int cb_hits;

static void cb_scan_anchor(char *buf, size_t n, const char *perms) {
    (void)perms; char val[MAXVAL]; size_t i;
    for (i = 0; i + cb_alen <= n; i++)
        if (buf[i] == cb_anchor[0] && memcmp(buf + i, cb_anchor, cb_alen) == 0)
            if (take(buf + i + cb_alen, n - i - cb_alen, val)) record(cb_anchor, val);
}

static void cb_findkey(char *buf, size_t n, const char *perms) {
    (void)perms; size_t llen = strlen(label), near = 256, i;
    for (i = 0; i + keylen <= n; i++) {
        if (i && is_an((unsigned char)buf[i - 1])) continue;        /* run must start here */
        size_t j = i;
        while (j < n && is_an((unsigned char)buf[j])) j++;
        if (j - i != (size_t)keylen) { if (j > i) i = j - 1; continue; }  /* exactly 32 */
        size_t ws = (i > near) ? i - near : 0, we = i + keylen + near;
        if (we > n) we = n;
        int close = 0; size_t p;
        for (p = ws; p + llen <= we; p++)
            if (buf[p] == label[0] && memcmp(buf + p, label, llen) == 0) { close = 1; break; }
        if (close) { char v[64]; memcpy(v, buf + i, keylen); v[keylen] = 0; record("", v); }
        i = j - 1;
    }
}

static void cb_ctx(char *buf, size_t n, const char *perms) {
    size_t pad = 48, i;
    for (i = 0; i + cb_nlen <= n; i++) {
        if (buf[i] == cb_needle[0] && memcmp(buf + i, cb_needle, cb_nlen) == 0) {
            cb_hits++;
            size_t s = (i > pad) ? i - pad : 0, e = i + cb_nlen + pad; if (e > n) e = n;
            printf("@%s  ", perms);
            size_t k; for (k = s; k < e; k++) { unsigned char c = (unsigned char)buf[k]; putchar((c >= 0x20 && c < 0x7f) ? c : '.'); }
            putchar(0x0a);
        }
    }
}

int main(int argc, char **argv) {
    int a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "-m")) mask = 1;
        else if (!strcmp(argv[a], "-a")) allr = 1;
        else if (!strcmp(argv[a], "-k")) { findkey = 1; allr = 1; }
        else if (!strcmp(argv[a], "-l")) { if (++a >= argc) { fprintf(stderr, "-l needs a label\n"); return 2; } label = argv[a]; }
        else if (!strcmp(argv[a], "-C")) { if (++a >= argc) { fprintf(stderr, "-C needs a string\n"); return 2; } ctx_needle = argv[a]; allr = 1; }
        else { fprintf(stderr, "unknown option %s\n", argv[a]); return 2; }
    }
    if (a >= argc) {
        fprintf(stderr, "usage: vgetkey -k [-m] <pid>            find the stream key (DeviceKey2)\n"
                        "       vgetkey [-m] [-a] <pid> [anchor]  value after a text anchor\n"
                        "       vgetkey -C <needle> <pid>         bytes around <needle>\n");
        return 2;
    }
    const char *pid = argv[a++];
    cb_anchor = (a < argc) ? argv[a] : "DeviceKey2=";
    cb_alen = strlen(cb_anchor);
    cb_needle = ctx_needle ? ctx_needle : "";
    cb_nlen = strlen(cb_needle);

    char path[64];
    snprintf(path, sizeof path, "/proc/%s/maps", pid);
    FILE *maps = fopen(path, "r");
    if (!maps) { fprintf(stderr, "cannot open %s (wrong pid, or not root?)\n", path); return 1; }
    snprintf(path, sizeof path, "/proc/%s/mem", pid);
    int mem = open(path, O_RDONLY);
    if (mem < 0) { fprintf(stderr, "cannot open %s\n", path); fclose(maps); return 1; }

    char *buf = malloc(CHUNK + MAXVAL);
    char line[512], perms[8];
    unsigned long long lo, hi;

    while (fgets(line, sizeof line, maps)) {
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) < 3) continue;
        if (perms[0] != 'r') continue;
        if (!allr && perms[1] != 'w') continue;
        if (ctx_needle)   each_window(mem, lo, hi, perms, buf, cb_nlen + 2 * 48, cb_ctx);
        else if (findkey) each_window(mem, lo, hi, perms, buf, 256 + 64 + keylen, cb_findkey);
        else              each_window(mem, lo, hi, perms, buf, cb_alen > 1 ? cb_alen - 1 : 0, cb_scan_anchor);
    }
    free(buf); close(mem); fclose(maps);

    if (ctx_needle) { if (!cb_hits) { fprintf(stderr, "needle not found\n"); return 1; } return 0; }
    if (findkey)    { if (!nseen) { fprintf(stderr, "no 32-char key found near a \"%s\" label\n", label); return 1; } return 0; }
    if (!nseen) { fprintf(stderr, "anchor \"%s\" not found in pid %s%s\n", cb_anchor, pid, allr ? "" : " (try -a)"); return 1; }
    return 0;
}
