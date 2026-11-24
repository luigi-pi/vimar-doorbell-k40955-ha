/*
 * vimaraudio - RTSP/PCMA two-way audio endpoint for the Vimar K40955.
 *
 * RTSP tracks:
 *   trackID=0: monitor microphone -> HA, PCMA/8000 (server recvonly)
 *   trackID=1: HA microphone -> monitor speaker, PCMA/8000 (server sendonly)
 *
 * The helper is deliberately separate from vimarbridge. It opens the audio
 * devices only while audio.state is 1 and an RTSP client is in PLAY.
 *
 * Hardware rules taken from docs/08-audio-path-ghidra.md (T27-T29):
 *   - Never call AI_SET_ATTR/AO_SET_ATTR: mainapp owns the configured rings.
 *   - Never call 0x40045932 or the driver unmap ioctls: those register/release
 *     mainapp's process-specific mappings.
 *   - AI is 8 kHz PCM16 mono, 320 samples/frame.
 *   - AO is 8 or 16 kHz, stereo, and its slot geometry can vary. The helper
 *     reads AO_GET_ATTR and validates the computed slot size against the
 *     AO_GET_SLOT header before writing to the mapped ring.
 *   - Do not change global AI gain or AO volume. There are no corresponding
 *     GET ioctls in the inspected interface, so a helper-side SET would leave
 *     a persistent setting that cannot be restored reliably.
 *
 * Build with the same target flags as vimarbridge. Real audio I/O and the
 * AO_GET_SLOT geometry still require validation on the actual monitor.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define AI_GET_ATTR  0x80045901
#define AI_ENABLE    0x5904
#define AI_DISABLE   0x5905
#define AI_GET_FRAME 0x80045906
#define AO_GET_ATTR  0x80045911
#define AO_ENABLE    0x5912
#define AO_DISABLE   0x5913
#define AO_GET_SLOT  0x40045914

#define RXBUF                 16384
#define RTP_PCMA_PT           8
#define RTP_INPUT_RATE        8000U
#define RTP_INPUT_SAMPLES     320U       /* AI frame and normal 40 ms PCMA payload */
#define MAX_INPUT_SAMPLES     4096U
#define MAX_RTP_PACKET        2048U
#define MAX_AO_SLOT_BYTES     16384U
#define MAX_AUDIO_FRAMES      128U
#define AUDIO_STATE_DEFAULT   "/tmp/vb/audio.state"
#define DEFAULT_PORT          8554
#define SEND_TIMEOUT_MS       300

static volatile sig_atomic_t quitting;

static void on_signal(int sig)
{
    (void)sig;
    quitting = 1;
}

static long mono_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0)
        return 0;
    return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

#define VIMAR_LOG_SOCKET "/tmp/vb/log.sock"
static int audio_log_fd = -1;

static void audio_log(const char *level, const char *fmt, ...)
{
    char msg[640], packet[704];
    struct sockaddr_un addr;
    va_list ap;
    size_t i, n;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fputs(msg, stderr);
    fflush(stderr);
    if (audio_log_fd < 0) return;
    n = (size_t)snprintf(packet, sizeof packet, "audio\t%s\t", level);
    for (i = 0; msg[i] && n + 1 < sizeof packet; i++) {
        unsigned char c = (unsigned char)msg[i];
        packet[n++] = (c == '\r' || c == '\n' || c == '\t' || c < 32) ? ' ' : (char)c;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", VIMAR_LOG_SOCKET);
    (void)sendto(audio_log_fd, packet, n, MSG_DONTWAIT, (struct sockaddr *)&addr, sizeof addr);
}

static long rtp_last_ms;        /* T32: mono time of the last idle-silence packet, 0 = none pending */

/* [T30 addition] diagnostics: counters printed every 2 s while the devices are open */
static unsigned long st_ai, st_ao_slots, st_bc_bytes, st_ch[4], st_ch_other, st_ssrc_drop;

/* [T34 addition] one talker per call: go2rtc forwards talk-back from every viewer into the one backchannel.
   Two open microphones (phone + desktop card) gave mixed audio at double rate and a growing delay.
   Rule (owner's choice, kept simple): the first source that SPEAKS gets the talk-back for the whole call; all
   others are ignored until the AO session ends (End remote / Hang up / limit). WebRTC cards send silence
   continuously, so arrival order alone would pick a silent card. To switch device: end the call, answer again. */
#define BC_VOICE_PEAK 600           /* decoded PCM peak counted as speech */
/* [T41] the locked source is released when it has sent nothing for BC_GONE_MS while another source sends: a go2rtc
   reconnect (live view re-opened by Answer) gives the same phone a new SSRC, and the old lock dropped all its talk-back */
#define BC_GONE_MS 2000
static uint32_t bc_ssrc;
static int bc_locked;
static long bc_last_ms;             /* last packet from the locked source */
static int st_peak_in;
static int st_peak_door;   /* T41: loudest door (AI) sample per stats period, to tell door audio from silence */

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_be16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void put_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* ITU-T G.711 A-law conversion. */
static int16_t alaw_decode(unsigned char a)
{
    int t, seg;
    a ^= 0x55;
    t = ((int)(a & 0x0f) << 4) + 8;
    seg = (a & 0x70) >> 4;
    if (seg >= 1)
        t += 0x100;
    if (seg > 1)
        t <<= (seg - 1);
    return (int16_t)((a & 0x80) ? t : -t);
}

static unsigned char alaw_encode(int16_t pcm)
{
    static const int seg_end[8] = {
        0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF
    };
    int mask, seg, v = pcm;

    if (v >= 0)
        mask = 0xD5;
    else {
        mask = 0x55;
        v = -v - 1;
    }
    if (v > 32767)
        v = 32767;
    for (seg = 0; seg < 8 && v > seg_end[seg]; seg++)
        ;
    if (seg >= 8)
        return (unsigned char)(0x7F ^ mask);

    unsigned char aval = (unsigned char)(seg << 4);
    if (seg < 2)
        aval |= (unsigned char)((v >> 4) & 0x0F);
    else
        aval |= (unsigned char)((v >> (seg + 3)) & 0x0F);
    return (unsigned char)(aval ^ mask);
}

struct audio_hw {
    int ai;
    int ao;
    unsigned aiattr[16];
    unsigned aoattr[16];
    unsigned char *aimap;
    unsigned char *aomap;
    size_t aimap_len;
    size_t aomap_len;
    size_t ai_frame_len;
    size_t ao_slot_len;
    size_t ao_frames_per_slot;
    size_t ao_input_samples;
    uint32_t ai_last_seq;
    uint32_t ai_kbase;
    int ai_started;
    int ao_started;
    int ao_ready;
    int ao_fault;
};

static struct audio_hw hw = {
    .ai = -1,
    .ao = -1,
    .aimap = MAP_FAILED,
    .aomap = MAP_FAILED
};

static void ao_disable_owned(void)
{
    bc_locked = 0;
    if (hw.ao >= 0 && hw.ao_started) {
        if (ioctl(hw.ao, AO_DISABLE) < 0)
            audio_log("WARNING", "AO_DISABLE: %s\n", strerror(errno));
        else
            hw.ao_started = 0;
    }
}

static void audio_close(void)
{
    /* Disable only engines this process successfully enabled. */
    ao_disable_owned();
    if (hw.ai >= 0 && hw.ai_started) {
        if (ioctl(hw.ai, AI_DISABLE) < 0)
            audio_log("WARNING", "AI_DISABLE: %s\n", strerror(errno));
        else
            hw.ai_started = 0;
    }

    if (hw.aimap != MAP_FAILED)
        (void)munmap(hw.aimap, hw.aimap_len);
    if (hw.aomap != MAP_FAILED)
        (void)munmap(hw.aomap, hw.aomap_len);
    if (hw.ai >= 0)
        (void)close(hw.ai);
    if (hw.ao >= 0)
        (void)close(hw.ao);

    memset(&hw, 0, sizeof hw);
    hw.ai = -1;
    hw.ao = -1;
    hw.aimap = MAP_FAILED;
    hw.aomap = MAP_FAILED;
}

static int valid_frames(unsigned n)
{
    return n >= 2U && n <= MAX_AUDIO_FRAMES;
}

static int audio_open(void)
{
    int r;

    if (hw.ai >= 0 && hw.ai_started)
        return 0;
    audio_close();

    hw.ai = open("/dev/ai_dev", O_RDWR | O_NONBLOCK);
    if (hw.ai < 0) {
        audio_log("ERROR", "open /dev/ai_dev: %s\n", strerror(errno));
        goto fail;
    }
    hw.ao = open("/dev/ao_dev", O_RDWR | O_NONBLOCK);
    if (hw.ao < 0)
        audio_log("WARNING", "open /dev/ao_dev: %s (listen-only mode)\n", strerror(errno));

    memset(hw.aiattr, 0, sizeof hw.aiattr);
    if (ioctl(hw.ai, AI_GET_ATTR, hw.aiattr) < 0) {
        audio_log("ERROR", "AI_GET_ATTR: %s\n", strerror(errno));
        goto fail;
    }

    /* Stock AI configuration: {8000 Hz, 16 bit, mode 4, 320, 30}. */
    if (hw.aiattr[0] != RTP_INPUT_RATE || hw.aiattr[1] != 16U ||
        hw.aiattr[2] != 4U || hw.aiattr[3] != RTP_INPUT_SAMPLES ||
        !valid_frames(hw.aiattr[4])) {
        audio_log("ERROR",
                  "unsupported AI attr: rate=%u bits=%u mode=%u samples=%u frames=%u\n",
                  hw.aiattr[0], hw.aiattr[1], hw.aiattr[2],
                  hw.aiattr[3], hw.aiattr[4]);
        goto fail;
    }

    hw.ai_frame_len = (size_t)hw.aiattr[3] * (hw.aiattr[1] / 8U);
    if (hw.ai_frame_len == 0 ||
        hw.ai_frame_len > SIZE_MAX / hw.aiattr[4])
        goto fail;
    hw.aimap_len = hw.ai_frame_len * hw.aiattr[4];
    hw.aimap = mmap(NULL, hw.aimap_len, PROT_READ | PROT_WRITE,
                    MAP_SHARED, hw.ai, 0);
    if (hw.aimap == MAP_FAILED) {
        audio_log("ERROR", "AI mmap (direct): %s\n", strerror(errno));
        goto fail;
    }

    /* AO is optional for listening. Its live slot length is verified later
       against AO_GET_SLOT before any output samples are written. */
    if (hw.ao >= 0) {
        memset(hw.aoattr, 0, sizeof hw.aoattr);
        if (ioctl(hw.ao, AO_GET_ATTR, hw.aoattr) < 0) {
            audio_log("WARNING", "AO_GET_ATTR: %s (listen-only mode)\n", strerror(errno));
            (void)close(hw.ao);
            hw.ao = -1;
        } else if ((hw.aoattr[0] != 8000U && hw.aoattr[0] != 16000U) ||
                   hw.aoattr[1] != 16U ||
                   (hw.aoattr[2] != 3U && hw.aoattr[2] != 4U) ||
                   hw.aoattr[3] == 0U || !valid_frames(hw.aoattr[4])) {
            audio_log("WARNING",
                      "unsupported AO attr: rate=%u bits=%u mode=%u samples=%u frames=%u (listen-only mode)\n",
                      hw.aoattr[0], hw.aoattr[1], hw.aoattr[2],
                      hw.aoattr[3], hw.aoattr[4]);
            (void)close(hw.ao);
            hw.ao = -1;
        } else {
            /* Same initial map-size calculation used by the device-side
               aicap probe. AO_GET_SLOT's returned length is authoritative. */
            hw.ao_slot_len = (size_t)hw.aoattr[3] * (hw.aoattr[1] / 8U);
            if (hw.ao_slot_len < 4U ||
                hw.ao_slot_len > MAX_AO_SLOT_BYTES ||
                hw.ao_slot_len > SIZE_MAX / hw.aoattr[4]) {
                audio_log("WARNING", "invalid AO map geometry (listen-only mode)\n");
                (void)close(hw.ao);
                hw.ao = -1;
                hw.ao_slot_len = 0;
            } else {
                hw.aomap_len = hw.ao_slot_len * hw.aoattr[4];
                hw.aomap = mmap(NULL, hw.aomap_len, PROT_READ | PROT_WRITE,
                                MAP_SHARED, hw.ao, 0);
                if (hw.aomap == MAP_FAILED) {
                    audio_log("WARNING", "AO mmap (direct): %s (listen-only mode)\n",
                          strerror(errno));
                    (void)close(hw.ao);
                    hw.ao = -1;
                    hw.ao_slot_len = 0;
                } else {
                    hw.ao_ready = 1;
                }
            }
        }
    }

    /* Do not issue DISABLE first. T28/T29 establish that the normal HA-answer
       path arrives with the stock engines already disabled and mainapp's
       enable flags clear. A blind DISABLE could interfere with another owner. */
    r = ioctl(hw.ai, AI_ENABLE);
    if (r < 0) {
        audio_log("WARNING", "AI_ENABLE: %s (refusing to take over capture)\n", strerror(errno));
        goto fail;
    }
    hw.ai_started = 1;

    audio_log("INFO",
              "AI active: %u Hz, %u-bit, %u samples/frame, %u frames; AO %s\n",
              hw.aiattr[0], hw.aiattr[1], hw.aiattr[3], hw.aiattr[4],
              hw.ao_ready ? "available (not enabled until backchannel SETUP)" : "listen-only");
    if (hw.ao_ready) {
        audio_log("DEBUG",
                  "AO attr read: rate=%u bits=%u mode=%u samples=%u frames=%u; provisional slot=%lu bytes\n",
                  hw.aoattr[0], hw.aoattr[1], hw.aoattr[2], hw.aoattr[3],
                  hw.aoattr[4], (unsigned long)hw.ao_slot_len);
    }
    return 0;

fail:
    audio_close();
    return -1;
}

static int ao_map_resize(size_t slot_len)
{
    size_t need;
    unsigned frames = hw.aoattr[4];

    if (slot_len < 4U || slot_len > MAX_AO_SLOT_BYTES ||
        (slot_len % 4U) != 0U || !valid_frames(frames) ||
        slot_len > SIZE_MAX / frames)
        return -1;

    need = slot_len * frames;
    if (need > hw.aomap_len) {
        if (hw.aomap != MAP_FAILED) {
            (void)munmap(hw.aomap, hw.aomap_len);
            hw.aomap = MAP_FAILED;
            hw.aomap_len = 0;
        }
        hw.aomap = mmap(NULL, need, PROT_READ | PROT_WRITE,
                        MAP_SHARED, hw.ao, 0);
        if (hw.aomap == MAP_FAILED) {
            audio_log("WARNING", "AO mmap resize (%zu bytes): %s\n",
                      need, strerror(errno));
            return -1;
        }
        hw.aomap_len = need;
    }

    hw.ao_slot_len = slot_len;
    hw.ao_frames_per_slot = slot_len / (sizeof(int16_t) * 2U);
    if (hw.ao_frames_per_slot == 0U ||
        (hw.ao_frames_per_slot * RTP_INPUT_RATE) % hw.aoattr[0] != 0U)
        return -1;

    hw.ao_input_samples =
        (hw.ao_frames_per_slot * RTP_INPUT_RATE) / hw.aoattr[0];
    if (hw.ao_input_samples == 0U || hw.ao_input_samples > MAX_INPUT_SAMPLES)
        return -1;

    /* Only 8 kHz (1:1) and 16 kHz (2:1) conversion are supported. */
    if (hw.aoattr[0] == 8000U) {
        if (hw.ao_frames_per_slot != hw.ao_input_samples)
            return -1;
    } else if (hw.aoattr[0] == 16000U) {
        if (hw.ao_frames_per_slot != hw.ao_input_samples * 2U)
            return -1;
    } else {
        return -1;
    }
    return 0;
}

static int ao_get_slot(unsigned char hdr[64], int block)
{
    struct { void *out; int block; int zero; } req;
    memset(hdr, 0, 64);
    req.out = hdr;
    req.block = block;
    req.zero = 0;
    return ioctl(hw.ao, AO_GET_SLOT, &req);
}

/* [T30 fix] AO_GET_SLOT header (T24 dumps): +0x00 bits, +0x04 mode, +0x08 user address, +0x0c kernel
   address (both step by the slot size), +0x18 sequence. +0x1c is NOT a length for AO (stale values), so the
   slot length comes from AO_GET_ATTR (samples * bits/8) and the header is checked by bits and mode. */
static int ao_hdr_ok(const unsigned char hdr[64])
{
    uint32_t bits = 0, mode = 0;
    memcpy(&bits, hdr + 0x00, sizeof bits);
    memcpy(&mode, hdr + 0x04, sizeof mode);
    return bits == hw.aoattr[1] && mode == hw.aoattr[2];
}

static int ao_write_slot(const unsigned char hdr[64], const int16_t *input,
                         size_t input_samples)
{
    uint32_t seq = 0;
    size_t offset;
    int16_t *dst;

    memcpy(&seq, hdr + 0x18, sizeof seq);
    if (seq == 0U || !ao_hdr_ok(hdr)) {
        uint32_t bits = 0, mode = 0;
        memcpy(&bits, hdr, sizeof bits);
        memcpy(&mode, hdr + 4, sizeof mode);
        audio_log("WARNING",
                  "AO slot header unexpected: seq=%u bits=%u mode=%u (attr bits=%u mode=%u); disabling talkback\n",
                  seq, bits, mode, hw.aoattr[1], hw.aoattr[2]);
        return -1;
    }

    offset = (size_t)((seq - 1U) % hw.aoattr[4]) * hw.ao_slot_len;
    if (offset > hw.aomap_len || hw.ao_slot_len > hw.aomap_len - offset)
        return -1;

    if (input_samples != hw.ao_input_samples)
        return -1;
    dst = (int16_t *)(void *)(hw.aomap + offset);

    if (hw.aoattr[0] == 8000U) {
        for (size_t i = 0; i < input_samples; i++) {
            dst[2U * i] = input[i];
            dst[2U * i + 1U] = input[i];
        }
    } else {
        /* 8 kHz mono -> 16 kHz stereo. Each input sample produces a sample
           and a linearly interpolated midpoint; hold the final sample for
           the final midpoint in this slot. */
        for (size_t i = 0; i < input_samples; i++) {
            int32_t a = input[i];
            int32_t b = (i + 1U < input_samples) ? input[i + 1U] : input[i];
            int16_t mid = (int16_t)((a + b) / 2);
            dst[4U * i] = input[i];
            dst[4U * i + 1U] = input[i];
            dst[4U * i + 2U] = mid;
            dst[4U * i + 3U] = mid;
        }
    }
    return 0;
}

static void ao_disable_fault(const char *why)
{
    if (!hw.ao_fault)
        audio_log("WARNING", "AO talkback disabled: %s\n", why);
    hw.ao_fault = 1;
    ao_disable_owned();
}

static int ao_enable_and_prime(void)
{
    unsigned char hdr[64];
    static const int16_t silence[MAX_INPUT_SAMPLES] = {0};

    if (!hw.ao_ready || hw.ao < 0 || hw.ao_fault || hw.ao_started)
        return hw.ao_started ? 0 : -1;

    if (ioctl(hw.ao, AO_ENABLE) < 0) {
        ao_disable_fault(strerror(errno));
        return -1;
    }
    hw.ao_started = 1;

    /* Obtain one slot before the first microphone packet and prime it with silence (validates the
       slot header and the mapping before real audio). */
    if (ao_get_slot(hdr, 1) < 0) {
        ao_disable_fault(strerror(errno));
        return -1;
    }
    if (ao_map_resize(hw.ao_slot_len) < 0) {      /* [T30 fix] length from AO_GET_ATTR, see ao_hdr_ok */
        ao_disable_fault("AO geometry not supported (needs 8 or 16 kHz stereo slots)");
        return -1;
    }
    if (ao_write_slot(hdr, silence, hw.ao_input_samples) < 0) {
        ao_disable_fault("cannot prime first AO slot");
        return -1;
    }

    audio_log("INFO",
              "AO talkback active: %u Hz, slot=%lu bytes/%lu output frames, input=%lu PCMA samples\n",
              hw.aoattr[0], (unsigned long)hw.ao_slot_len,
              (unsigned long)hw.ao_frames_per_slot,
              (unsigned long)hw.ao_input_samples);
    return 0;
}

static int state_is_on(const char *path)
{
    char b[8] = {0};
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return 0;
    n = read(fd, b, sizeof b - 1U);
    (void)close(fd);
    return n > 0 && b[0] == '1';
}

static int write_all(int fd, const void *vbuf, size_t len)
{
    const unsigned char *buf = (const unsigned char *)vbuf;
    while (len != 0U) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_rtsp(int fd, const char *cseq, const char *headers,
                     const void *body, size_t bodylen)
{
    char h[2048];
    int n = snprintf(h, sizeof h,
        "RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sContent-Length: %lu\r\n\r\n",
        cseq != NULL ? cseq : "1", headers != NULL ? headers : "",
        (unsigned long)bodylen);
    if (n < 0 || (size_t)n >= sizeof h)
        return -1;
    if (write_all(fd, h, (size_t)n) < 0)
        return -1;
    return bodylen != 0U ? write_all(fd, body, bodylen) : 0;
}

static int send_error(int fd, const char *cseq, const char *status)
{
    char h[512];
    int n = snprintf(h, sizeof h,
        "RTSP/1.0 %s\r\nCSeq: %s\r\nContent-Length: 0\r\n\r\n",
        status, cseq != NULL ? cseq : "1");
    return n > 0 && (size_t)n < sizeof h ? write_all(fd, h, (size_t)n) : -1;
}

static int send_interleaved(int fd, unsigned char channel,
                            const unsigned char *packet, size_t len)
{
    unsigned char hdr[4];
    if (len > 65535U)
        return -1;
    hdr[0] = '$';
    hdr[1] = channel;
    put_be16(hdr + 2, (unsigned)len);
    if (write_all(fd, hdr, sizeof hdr) < 0)
        return -1;
    return write_all(fd, packet, len);
}

static int track_from_url(const char *url)
{
    const char *p = strstr(url, "trackID=");
    if (p == NULL)
        return -1;
    p += 8;
    if (*p == '0' && (p[1] == '\0' || p[1] == '&' || p[1] == '?'))
        return 0;
    if (*p == '1' && (p[1] == '\0' || p[1] == '&' || p[1] == '?'))
        return 1;
    return -1;
}

static int parse_content_length(const char *h)
{
    const char *p = strcasestr(h, "Content-Length:");
    char *end;
    long n;

    if (p == NULL)
        return 0;
    p += 15;
    while (*p == ' ' || *p == '\t')
        p++;
    errno = 0;
    n = strtol(p, &end, 10);
    if (errno != 0 || end == p || n < 0 || n > 8192 ||
        (*end != '\r' && *end != '\n' && *end != '\0'))
        return -1;
    return (int)n;
}

static void parse_cseq(const char *h, char *out, size_t cap)
{
    const char *p = strcasestr(h, "CSeq:");
    size_t n;

    if (p == NULL) {
        (void)snprintf(out, cap, "1");
        return;
    }
    p += 5;
    while (*p == ' ' || *p == '\t')
        p++;
    n = strcspn(p, "\r\n");
    if (n >= cap)
        n = cap - 1U;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Return 1 when a frame was read, 0 when no frame is currently available,
   and -1 for a persistent/structural error. */
static int ai_read_frame(unsigned char pcma[RTP_INPUT_SAMPLES], size_t *payload)
{
    unsigned char hdr[64] = {0};
    struct { void *frame; int block; } req;
    uint32_t kva = 0, seq = 0, len = 0;
    size_t pos;

    req.frame = hdr;
    req.block = 1; /* same blocking mode used by mainapp and the tested aicap probe */
    if (hw.ai < 0 || ioctl(hw.ai, AI_GET_FRAME, &req) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        return -1;
    }

    memcpy(&kva, hdr + 0x0c, sizeof kva);
    memcpy(&seq, hdr + 0x18, sizeof seq);
    memcpy(&len, hdr + 0x1c, sizeof len);
    if (seq == 0U || len == 0U || len > hw.ai_frame_len || (len & 1U) != 0U)
        return -1;
    if (seq == hw.ai_last_seq)
        return 0;
    hw.ai_last_seq = seq;

    pos = (size_t)((seq - 1U) % hw.aiattr[4]) * hw.ai_frame_len;
    if (kva != 0U) {
        if (hw.ai_kbase == 0U)
            hw.ai_kbase = kva - (uint32_t)pos;
        if (kva >= hw.ai_kbase) {
            size_t by_addr = (size_t)(kva - hw.ai_kbase);
            if (by_addr <= hw.aimap_len && len <= hw.aimap_len - by_addr)
                pos = by_addr;
        }
    }
    if (pos > hw.aimap_len || len > hw.aimap_len - pos)
        return -1;

    const unsigned char *src = hw.aimap + pos;
    size_t samples = len / sizeof(int16_t);
    if (samples > RTP_INPUT_SAMPLES)
        samples = RTP_INPUT_SAMPLES;
    for (size_t i = 0; i < samples; i++) {
        uint16_t raw = (uint16_t)src[2U * i] |
                       (uint16_t)((uint16_t)src[2U * i + 1U] << 8);
        int16_t s;
        memcpy(&s, &raw, sizeof s);
        { int v = s; if (v < 0) v = -v; if (v > st_peak_door) st_peak_door = v; }
        pcma[i] = alaw_encode(s);
    }
    *payload = samples;
    return 1;
}

static void rtp_resync(uint32_t *ts);   /* T32: defined with send_idle_silence below */

static int send_door_audio(int fd, unsigned char channel,
                           uint16_t *seq, uint32_t *ts, uint32_t ssrc)
{
    unsigned char payload[RTP_INPUT_SAMPLES];
    unsigned char pkt[12U + RTP_INPUT_SAMPLES];
    size_t n = 0;
    int r = ai_read_frame(payload, &n);

    if (r == 0)
        return 0;
    if (r < 0) {
        audio_log("WARNING", "AI_GET_FRAME/header error\n");
        return -1;
    }
    if (n == 0U)
        return 0;

    rtp_resync(ts);                 /* first real frame after idle silence: real elapsed time (T32) */
    pkt[0] = 0x80;
    pkt[1] = RTP_PCMA_PT;
    put_be16(pkt + 2, *seq);
    put_be32(pkt + 4, *ts);
    put_be32(pkt + 8, ssrc);
    memcpy(pkt + 12, payload, n);
    if (send_interleaved(fd, channel, pkt, 12U + n) < 0)
        return -1;
    (*seq)++;
    *ts += (uint32_t)n;
    st_ai++;
    return 0;
}

/* RTP clock across idle silence (T32): *ts always holds "timestamp of the next packet if contiguous".
   After a silence packet the next packet (silence or the first real door frame) is not contiguous, so its
   timestamp is advanced by the real elapsed time instead: ts_next = ts_last + elapsed_ms * 8. */

static void rtp_resync(uint32_t *ts)
{
    if (rtp_last_ms != 0) {
        long el = mono_ms() - rtp_last_ms;
        if (el > 0 && (uint32_t)el * 8U > RTP_INPUT_SAMPLES)
            *ts += (uint32_t)el * 8U - RTP_INPUT_SAMPLES;   /* the 320 of the last packet are already in *ts */
        rtp_last_ms = 0;
    }
}

/* [T30 addition, T32: 1 packet/s] While a client is in PLAY but the devices are closed (no HA call), send
   40 ms of A-law silence once per second so that go2rtc's RTSP reader does not time out ("read tcp ...:
   i/o timeout"). No device access. */
#define IDLE_SILENCE_MS 1000
static int send_idle_silence(int fd, unsigned char channel,
                             uint16_t *seq, uint32_t *ts, uint32_t ssrc, long *next_ms)
{
    unsigned char pkt[12U + RTP_INPUT_SAMPLES];
    long now = mono_ms();

    if (now < *next_ms)
        return 0;
    *next_ms = now + IDLE_SILENCE_MS;
    rtp_resync(ts);
    pkt[0] = 0x80;
    pkt[1] = RTP_PCMA_PT;
    put_be16(pkt + 2, *seq);
    put_be32(pkt + 4, *ts);
    put_be32(pkt + 8, ssrc);
    memset(pkt + 12, 0xD5, RTP_INPUT_SAMPLES);   /* A-law code for 0 */
    if (send_interleaved(fd, channel, pkt, sizeof pkt) < 0)
        return -1;
    (*seq)++;
    *ts += RTP_INPUT_SAMPLES;
    rtp_last_ms = now;
    return 0;
}

static int ao_write_frame(const int16_t *input, size_t input_samples)
{
    unsigned char hdr[64];

    if (!hw.ao_started || hw.ao_fault || hw.aomap == MAP_FAILED)
        return -1;
    if (ao_get_slot(hdr, 1) < 0)
        return -1;
    if (ao_write_slot(hdr, input, input_samples) < 0)
        return -1;
    st_ao_slots++;
    return 0;
}

static void drain_ao_input(int16_t input[MAX_INPUT_SAMPLES], size_t *used)
{
    while (hw.ao_started && !hw.ao_fault &&
           hw.ao_input_samples != 0U && *used >= hw.ao_input_samples) {
        if (ao_write_frame(input, hw.ao_input_samples) < 0) {
            ao_disable_fault("AO_GET_SLOT or slot write failed");
            *used = 0;
            return;
        }
        *used -= hw.ao_input_samples;
        if (*used != 0U)
            memmove(input, input + hw.ao_input_samples,
                    *used * sizeof input[0]);
    }
}

static void receive_backchannel(unsigned char channel,
                                const unsigned char *pkt, size_t len,
                                unsigned char expected_channel,
                                int16_t input[MAX_INPUT_SAMPLES], size_t *used)
{
    size_t off, end;

    if (channel < 4) st_ch[channel]++; else st_ch_other++;
    if (channel != expected_channel || !hw.ao_started || hw.ao_fault)
        return;
    if (len < 12U || (pkt[0] >> 6) != 2U ||
        (pkt[1] & 0x7fU) != RTP_PCMA_PT)
        return;
    {   /* one talker per call: the first source that speaks (see bc_ssrc) */
        uint32_t ssrc = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) |
                        ((uint32_t)pkt[10] << 8) | (uint32_t)pkt[11];
        if (bc_locked && ssrc != bc_ssrc && mono_ms() - bc_last_ms > BC_GONE_MS) {
            long now = mono_ms();
            audio_log("DEBUG", "[%ld.%03ld] talk-back source %08x sent nothing for %d ms: released\n",
                    now / 1000, now % 1000, (unsigned)bc_ssrc, BC_GONE_MS);
            bc_locked = 0;
        }
        if (!bc_locked) {
            int peak = 0;
            size_t hdr = 12U + (size_t)(pkt[0] & 0x0fU) * 4U;
            for (size_t i = hdr; i < len; i++) {
                int v = alaw_decode(pkt[i]);
                if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
            if (peak < BC_VOICE_PEAK)
                return;                            /* nobody has spoken yet: nothing to play */
            long now = mono_ms();
            bc_ssrc = ssrc;
            bc_locked = 1;
            *used = 0;
            audio_log("DEBUG", "[%ld.%03ld] talk-back source %08x (first speaker, kept while it sends)\n",
                    now / 1000, now % 1000, (unsigned)ssrc);
        }
        if (ssrc != bc_ssrc) {
            st_ssrc_drop++;
            return;
        }
        bc_last_ms = mono_ms();
    }

    off = 12U + (size_t)(pkt[0] & 0x0fU) * 4U;
    if (off > len)
        return;
    if ((pkt[0] & 0x10U) != 0U) {
        if (off + 4U > len)
            return;
        off += 4U + (size_t)be16(pkt + off + 2U) * 4U;
        if (off > len)
            return;
    }
    end = len;
    if ((pkt[0] & 0x20U) != 0U) {
        unsigned padding = pkt[len - 1U];
        if (padding == 0U || padding > end - off)
            return;
        end -= padding;
    }

    for (size_t i = off; i < end; i++) {
        if (*used >= MAX_INPUT_SAMPLES)
            drain_ao_input(input, used);
        if (*used >= MAX_INPUT_SAMPLES) {
            audio_log("WARNING", "dropping oversized backchannel audio packet\n");
            *used = 0;
            return;
        }
        input[(*used)++] = alaw_decode(pkt[i]);
        { int v = input[*used - 1]; if (v < 0) v = -v; if (v > st_peak_in) st_peak_in = v; }
    }
    st_bc_bytes += end - off;
    drain_ao_input(input, used);
}

static int process_rtsp_request(int fd, const char *req, int *playing,
                                int *setup0, int *setup1,
                                unsigned char *out_ch, unsigned char *in_ch,
                                const char *session)
{
    char method[24] = {0}, url[512] = {0}, cseq[64], first[1024], extra[1024];
    const char *line_end = strstr(req, "\r\n");
    size_t first_len;

    if (line_end == NULL)
        return -1;
    first_len = (size_t)(line_end - req);
    if (first_len >= sizeof first)
        return -1;
    memcpy(first, req, first_len);
    first[first_len] = '\0';
    if (sscanf(first, "%23s %511s", method, url) < 2)
        return -1;
    parse_cseq(req, cseq, sizeof cseq);
    {   /* [T30 addition] request trace, with the Transport line for SETUP */
        long t = mono_ms();
        const char *tr = strcasestr(req, "Transport:");
        int trl = tr ? (int)strcspn(tr, "\r\n") : 0;
        audio_log("DEBUG", "[%ld.%03ld] RTSP %s %s %.*s\n", t / 1000, t % 1000, method, url,
                  !strcmp(method, "SETUP") ? trl : 0, tr ? tr : "");
    }

    if (!strcmp(method, "OPTIONS")) {
        (void)snprintf(extra, sizeof extra,
            "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER\r\n");
        return send_rtsp(fd, cseq, extra, NULL, 0);
    }
    if (!strcmp(method, "DESCRIBE")) {
        static const char sdp[] =
            "v=0\r\n"
            "o=- 1 1 IN IP4 0.0.0.0\r\n"
            "s=Vimar K40955 audio\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "t=0 0\r\n"
            "a=control:*\r\n"
            "m=audio 0 RTP/AVP 8\r\n"
            "a=rtpmap:8 PCMA/8000\r\n"
            "a=recvonly\r\n"
            "a=control:trackID=0\r\n"
            "m=audio 0 RTP/AVP 8\r\n"
            "a=rtpmap:8 PCMA/8000\r\n"
            "a=sendonly\r\n"
            "a=control:trackID=1\r\n";
        (void)snprintf(extra, sizeof extra, "Content-Type: application/sdp\r\n");
        return send_rtsp(fd, cseq, extra, sdp, sizeof sdp - 1U);
    }
    if (!strcmp(method, "SETUP")) {
        int track = track_from_url(url);
        const char *transport = strcasestr(req, "Transport:");
        if (track < 0 || transport == NULL ||
            strcasestr(transport, "RTP/AVP/TCP") == NULL)
            return send_error(fd, cseq, "461 Unsupported Transport");
        if (track == 0) {
            *setup0 = 1;
            *out_ch = 0;
        } else {
            *setup1 = 1;
            *in_ch = 2;
        }
        (void)snprintf(extra, sizeof extra,
            "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\nSession: %s\r\n",
            track * 2, track * 2 + 1, session);
        return send_rtsp(fd, cseq, extra, NULL, 0);
    }
    if (!strcmp(method, "PLAY")) {
        if (!*setup0)
            return send_error(fd, cseq, "455 Method Not Valid in This State");
        *playing = 1;
        (void)snprintf(extra, sizeof extra,
            "Session: %s\r\nRange: npt=0.000-\r\n", session);
        return send_rtsp(fd, cseq, extra, NULL, 0);
    }
    if (!strcmp(method, "PAUSE")) {
        *playing = 0;
        (void)snprintf(extra, sizeof extra, "Session: %s\r\n", session);
        return send_rtsp(fd, cseq, extra, NULL, 0);
    }
    if (!strcmp(method, "GET_PARAMETER") || !strcmp(method, "SET_PARAMETER")) {
        (void)snprintf(extra, sizeof extra, "Session: %s\r\n", session);
        return send_rtsp(fd, cseq, extra, NULL, 0);
    }
    if (!strcmp(method, "TEARDOWN")) {
        *playing = 0;
        (void)snprintf(extra, sizeof extra, "Session: %s\r\n", session);
        return send_rtsp(fd, cseq, extra, NULL, 0);
    }
    return send_error(fd, cseq, "405 Method Not Allowed");
}

static int configure_client_socket(int fd)
{
    int yes = 1;
    struct timeval snd_timeout;

    snd_timeout.tv_sec = SEND_TIMEOUT_MS / 1000;
    snd_timeout.tv_usec = (SEND_TIMEOUT_MS % 1000) * 1000;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes) < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   &snd_timeout, sizeof snd_timeout) < 0)
        return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof yes);
#ifdef TCP_KEEPIDLE
    {
        int idle = 15, interval = 5, count = 3;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
#ifdef TCP_KEEPINTVL
        (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
#endif
#ifdef TCP_KEEPCNT
        (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
#endif
    }
#endif
    return 0;
}

static int serve_client(int fd, const char *state_path)
{
    unsigned char buf[RXBUF + 1U];
    size_t have = 0;
    int playing = 0, setup0 = 0, setup1 = 0;
    unsigned char out_ch = 0, in_ch = 2;
    int hw_on = 0, state = 0, lease_expired = 0;
    long active_since = 0, next_state_check = 0, retry_audio_at = 0, next_silence = 0, next_stats = 0;
    char session[24];
    uint16_t rtp_seq = 1;
    uint32_t rtp_ts = 0, ssrc = 0x4B343039;
    int16_t in_pcm[MAX_INPUT_SAMPLES];
    size_t in_used = 0;

    rtp_last_ms = 0;
    if (configure_client_socket(fd) < 0) {
        audio_log("ERROR", "configure RTSP socket: %s\n", strerror(errno));
        return -1;
    }
    (void)snprintf(session, sizeof session, "%08X",
                   (unsigned)(mono_ms() ^ getpid()));

    for (;;) {
        long now;
        int should_open;
        struct pollfd pfd;
        int pr;

        if (quitting)
            break;
        now = mono_ms();
        if (now >= next_state_check) {
            next_state_check = now + 100;
            int new_state = state_is_on(state_path);
            if (new_state != state) {
                state = new_state;
                lease_expired = 0;
                active_since = state ? now : 0;
            }
        }

        /* Mirrors vimarbridge's 120 s limit, with a local 5 s margin. */
        if (state && active_since != 0 && now - active_since > 125000L)
            lease_expired = 1;
        should_open = state && !lease_expired && playing && setup0;
        {   /* [T30 addition] log every change of the open conditions */
            static int last = -1;
            int now_bits = (state << 3) | (lease_expired << 2) | (playing << 1) | setup0 | (setup1 << 4);
            if (now_bits != last) {
                audio_log("DEBUG", "[%ld.%03ld] gate: state=%d playing=%d setup0=%d setup1=%d lease_expired=%d -> open=%d\n",
                        now / 1000, now % 1000, state, playing, setup0, setup1, lease_expired, should_open);
                last = now_bits;
            }
        }

        if (should_open && !hw_on && now >= retry_audio_at) {
            if (audio_open() == 0) {
                hw_on = 1;
                in_used = 0;
            } else {
                retry_audio_at = now + 2000L;
            }
        } else if (!should_open && hw_on) {
            audio_close();
            hw_on = 0;
            in_used = 0;
        }

        if (hw_on && now >= next_stats) {
            if (next_stats != 0)
                audio_log("DEBUG", "[%ld.%03ld] stats 2s: door_frames=%lu door_peak=%d ao_slots=%lu bc_bytes=%lu bc_peak=%d "
                        "rx_pkts ch0=%lu ch1=%lu ch2=%lu ch3=%lu other=%lu other_talkers_dropped=%lu (talk-back expected on ch%u)\n",
                        now / 1000, now % 1000, st_ai, st_peak_door, st_ao_slots, st_bc_bytes, st_peak_in,
                        st_ch[0], st_ch[1], st_ch[2], st_ch[3], st_ch_other, st_ssrc_drop, in_ch);
            st_ai = st_ao_slots = st_bc_bytes = st_ch_other = st_ssrc_drop = 0;
            st_ch[0] = st_ch[1] = st_ch[2] = st_ch[3] = 0; st_peak_in = 0; st_peak_door = 0;
            next_stats = now + 2000;
        } else if (!hw_on) {
            next_stats = 0;
        }

        if (hw_on && setup1 && hw.ao_ready && !hw.ao_started && !hw.ao_fault)
            (void)ao_enable_and_prime();
        if (hw_on && (!setup1 || !playing) && hw.ao_started) {
            ao_disable_owned();
            in_used = 0;
        }

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, 10);
        if (pr < 0 && errno != EINTR)
            break;
        if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            break;
        if (pr > 0 && (pfd.revents & POLLIN) != 0) {
            if (have >= RXBUF)
                break;
            ssize_t n = recv(fd, buf + have, RXBUF - have, 0);
            if (n <= 0)
                break;
            have += (size_t)n;
        }

        /* RTSP text records and '$' interleaved RTP records share the TCP
           connection. Never parse binary RTP as a NUL-terminated string. */
        while (have != 0U) {
            if (buf[0] == '$') {
                size_t plen;
                if (have < 4U)
                    break;
                plen = be16(buf + 2);
                if (have < 4U + plen)
                    break;
                if (hw_on && setup1 && playing && plen <= MAX_RTP_PACKET)
                    receive_backchannel(buf[1], buf + 4, plen, in_ch,
                                        in_pcm, &in_used);
                memmove(buf, buf + 4U + plen, have - 4U - plen);
                have -= 4U + plen;
                continue;
            }

            unsigned char *end = NULL;
            for (size_t i = 0; i + 3U < have; i++) {
                if (buf[i] == '\r' && buf[i + 1U] == '\n' &&
                    buf[i + 2U] == '\r' && buf[i + 3U] == '\n') {
                    end = buf + i + 4U;
                    break;
                }
            }
            if (end == NULL)
                break;

            size_t headlen = (size_t)(end - buf);
            int content = parse_content_length((const char *)buf);
            if (content < 0)
                return -1;
            if (have < headlen + (size_t)content)
                break;

            size_t req_len = headlen + (size_t)content;
            char req[RXBUF + 1U];
            if (req_len >= sizeof req)
                return -1;
            memcpy(req, buf, req_len);
            req[req_len] = '\0';
            memmove(buf, buf + req_len, have - req_len);
            have -= req_len;

            if (process_rtsp_request(fd, req, &playing, &setup0, &setup1,
                                     &out_ch, &in_ch, session) < 0)
                return -1;
            if (!playing)
                in_used = 0;
        }

        if (hw_on && playing && setup0) {
            if (send_door_audio(fd, out_ch, &rtp_seq, &rtp_ts, ssrc) < 0)
                break;
            next_silence = 0;
        } else if (playing && setup0) {
            if (send_idle_silence(fd, out_ch, &rtp_seq, &rtp_ts, ssrc, &next_silence) < 0)
                break;
        }
    }

    audio_close();
    return 0;
}

static int parse_port(const char *s, int *port)
{
    char *end;
    long n;

    errno = 0;
    n = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || n < 1 || n > 65535)
        return -1;
    *port = (int)n;
    return 0;
}

int main(int argc, char **argv)
{
    audio_log_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (audio_log_fd >= 0) (void)fcntl(audio_log_fd, F_SETFL, O_NONBLOCK);
    const char *state_path = AUDIO_STATE_DEFAULT;
    struct in_addr allowed_peer;
    struct sockaddr_in addr;
    int port = DEFAULT_PORT, srv;

    if (argc < 2 || argc > 4) {
        audio_log("ERROR", "usage: %s <HA_IP> [port] [state-file]\n", argv[0]);
        return 2;
    }
    if (inet_pton(AF_INET, argv[1], &allowed_peer) != 1) {
        audio_log("ERROR", "invalid HA IPv4 address: %s\n", argv[1]);
        return 2;
    }
    if (argc >= 3 && parse_port(argv[2], &port) < 0) {
        audio_log("ERROR", "invalid port: %s\n", argv[2]);
        return 2;
    }
    if (argc >= 4)
        state_path = argv[3];

    (void)signal(SIGTERM, on_signal);
    (void)signal(SIGINT, on_signal);
    (void)signal(SIGPIPE, SIG_IGN);

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        audio_log("ERROR", "socket: %s\n", strerror(errno));
        return 1;
    }
    int yes = 1;
    (void)setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(srv, 4) < 0) {
        audio_log("ERROR", "bind/listen: %s\n", strerror(errno));
        (void)close(srv);
        return 1;
    }

    audio_log("INFO", "vimaraudio listening on 0.0.0.0:%d; permitted peer %s; state %s\n",
            port, argv[1], state_path);

    while (!quitting) {
        struct pollfd pfd;
        int r;
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        int c;
        char ip[INET_ADDRSTRLEN] = {0};

        pfd.fd = srv;
        pfd.events = POLLIN;
        pfd.revents = 0;
        r = poll(&pfd, 1, 500);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            audio_log("WARNING", "poll(listener): %s\n", strerror(errno));
            break;
        }
        if (r == 0 || (pfd.revents & POLLIN) == 0)
            continue;

        c = accept(srv, (struct sockaddr *)&peer, &peer_len);
        if (c < 0) {
            if (errno == EINTR)
                continue;
            audio_log("WARNING", "accept: %s\n", strerror(errno));
            continue;
        }
        if (peer.sin_addr.s_addr != allowed_peer.s_addr) {
            (void)inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
            audio_log("WARNING", "reject RTSP peer %s\n", ip);
            (void)close(c);
            continue;
        }
        (void)inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        { long t = mono_ms(); audio_log("INFO", "[%ld.%03ld] RTSP client %s connected\n", t / 1000, t % 1000, ip); }
        (void)serve_client(c, state_path);
        (void)close(c);
        { long t = mono_ms(); audio_log("INFO", "[%ld.%03ld] RTSP client disconnected\n", t / 1000, t % 1000); }
    }

    audio_close();
    (void)close(srv);
    return 0;
}
