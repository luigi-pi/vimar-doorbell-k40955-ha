/* vgetkey - read a key=value pair out of a running process's memory (READ ONLY).
 *
 * Built for the Vimar K40955 monitor (rooted): the video "stream key" (DeviceKey2)
 * is never written to disk - mainapp keeps it only in its in-memory config, rebuilt
 * at boot from the [SECRET] values in /mntfs/wifi/npc_nts_client_config.ini. This
 * reads the live process memory and prints the current value, so Home Assistant's
 * stream_credentials.json can be refreshed without the vendor cloud / quii-helper.
 *
 * It opens /proc/<pid>/mem read-only and scans the writable (heap/data) regions for
 * an anchor such as "DeviceKey2=", printing the value up to the end of the line. It
 * never writes to the process or to the flash. Each distinct value is printed once.
 *
 *   vgetkey [-m] [-a] <pid> [anchor]
 *     -m   masked: print only the anchor, value length and charset (safe to share)
 *     -a   scan all readable regions, not only writable ones (fallback)
 *     anchor defaults to DeviceKey2=
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

static int mask = 0, allr = 0;
static char seen[MAXUNIQ][MAXVAL];
static int nseen = 0;

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
    for (i = 0; i < nseen; i++) if (strcmp(seen[i], val) == 0) return;   /* already printed */
    if (nseen < MAXUNIQ) strcpy(seen[nseen++], val);
    if (mask) {
        size_t n = strlen(val); int hex = 1, j;
        for (j = 0; val[j]; j++) if (!isxdigit((unsigned char)val[j])) { hex = 0; break; }
        printf("%s<len=%zu,%s>\n", anchor, n, hex ? "hex" : "mixed");
    } else {
        printf("%s%s\n", anchor, val);
    }
}

int main(int argc, char **argv) {
    int a = 1;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-m")) mask = 1;
        else if (!strcmp(argv[a], "-a")) allr = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[a]); return 2; }
    }
    if (a >= argc) { fprintf(stderr, "usage: vgetkey [-m] [-a] <pid> [anchor]\n"); return 2; }
    const char *pid = argv[a++];
    const char *anchor = (a < argc) ? argv[a] : "DeviceKey2=";
    size_t alen = strlen(anchor);

    char path[64];
    snprintf(path, sizeof path, "/proc/%s/maps", pid);
    FILE *maps = fopen(path, "r");
    if (!maps) { fprintf(stderr, "cannot open %s (wrong pid, or not root?)\n", path); return 1; }
    snprintf(path, sizeof path, "/proc/%s/mem", pid);
    int mem = open(path, O_RDONLY);
    if (mem < 0) { fprintf(stderr, "cannot open %s\n", path); fclose(maps); return 1; }

    char *buf = malloc(CHUNK + MAXVAL);
    char line[512], val[MAXVAL], perms[8];
    unsigned long long lo, hi;

    while (fgets(line, sizeof line, maps)) {
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) < 3) continue;
        if (perms[0] != 'r') continue;
        if (!allr && perms[1] != 'w') continue;   /* default: writable regions only */
        unsigned long long off = lo;
        size_t carry = 0;
        while (off < hi) {
            size_t want = (size_t)(hi - off);
            if (want > CHUNK) want = CHUNK;
            ssize_t got = pread(mem, buf + carry, want, (off_t)off);
            if (got <= 0) break;
            size_t total = carry + (size_t)got, i;
            for (i = 0; i + alen <= total; i++)
                if (buf[i] == anchor[0] && memcmp(buf + i, anchor, alen) == 0)
                    if (take(buf + i + alen, total - i - alen, val)) record(anchor, val);
            carry = alen > 1 ? alen - 1 : 0;
            if (carry > total) carry = total;
            memmove(buf, buf + total - carry, carry);
            off += (size_t)got;
        }
    }
    free(buf); close(mem); fclose(maps);
    if (!nseen) { fprintf(stderr, "anchor \"%s\" not found in pid %s%s\n",
                          anchor, pid, allr ? "" : " (try -a to scan all regions)"); return 1; }
    return 0;
}
