/*
 * vimarbridge — Vimar K40955 <-> Home Assistant (MQTT over TLS), on the monitor.
 *
 * Process 1 (relay, parent): transparent pty proxy between mainapp and the ACB
 *   bus-MCU UART. Forwards every byte unchanged; reports ring frames to the
 *   MQTT process; injects command frames ONLY between mainapp's frames.
 * Process 2 (mqtt, child): one persistent MQTT 3.1.1 connection over TLS
 *   (BearSSL, server key pinned from a cert file -> no CA, no clock needed).
 *   Publishes HA discovery (event + buttons), ring events, availability (LWT).
 *   If it dies, the relay keeps forwarding and restarts it.
 *
 * Usage: vimarbridge <real_tty> <link_path> <config_file> [-v]
 * Config (key=value): host port user pass cert [base=vimar/k40955] [node=vimar_k40955]
 *
 * ACB frame: FE LEN CMD payload[LEN-4] CHK, CHK = sum(previous bytes) % 0xFE
 * (mainapp uart_rx_frame_parser @0x000ef19c; verified on device 2026-11-04).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "bearssl.h"

static int verbose;
static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* log with a CLOCK_MONOTONIC timestamp at each line start (same clock as aicap) */
static void logf_(const char *fmt, ...)
{
    static int midline;
    char tmp[1024];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n <= 0) return;
    if (!midline) { long t = now_ms(); fprintf(stderr, "[%ld.%03ld] ", t / 1000, t % 1000); }
    fputs(tmp, stderr); fflush(stderr);
    midline = tmp[strlen(tmp) - 1] != '\n';
}
#define LOG(...) do { if (verbose) logf_(__VA_ARGS__); } while (0)

/* ===================================================================== */
/*  ACB frames                                                            */
/* ===================================================================== */
static int build_frame(unsigned char *out, unsigned char cmd, const unsigned char *pl, int n)
{
    unsigned sum = 0xFE + (n + 4) + cmd;
    out[0] = 0xFE; out[1] = n + 4; out[2] = cmd;
    for (int i = 0; i < n; i++) { out[3 + i] = pl[i]; sum += pl[i]; }
    out[3 + n] = sum % 0xFE;
    return n + 4;
}

struct parser { unsigned char b[128]; unsigned pos, len, sum; };

/* returns frame length when a valid frame completed, else 0 */
static int parse_byte(struct parser *p, unsigned char c)
{
    if (p->pos < 2) {
        if (c == 0xFE) { p->b[0] = c; p->pos = 1; p->sum = c; return 0; }
        if (p->pos == 1) {
            if (c < 4 || c > 99) { p->pos = 0; return 0; }
            p->b[1] = c; p->len = c; p->sum += c; p->pos = 2;
        }
        return 0; /* bytes outside frames (e.g. DD acks) ignored */
    }
    p->b[p->pos++] = c;
    if (p->pos < p->len) { p->sum += c; return 0; }
    int ok = (p->sum % 0xFE) == c;
    unsigned l = p->len;
    p->pos = 0;
    return ok ? (int)l : 0;
}

/* ===================================================================== */
/*  Relay (parent)                                                        */
/* ===================================================================== */
static void set_raw(int fd, int speed)
{
    struct termios t;
    if (tcgetattr(fd, &t) != 0) return;
    cfmakeraw(&t);
    if (speed) {
        cfsetispeed(&t, B57600); cfsetospeed(&t, B57600);
        t.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
        t.c_cflag |= CS8 | CLOCAL | CREAD;
    }
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);
}

static void write_all(int fd, const unsigned char *b, ssize_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, b, n);
        if (w > 0) { b += w; n -= w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd p = { fd, POLLOUT, 0 };
            poll(&p, 1, 50);
            continue;
        }
        return;
    }
}

static int mqtt_main(int pairfd, const char *cfgpath); /* child */

/* call state, derived from the frames the relay sees (published by the mqtt child) */
enum { ST_IDLE, ST_RINGING, ST_ANS_MONITOR, ST_ANS_APP, ST_ANS_REMOTE, ST_VIEWING };
static const char *st_name[] = { "idle", "ringing", "answered_monitor", "answered_app", "answered_HA", "viewing" };
static int cstate = ST_IDLE;
static void send_state(int pairfd)
{
    char m[8]; int l = snprintf(m, sizeof m, "S%d\n", cstate);
    if (pairfd >= 0) write(pairfd, m, l);
}
static void set_state(int s, int pairfd, const char *why)
{
    if (s == cstate) return;
    cstate = s;
    LOG("state: %s (%s)\n", st_name[s], why);
    send_state(pairfd);
}

/* call-audio gate for the separate media helper (vimaraudio): "1" while 43 01 is in effect, else "0".
   Written atomically (tmp + rename). Any way a call ends writes "0". */
#define AUDIO_STATE "/tmp/vb/audio.state"
/* longest stock ringtone: melody a04 20 s x 2 = ~40 s (T48). mainapp's ringtone cannot be stopped from the bus,
   and in answered_app nothing in the Lua stops it, so 43 01 before its end puts it on the door bus */
#define RINGTONE_MS 42000
static int audio_on;                           /* last value written: 43 01 in effect for a HA call */
static void audio_state(int on)
{
    audio_on = on;
    int fd = open(AUDIO_STATE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { LOG("relay: cannot write %s: %s\n", AUDIO_STATE, strerror(errno)); return; }
    write(fd, on ? "1\n" : "0\n", 2);
    close(fd);
    rename(AUDIO_STATE ".tmp", AUDIO_STATE);
    LOG("relay: audio.state=%d\n", on);
}

static pid_t start_child(int *pairfd, const char *cfg, int keep1, int keep2, int keep3)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]); close(keep1); close(keep2); close(keep3);
        signal(SIGCHLD, SIG_DFL);
        _exit(mqtt_main(sv[1], cfg));
    }
    close(sv[1]);
    if (pid < 0) { close(sv[0]); return -1; }
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    *pairfd = sv[0];
    return pid;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s real_tty link_path config_file [-v]\n", argv[0]);
        return 2;
    }
    verbose = (argc > 4 && strcmp(argv[4], "-v") == 0);
    signal(SIGPIPE, SIG_IGN);

    int real = open(argv[1], O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (real < 0) { perror("open real tty"); return 1; }
    set_raw(real, 1);
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (m < 0 || grantpt(m) || unlockpt(m)) { perror("pty"); return 1; }
    const char *sname = ptsname(m);
    int s = open(sname, O_RDWR | O_NOCTTY); /* held open: no EIO, raw before mainapp */
    if (s < 0) { perror("open slave"); return 1; }
    set_raw(s, 1);
    unlink(argv[2]);
    if (symlink(sname, argv[2]) != 0) { perror("symlink"); return 1; }
    LOG("relay: %s <-> %s (%s)\n", argv[1], argv[2], sname);

    int pairfd = -1;
    pid_t child = start_child(&pairfd, argv[3], real, m, s);
    send_state(pairfd);
    audio_state(0);                            /* never inherit a stale "on" */
    long restart_at = 0, last_inject = 0;

    struct parser acb = {0}, gk = {0};
    unsigned char pend[64]; int pendlen = 0;   /* one queued command (1-3 frames) */
    unsigned char gkb[128]; int gkn = 0;       /* mainapp frame being assembled */
    long remote_until = 0;
    unsigned char fake[16]; int fakelen = 0;   /* frame to mainapp as if from the ACB */
    long await17 = 0, audio_at = 0;            /* auto 43 01 once mainapp confirmed (its 17) */
    int talk_faked = 0;
    long ring_at = 0;                          /* last ACB ring frame while ringing (safety timeout, T39) */
    long ring_start = 0;                       /* start of the current ring (takeover lock, T48) */
    int ring_viewer = 0;                       /* mainapp sent 40 during this ring (LAN viewer or cloud app) */
    int live_open = 0;                         /* mainapp has a LAN/app live session: its 40 seen, its 41 not yet (T41) */
    long rearm_at = 0;                         /* send 43 01 again: mainapp's 40 cut the door->monitor audio (T41) */
    long app_at = 0;                           /* answered_app since (safety timeout, T41) */
    char pcmd = 0;                             /* HA command waiting for the 1 s injection spacing */                        /* fake 12 02 sent: mainapp's DEVtalkState is 1 (T29) */
    unsigned char held[1024]; int heldn = 0;   /* ACB bytes not yet forwarded (HA call active) */                     /* HA remote call active: hold mainapp's 32/15 timeout */
    unsigned char buf[256];

    for (;;) {
        /* reap / restart the mqtt child */
        int st;
        if (child > 0 && waitpid(child, &st, WNOHANG) == child) {
            LOG("relay: mqtt child exited, restart in 5 s\n");
            child = -1; close(pairfd); pairfd = -1; restart_at = now_ms() + 5000;
        }
        if (child < 0 && now_ms() >= restart_at)
        { child = start_child(&pairfd, argv[3], real, m, s); send_state(pairfd); }

        struct pollfd p[3] = { { real, POLLIN, 0 }, { m, POLLIN, 0 }, { pairfd, POLLIN, 0 } };
        int np = pairfd >= 0 ? 3 : 2;
        if (poll(p, np, (audio_at || await17 || remote_until || fakelen || pcmd || rearm_at || cstate == ST_ANS_APP) ? 100 : 1000) < 0 && errno != EINTR) break;

        if (p[0].revents & POLLIN) {            /* ACB -> mainapp */
            ssize_t n = read(real, buf, sizeof buf);
            if (n > 0) {
                /* Outside a HA call: forward first, as before. During a HA call the bytes go through 'held'
                   so that a ring frame can be removed before mainapp sees it (T30). */
                int gate = remote_until != 0, base = heldn, shift = 0;
                if (!gate) { if (heldn) { write_all(m, held, heldn); heldn = 0; } write_all(m, buf, n); }
                else if (heldn + n <= (int)sizeof held) { memcpy(held + heldn, buf, n); heldn += n; }
                else { write_all(m, held, heldn); heldn = 0; write_all(m, buf, n); gate = 0; }   /* overflow: never lose bytes */
                for (ssize_t i = 0; i < n; i++) {
                    int l = parse_byte(&acb, buf[i]);
                    if (!l) continue;
                    if (verbose) { LOG("ACB>"); for (int k = 0; k < l; k++) LOG(" %02x", acb.b[k]); LOG("\n"); }
                    if (acb.b[2] == 0x10 && gate) {   /* new ring during a HA call: keep talking, mainapp never sees it
                                                         (no ringtone, AO untouched); HA still gets the ring event below */
                        int end = base + (int)i + 1 - shift, start = end - l;
                        if (start >= 0) {
                            memmove(held + start, held + end, heldn - end); heldn -= l; shift += l;
                            LOG("relay: ring during HA call: not forwarded to mainapp, call continues\n");
                        }
                    }
                    if (acb.b[2] == 0x10) {
                        /* a ring while someone only watches is still a ring: Answer remotely must take the ring
                           path (fake 12 02 stops mainapp's ringtone), else 43 01 puts the chime on the bus (T39) */
                        /* answered_app too: that call ended without an end frame (T40 test 4) */
                        if (cstate == ST_IDLE || cstate == ST_VIEWING || cstate == ST_ANS_APP) {
                            set_state(ST_RINGING, pairfd, "ACB ring"); ring_start = now_ms();
                        }
                        if (cstate == ST_RINGING) ring_at = now_ms();
                    }
                    if (acb.b[2] == 0x12 && l >= 5) {
                        if (acb.b[3] == 0x05) {                 /* bus idle */
                            set_state(ST_IDLE, pairfd, "ACB 12 05");
                            audio_state(0);
                            remote_until = 0; await17 = 0; audio_at = 0; talk_faked = 0;  /* mainapp got the real 12 05 */
                        } else if (acb.b[3] == 0x03 && cstate == ST_ANS_APP) {
                            set_state(ST_IDLE, pairfd, "ACB 12 03");
                        } else if (acb.b[3] == 0x02 && cstate != ST_ANS_REMOTE)
                            set_state(ST_ANS_MONITOR, pairfd, "ACB 12 02 answered elsewhere");
                    }
                    if (acb.b[2] == 0x10 && l >= 5 && pairfd >= 0) {
                        char ev[16]; int el = snprintf(ev, sizeof ev, "R%u\n", acb.b[3]);
                        write(pairfd, ev, el); /* non-blocking; dropped if child busy */
                    }
                }
                if (heldn) {                    /* forward all but an incomplete trailing frame */
                    int keep = (int)acb.pos < heldn ? (int)acb.pos : 0;
                    write_all(m, held, heldn - keep);
                    memmove(held, held + heldn - keep, keep); heldn = keep;
                }
            }
        }
        if (p[1].revents & POLLIN) {            /* mainapp -> ACB */
            ssize_t n = read(m, buf, sizeof buf);
            for (ssize_t i = 0; i < n; i++) {
                unsigned char c = buf[i];
                if (c == 0xFE && gk.pos == 1 && gkn) { write_all(real, gkb, gkn); gkn = 0; } /* stray FE */
                int l = parse_byte(&gk, c);
                gkb[gkn++] = c;
                if (l) {                        /* complete valid frame from mainapp */
                    /* during a HA call: mainapp's own end/stop frames must not end the bus session; 41 comes
                       when a LAN/app viewer disconnects (T34) */
                    /* 43 too: after the early fake 12 05 (T39) mainapp may send its own 43 00, which would switch
                       the call audio off on the bus */
                    if (remote_until && gkb[2] == 0x12 && gkn >= 5 && gkb[3] == 0x02) {
                        /* the indoor monitor takes over the HA call (answer at the monitor): from now on it owns the
                           call, so its end frames must reach the bus again; HA audio off (T57). Before, they were
                           dropped until the 120 s limit: monitor audio left on, HA stuck in answered_monitor */
                        remote_until = 0; await17 = 0; audio_at = 0; rearm_at = 0; talk_faked = 0;
                        audio_state(0);
                        LOG("relay: monitor took over the HA call\n");
                    }
                    int drop = remote_until && (gkb[2] == 0x15 || gkb[2] == 0x32 || gkb[2] == 0x41 || gkb[2] == 0x43);
                    if (await17 && gkb[2] == 0x17) { await17 = 0; audio_at = now_ms() + 300; }  /* ringtone stopped */
                    if (gkb[2] == 0x40) live_open = 1;
                    if (gkb[2] == 0x41) live_open = 0;
                    /* a 40 from mainapp (live view opened after Answer, or a viewer reconnecting) reaches the bus after our
                       43 01 and cuts the door->monitor audio; a new 43 01 brings it back (T41, door_peak test D1/D2) */
                    if (gkb[2] == 0x40 && remote_until && audio_on) rearm_at = now_ms() + 300;
                    if (!drop) {
                        write_all(real, gkb, gkn);
                        if (gkb[2] == 0x12 && gkn >= 5 && gkb[3] == 0x02) set_state(ST_ANS_MONITOR, pairfd, "monitor answered");
                        else if (gkb[2] == 0x40) {      /* remote monitor: app answering a ring, or someone viewing (T34) */
                            /* during a ring 40 alone is not an answer: a LAN viewer opening the stream sends it too.
                               The cloud app's answer follows it with 32 (stop ringtone), see below (T39) */
                            if (cstate == ST_RINGING) ring_viewer = 1;
                            else if (cstate == ST_IDLE) set_state(ST_VIEWING, pairfd, "viewing (remote monitor)");
                        }
                        else if (gkb[2] == 0x32 && cstate == ST_RINGING && ring_viewer)
                        { set_state(ST_ANS_APP, pairfd, "app answered"); app_at = now_ms(); }
                        else if (gkb[2] == 0x41 && cstate == ST_RINGING)
                            ring_viewer = 0;            /* a viewer's session ended (often ~13 s late): the ring goes on */
                        else if (gkb[2] == 0x15 || gkb[2] == 0x41) set_state(ST_IDLE, pairfd, "call ended");
                    }
                    if (verbose) { LOG(drop ? "DROP>" : "GK >"); for (int k = 0; k < gkn; k++) LOG(" %02x", gkb[k]); LOG("\n"); }
                    gkn = 0;
                } else if (gk.pos == 0 || gkn >= (int)sizeof gkb) {   /* not a frame / bad checksum: pass as is */
                    write_all(real, gkb, gkn);
                    if (verbose) { LOG("GK ?"); for (int k = 0; k < gkn; k++) LOG(" %02x", gkb[k]); LOG("\n"); }
                    gkn = 0;
                }
            }
        }
        if (np == 3 && (p[2].revents & (POLLIN | POLLHUP))) {   /* commands from mqtt */
            char rc;
            ssize_t n = read(pairfd, &rc, 1);
            if (n == 1) {                       /* queue it (latest wins); executed once injection is allowed (T30) */
                if (pcmd && pcmd != rc) LOG("relay: command %c replaced by %c\n", pcmd, rc);
                pcmd = rc;
            } else if (n == 0) {
                close(pairfd); pairfd = -1;     /* child gone; waitpid handles restart */
            }
        }
        if (pcmd && !pendlen && now_ms() - last_inject > 1000) {
            char c = pcmd; pcmd = 0;
            {
                unsigned char pl[2];
                if (c == 'E' || c == 'H') {
                    remote_until = 0; await17 = 0; audio_at = 0; set_state(ST_IDLE, pairfd, "HA end"); audio_state(0);
                    if (talk_faked) {           /* clear mainapp's DEVtalkState (base.lua: state 5 -> 0), else cloud video is refused */
                        unsigned char st = 5; fakelen = build_frame(fake, 0x12, &st, 1); talk_faked = 0;
                    }
                }
                if (c == 'R' && cstate == ST_ANS_APP && ring_start && now_ms() - ring_start < RINGTONE_MS) {
                    /* "app answered" = our LAN viewer opened during the ring (T41). The viewer start closed the call
                       page with the "phone" flag set, so mainapp's ringtone keeps playing and nothing local stops it
                       (T47/T48): 43 01 now puts a loud chime on the door bus and the ringtone player breaks talk-back.
                       Refuse until the longest ringtone is over */
                    LOG("relay: takeover refused: mainapp's ringtone may still play (%ld s left, T48)\n",
                        (RINGTONE_MS - (now_ms() - ring_start) + 999) / 1000);
                    c = 0;
                }
                if (c == 'R' && cstate == ST_ANS_APP) {
                    /* ringtone over: HA takes the call over. mainapp has already sent 40 01 + 32 + 17 and holds no call
                       audio; no fake 12 02 (on the page now open it only sets DEVtalkState, T47), audio on at once,
                       mainapp's end frames dropped as in any HA call */
                    remote_until = now_ms() + 120000; audio_at = now_ms(); rearm_at = 0;
                    set_state(ST_ANS_REMOTE, pairfd, "HA takes over the app-answered call");
                    c = 0;
                }
                if (c == 'R' && cstate == ST_ANS_MONITOR) {
                    LOG("relay: answer_remote refused, call held (%s)\n", st_name[cstate]);
                    c = 0;
                }
                if (c == 'R' && cstate == ST_ANS_REMOTE) {   /* a second card answering: the call is already up (T35) */
                    LOG("relay: answer_remote ignored, HA call already active\n");
                    c = 0;
                }
                if (c == 'G') { pl[0] = (2 << 2) | 1; pendlen = build_frame(pend, 0x14, pl, 1); } /* open gate: output 1, time 2 */
                else if (c == 'H') { pendlen = build_frame(pend, 0x15, NULL, 0); }               /* hang up */
                else if (c == 'A') { pl[0] = 2; pendlen = build_frame(pend, 0x12, pl, 1);       /* answer on monitor */
                                     set_state(ST_ANS_MONITOR, pairfd, "HA answer on monitor"); }
                else if (c == 'R') {    /* answer remotely, as the cloud app does: audio to SoC codec, not monitor speaker */
                    static const unsigned char z3[3] = { 0, 0, 0 };
                    pl[0] = 1; pendlen  = build_frame(pend, 0x40, pl, 1);            /* wifi_monitor ch 1 */
                    pendlen += build_frame(pend + pendlen, 0x32, z3, 3);             /* stop ringtone */
                    pendlen += build_frame(pend + pendlen, 0x17, NULL, 0);           /* sgm_idle */
                    remote_until = now_ms() + 120000;
                    if (cstate == ST_RINGING) {
                        /* tell mainapp "answered by another station" (Lua MONITOR_STATE.TALKED = 2): its call page
                           closes, stops its ringtone (MMP_PlayVoiceStop) and then sends sgm_idle (17) */
                        unsigned char st = 2; fakelen = build_frame(fake, 0x12, &st, 1); talk_faked = 1;
                        await17 = now_ms() + 3000;
                    } else {
                        /* no ring in progress (none, or it already timed out): no call page to close and no 17
                           will come, so turn the audio on directly (T30, RAM test 2026-11-06) */
                        audio_at = now_ms() + 500;
                    }
                    set_state(ST_ANS_REMOTE, pairfd, "HA answer remotely");
                } else if (c == 'E') {  /* end remote call, as the cloud app does */
                    pl[0] = 0; pendlen  = build_frame(pend, 0x43, pl, 1);            /* audio_led off */
                    /* 41 switches the door camera off; while mainapp still serves a live session that session would go
                       black and be reused by go2rtc. mainapp sends its own 43 00 + 41 when the session closes (T41) */
                    if (!live_open) pendlen += build_frame(pend + pendlen, 0x41, NULL, 0);   /* wifi_monitor_end */
                    else LOG("relay: live session open: no 41 (mainapp ends the bus monitor when it closes)\n");
                }
            }
        }
        if (cstate != ST_RINGING) ring_viewer = 0;
        if (cstate == ST_ANS_APP && now_ms() - app_at > 120000) set_state(ST_IDLE, pairfd, "answered_app timeout");
        if (!remote_until || !audio_on) rearm_at = 0;
        if (rearm_at && now_ms() >= rearm_at && !pendlen) {
            unsigned char on = 1; rearm_at = 0;
            pendlen = build_frame(pend, 0x43, &on, 1);
            LOG("relay: 43 01 again after mainapp's 40 (door audio)\n");
        }
        if (cstate == ST_RINGING && ring_at && now_ms() - ring_at > 60000) {   /* safety: a ring never lasts this long */
            ring_at = 0;
            set_state(ST_IDLE, pairfd, "ring timeout");
        }
        if (remote_until && now_ms() > remote_until && !pendlen) {   /* safety: auto End remote */
            unsigned char z = 0; remote_until = 0;
            LOG("relay: remote call 120 s limit, ending\n");
            set_state(ST_IDLE, pairfd, "120 s limit");
            pendlen  = build_frame(pend, 0x43, &z, 1);
            audio_state(0);
            if (!live_open) pendlen += build_frame(pend + pendlen, 0x41, NULL, 0);
            if (talk_faked) { unsigned char st = 5; fakelen = build_frame(fake, 0x12, &st, 1); talk_faked = 0; }
        }
        if (fakelen && acb.pos == 0 && heldn == 0) {   /* at a frame boundary of the ACB stream */
            write_all(m, fake, fakelen);
            if (verbose) { LOG("FAKE>"); for (int k = 0; k < fakelen; k++) LOG(" %02x", fake[k]); LOG("\n"); }
            fakelen = 0;
        }
        if (await17 && now_ms() > await17) {
            await17 = 0;
            LOG("relay: mainapp did not confirm (no 17), no auto audio\n");
        }
        if (audio_at && now_ms() >= audio_at && !pendlen) {
            unsigned char on = 1; audio_at = 0;
            pendlen = build_frame(pend, 0x43, &on, 1);                         /* audio on, as the app does */
            audio_state(1);
            LOG("relay: mainapp stood down, audio on\n");
            if (talk_faked && !fakelen) {
                /* mainapp has closed its call page: clear its "talking" state (fake 12 02 -> DEVtalkState 1) now
                   instead of at the end of the call. While it is set, mainapp accepts a LAN live session but
                   sends no frames, so the live view stays on "loading" without video or audio (T39) */
                unsigned char st = 5; fakelen = build_frame(fake, 0x12, &st, 1); talk_faked = 0;
                LOG("relay: early fake 12 05 (mainapp talk state cleared)\n");
            }
        }
        /* inject only at a frame boundary of mainapp's stream */
        if (pendlen && gk.pos == 0 && gkn == 0) {
            write_all(real, pend, pendlen);
            if (verbose) { LOG("INJ>"); for (int k = 0; k < pendlen; k++) LOG(" %02x", pend[k]); LOG("\n"); }
            pendlen = 0; last_inject = now_ms();
        }
    }
    return 1;
}

/* ===================================================================== */
/*  MQTT over TLS (child)                                                 */
/* ===================================================================== */
static char c_host[128], c_port[8] = "8883", c_user[64], c_pass[128], c_cert[256];
static char c_base[64] = "vimar/k40955", c_node[64] = "vimar_k40955";

static int load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { LOG("mqtt: cannot open %s\n", path); return -1; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *e = strchr(line, '=');
        if (!e || line[0] == '#') continue;
        *e++ = 0;
        e[strcspn(e, "\r\n")] = 0;
#define K(name, dst) if (!strcmp(line, name)) snprintf(dst, sizeof dst, "%s", e);
        K("host", c_host) K("port", c_port) K("user", c_user) K("pass", c_pass)
        K("cert", c_cert) K("base", c_base) K("node", c_node)
#undef K
    }
    fclose(f);
    return (c_host[0] && c_cert[0]) ? 0 : -1;
}

/* ---- pinned server key, extracted from the broker's certificate ---- */
static unsigned char der[8192]; static size_t derlen;
static void der_append(void *ctx, const void *d, size_t n)
{
    (void)ctx;
    if (derlen + n <= sizeof der) { memcpy(der + derlen, d, n); derlen += n; }
}
static unsigned char keybuf[1024];
static br_ec_public_key eckey; static br_rsa_public_key rsakey; static int keytype;

static int load_pinned_key(const char *path)
{
    static unsigned char file[16384];
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t fl = fread(file, 1, sizeof file, f);
    fclose(f);
    derlen = 0;
    if (fl > 10 && !memcmp(file, "-----", 5)) {    /* PEM: take first CERTIFICATE */
        br_pem_decoder_context pc;
        br_pem_decoder_init(&pc);
        size_t off = 0; int incert = 0, done = 0;
        while (off < fl && !done) {
            off += br_pem_decoder_push(&pc, file + off, fl - off);
            switch (br_pem_decoder_event(&pc)) {
            case BR_PEM_BEGIN_OBJ:
                incert = strstr(br_pem_decoder_name(&pc), "CERTIFICATE") != NULL;
                br_pem_decoder_setdest(&pc, incert ? der_append : NULL, NULL);
                break;
            case BR_PEM_END_OBJ: if (incert && derlen) done = 1; break;
            case BR_PEM_ERROR: return -1;
            }
        }
    } else {
        memcpy(der, file, fl); derlen = fl;
    }
    if (!derlen) return -1;
    br_x509_decoder_context dc;
    br_x509_decoder_init(&dc, NULL, NULL);
    br_x509_decoder_push(&dc, der, derlen);
    const br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk) return -1;
    keytype = pk->key_type;
    if (keytype == BR_KEYTYPE_EC) {
        if (pk->key.ec.qlen > sizeof keybuf) return -1;
        memcpy(keybuf, pk->key.ec.q, pk->key.ec.qlen);
        eckey.curve = pk->key.ec.curve; eckey.q = keybuf; eckey.qlen = pk->key.ec.qlen;
    } else if (keytype == BR_KEYTYPE_RSA) {
        if (pk->key.rsa.nlen + pk->key.rsa.elen > sizeof keybuf) return -1;
        memcpy(keybuf, pk->key.rsa.n, pk->key.rsa.nlen);
        memcpy(keybuf + pk->key.rsa.nlen, pk->key.rsa.e, pk->key.rsa.elen);
        rsakey.n = keybuf; rsakey.nlen = pk->key.rsa.nlen;
        rsakey.e = keybuf + pk->key.rsa.nlen; rsakey.elen = pk->key.rsa.elen;
    } else return -1;
    return 0;
}

/* ---- TLS engine plumbing (low-level BearSSL API) ---- */
static br_ssl_client_context cc;
static br_x509_minimal_context xcdummy;
static br_x509_knownkey_context kk;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static int sock = -1;

static int send_records(void)
{
    br_ssl_engine_context *e = &cc.eng;
    while (br_ssl_engine_current_state(e) & BR_SSL_SENDREC) {
        size_t len;
        unsigned char *b = br_ssl_engine_sendrec_buf(e, &len);
        ssize_t w = write(sock, b, len);
        if (w <= 0) { if (errno == EINTR) continue; return -1; }
        br_ssl_engine_sendrec_ack(e, w);
    }
    return 0;
}

static int recv_records(void)
{
    size_t len;
    unsigned char *b = br_ssl_engine_recvrec_buf(&cc.eng, &len);
    ssize_t r = read(sock, b, len);
    if (r <= 0) return -1;
    br_ssl_engine_recvrec_ack(&cc.eng, r);
    return 0;
}

static int tls_write(const unsigned char *d, size_t n)
{
    br_ssl_engine_context *e = &cc.eng;
    while (n > 0) {
        unsigned st = br_ssl_engine_current_state(e);
        if (st & BR_SSL_CLOSED) return -1;
        if (st & BR_SSL_SENDAPP) {
            size_t al;
            unsigned char *b = br_ssl_engine_sendapp_buf(e, &al);
            if (al > n) al = n;
            memcpy(b, d, al);
            br_ssl_engine_sendapp_ack(e, al);
            d += al; n -= al;
            continue;
        }
        if (st & BR_SSL_SENDREC) { if (send_records()) return -1; continue; }
        if (st & BR_SSL_RECVREC) {
            struct pollfd p = { sock, POLLIN, 0 };
            if (poll(&p, 1, 5000) <= 0 || recv_records()) return -1;
            continue;
        }
        return -1;
    }
    br_ssl_engine_flush(e, 0);
    return send_records();
}

/* ---- MQTT 3.1.1 encoding ---- */
static int put_str(unsigned char *b, const char *s)
{
    int l = strlen(s);
    b[0] = l >> 8; b[1] = l & 0xff;
    memcpy(b + 2, s, l);
    return l + 2;
}

static int send_packet(unsigned char type, const unsigned char *body, int bl)
{
    unsigned char hdr[5]; int hl = 0;
    hdr[hl++] = type;
    int x = bl;
    do { unsigned char d = x % 128; x /= 128; if (x) d |= 0x80; hdr[hl++] = d; } while (x);
    if (tls_write(hdr, hl)) return -1;
    return bl ? tls_write(body, bl) : 0;
}

static unsigned char pkt[2048];

static int mqtt_publish(const char *topic, const char *payload, int retain)
{
    int n = put_str(pkt, topic), pl = strlen(payload);
    if (n + pl > (int)sizeof pkt) return -1;
    memcpy(pkt + n, payload, pl);
    return send_packet(0x30 | (retain ? 1 : 0), pkt, n + pl);
}

static int mqtt_subscribe(unsigned short id, const char *topic)
{
    int n = 0;
    pkt[n++] = id >> 8; pkt[n++] = id & 0xff;
    n += put_str(pkt + n, topic);
    pkt[n++] = 0; /* QoS 0 */
    return send_packet(0x82, pkt, n);
}

static int mqtt_connect(void)
{
    char will[96]; snprintf(will, sizeof will, "%s/status", c_base);
    int n = 0;
    n += put_str(pkt + n, "MQTT");
    pkt[n++] = 4;                                        /* 3.1.1 */
    pkt[n++] = 0x02 | 0x04 | 0x20 | (c_user[0] ? 0x80 : 0) | (c_pass[0] ? 0x40 : 0);
    pkt[n++] = 0; pkt[n++] = 60;                         /* keepalive 60 s */
    n += put_str(pkt + n, c_node);                       /* client id */
    n += put_str(pkt + n, will);
    n += put_str(pkt + n, "offline");
    if (c_user[0]) n += put_str(pkt + n, c_user);
    if (c_pass[0]) n += put_str(pkt + n, c_pass);
    return send_packet(0x10, pkt, n);
}

static int publish_discovery(void)
{
    char topic[160], js[1024], dev[200];
    snprintf(dev, sizeof dev,
             "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"Vimar K40955\",\"manufacturer\":\"Vimar\",\"model\":\"K40955\"}",
             c_node);
    snprintf(topic, sizeof topic, "homeassistant/event/%s/doorbell/config", c_node);
    snprintf(js, sizeof js,
             "{\"name\":\"Doorbell\",\"unique_id\":\"%s_doorbell\",\"device_class\":\"doorbell\","
             "\"event_types\":[\"ring\"],\"state_topic\":\"%s/event\",\"availability_topic\":\"%s/status\",%s}",
             c_node, c_base, c_base, dev);
    if (mqtt_publish(topic, js, 1)) return -1;
    snprintf(topic, sizeof topic, "homeassistant/sensor/%s/call_state/config", c_node);
    snprintf(js, sizeof js,
             "{\"name\":\"Call state\",\"unique_id\":\"%s_call_state\",\"icon\":\"mdi:phone-in-talk\","
             "\"device_class\":\"enum\",\"options\":[\"idle\",\"ringing\",\"answered_monitor\",\"answered_app\",\"answered_HA\",\"viewing\"],"
             "\"state_topic\":\"%s/state\",\"availability_topic\":\"%s/status\",%s}",
             c_node, c_base, c_base, dev);
    if (mqtt_publish(topic, js, 1)) return -1;
    static const char *btn[][3] = {
        { "open_gate", "Open gate", "mdi:gate-open" },
        { "answer", "Answer on monitor", "mdi:phone" },
        { "answer_remote", "Answer remotely", "mdi:cellphone-sound" },
        { "end_remote", "End remote", "mdi:cellphone-off" },
        { "hang_up", "Hang up", "mdi:phone-hangup" },
    };
    for (unsigned i = 0; i < sizeof btn / sizeof btn[0]; i++) {
        snprintf(topic, sizeof topic, "homeassistant/button/%s/%s/config", c_node, btn[i][0]);
        snprintf(js, sizeof js,
                 "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"icon\":\"%s\",\"command_topic\":\"%s/cmd/%s\","
                 "\"payload_press\":\"PRESS\",\"availability_topic\":\"%s/status\",%s}",
                 btn[i][1], c_node, btn[i][0], btn[i][2], c_base, btn[i][0], c_base, dev);
        if (mqtt_publish(topic, js, 1)) return -1;
    }
    /* "Remote audio on" was removed (T30): an empty retained config deletes the old entity in HA */
    snprintf(topic, sizeof topic, "homeassistant/button/%s/remote_audio/config", c_node);
    if (mqtt_publish(topic, "", 1)) return -1;
    return mqtt_publish(strcat(strcpy(topic, c_base), "/status"), "online", 1);
}

/* ---- incoming MQTT stream ---- */
static unsigned char in[4096]; static size_t inlen;
static int pairfd_g, connacked;
static int cur_state = -1;                    /* last state from the relay */
/* same table as the relay (st_name): one list, so a new state cannot be missed here again (T34 bug) */
#define N_STATES ((int)(sizeof st_name / sizeof st_name[0]))
static int publish_state(void)
{
    char t[96];
    if (cur_state < 0 || cur_state >= N_STATES) return 0;   /* was a hard-coded '> 4': skipped 'viewing' */
    snprintf(t, sizeof t, "%s/state", c_base);
    return mqtt_publish(t, (cur_state >= 0 && cur_state < N_STATES) ? st_name[cur_state] : "idle", 1);
}

/* Video quality (HA switch "High quality video"): cmd/video_hq or cmd/video_std. Only in idle / viewing: boot.sh
   restarts mainapp with the HQ or the normal build (about 12 s), which would cut a ring or a call. The request is
   a file for boot.sh's supervisor; boot.sh keeps the choice on /mntfs for the next boot. */
#define APP_WANT "/tmp/vb/app.want"
static void video_request(const char *mode)
{
    if (cur_state != 0 && cur_state != 5) {          /* st_name: 0 idle, 5 viewing */
        LOG("mqtt: video quality %s refused, call state %d\n", mode, cur_state);
        return;
    }
    int fd = open(APP_WANT ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { LOG("mqtt: cannot write %s: %s\n", APP_WANT, strerror(errno)); return; }
    write(fd, mode, strlen(mode));
    write(fd, "\n", 1);
    close(fd);
    rename(APP_WANT ".tmp", APP_WANT);
    LOG("mqtt: video quality %s requested\n", mode);
}

static void on_publish(const unsigned char *b, size_t n)
{
    if (n < 2) return;
    size_t tl = (b[0] << 8) | b[1];
    if (2 + tl > n) return;
    char topic[160];
    if (tl >= sizeof topic) return;
    memcpy(topic, b + 2, tl); topic[tl] = 0;
    const char *pl = (const char *)b + 2 + tl; size_t pll = n - 2 - tl;
    char cmdt[96]; snprintf(cmdt, sizeof cmdt, "%s/cmd/", c_base);
    size_t cl = strlen(cmdt);
    if (!strcmp(topic, "homeassistant/status")) {
        if (pll == 6 && !memcmp(pl, "online", 6)) publish_discovery();  /* HA restarted */
        return;
    }
    if (strncmp(topic, cmdt, cl) || pll != 5 || memcmp(pl, "PRESS", 5)) return;
    const char *what = topic + cl; char c = 0;
    if (!strcmp(what, "open_gate")) c = 'G';
    else if (!strcmp(what, "hang_up")) c = 'H';
    else if (!strcmp(what, "answer")) c = 'A';
    else if (!strcmp(what, "answer_remote")) c = 'R';
    else if (!strcmp(what, "end_remote")) c = 'E';
    else if (!strcmp(what, "video_hq")) { video_request("hq"); return; }
    else if (!strcmp(what, "video_std")) { video_request("std"); return; }
    if (c) { LOG("mqtt: command %s\n", what); write(pairfd_g, &c, 1); }
}

static int process_input(void)
{
    for (;;) {
        if (inlen < 2) return 0;
        size_t rl = 0, mul = 1, i = 1;
        for (;; i++) {
            if (i >= inlen) return 0;
            rl += (in[i] & 0x7f) * mul; mul *= 128;
            if (!(in[i] & 0x80)) break;
            if (i >= 4) return -1;
        }
        size_t total = i + 1 + rl;
        if (total > sizeof in) return -1;
        if (inlen < total) return 0;
        unsigned char type = in[0] & 0xf0;
        const unsigned char *body = in + i + 1;
        if (type == 0x20 && rl >= 2) {                    /* CONNACK */
            if (body[1] != 0) { LOG("mqtt: connack refused rc=%d\n", body[1]); return -1; }
            connacked = 1;
        } else if (type == 0x30) {                        /* PUBLISH (QoS0 expected) */
            if (((in[0] >> 1) & 3) == 0) on_publish(body, rl);
        }                                                 /* SUBACK / PINGRESP: ignore */
        memmove(in, in + total, inlen - total); inlen -= total;
    }
}

static int pump_app(void)
{
    br_ssl_engine_context *e = &cc.eng;
    while (br_ssl_engine_current_state(e) & BR_SSL_RECVAPP) {
        size_t len;
        unsigned char *b = br_ssl_engine_recvapp_buf(e, &len);
        if (len > sizeof in - inlen) len = sizeof in - inlen;
        if (!len) return -1;
        memcpy(in + inlen, b, len); inlen += len;
        br_ssl_engine_recvapp_ack(e, len);
        if (process_input()) return -1;
    }
    return 0;
}

static int tcp_connect(void)
{
    struct sockaddr_in a = {0};                       /* numeric IP only: no DNS/NSS in a static binary */
    a.sin_family = AF_INET;
    a.sin_port = htons(atoi(c_port));
    if (inet_pton(AF_INET, c_host, &a.sin_addr) != 1) { LOG("mqtt: host must be an IP address\n"); return -1; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    int r = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (r) { close(fd); return -1; }
    return fd;
}

/* one connection lifetime; returns when it ends */
static void session(void)
{
    sock = tcp_connect();
    if (sock < 0) { LOG("mqtt: tcp connect to %s:%s failed\n", c_host, c_port); return; }
    br_ssl_client_init_full(&cc, &xcdummy, NULL, 0);
    if (keytype == BR_KEYTYPE_EC) br_x509_knownkey_init_ec(&kk, &eckey, BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN);
    else br_x509_knownkey_init_rsa(&kk, &rsakey, BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN);
    br_ssl_engine_set_x509(&cc.eng, &kk.vtable);
    br_ssl_engine_set_buffer(&cc.eng, iobuf, sizeof iobuf, 1);
    br_ssl_client_reset(&cc, NULL, 0);
    inlen = 0; connacked = 0;

    if (mqtt_connect()) goto out;                     /* drives the handshake too */
    long t0 = now_ms(), last_tx = now_ms(), last_rx = now_ms();
    int discovered = 0;
    for (;;) {
        br_ssl_engine_context *e = &cc.eng;
        unsigned st = br_ssl_engine_current_state(e);
        if (st & BR_SSL_CLOSED) break;
        if (st & BR_SSL_SENDREC) { if (send_records()) break; continue; }
        if (st & BR_SSL_RECVAPP) { if (pump_app()) break; last_rx = now_ms(); continue; }

        if (connacked && !discovered) {
            char t[96];
            if (publish_discovery()) break;
            snprintf(t, sizeof t, "%s/cmd/+", c_base);
            if (mqtt_subscribe(1, t) || mqtt_subscribe(2, "homeassistant/status")) break;
            if (publish_state()) break;
            discovered = 1; last_tx = now_ms();
            LOG("mqtt: connected, discovery published\n");
            continue;
        }
        if (!connacked && now_ms() - t0 > 15000) { LOG("mqtt: no CONNACK\n"); break; }

        long wait = 30000 - (now_ms() - last_tx);
        if (wait < 0) wait = 0;
        struct pollfd p[2] = { { sock, (st & BR_SSL_RECVREC) ? POLLIN : 0, 0 }, { pairfd_g, POLLIN, 0 } };
        int r = poll(p, 2, wait);
        if (r < 0 && errno != EINTR) break;
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) { if (recv_records()) break; continue; }
        if (p[1].revents & POLLHUP) _exit(0);         /* relay gone */
        if (p[1].revents & POLLIN) {                  /* ring events from relay */
            char ev[64]; ssize_t n = read(pairfd_g, ev, sizeof ev - 1);
            if (n <= 0) _exit(0);
            ev[n] = 0;
            for (char *q = ev; (q = strchr(q, 'S')); q++) {   /* call state changes */
                cur_state = atoi(q + 1);
                if (connacked && discovered && publish_state()) goto out;
                last_tx = now_ms();
            }
            for (char *q = ev; (q = strchr(q, 'R')); q++) {
                char t[96], js[64];
                snprintf(t, sizeof t, "%s/event", c_base);
                snprintf(js, sizeof js, "{\"event_type\":\"ring\",\"station\":%d}", atoi(q + 1));
                if (connacked && mqtt_publish(t, js, 0)) goto out;
                LOG("mqtt: ring published\n");
                last_tx = now_ms();
            }
            continue;
        }
        if (now_ms() - last_rx > 90000) { LOG("mqtt: broker silent, reconnect\n"); break; }
        if (now_ms() - last_tx >= 30000) {            /* MQTT keepalive ping */
            if (send_packet(0xC0, NULL, 0)) break;
            last_tx = now_ms();
        }
    }
out:
    if (br_ssl_engine_last_error(&cc.eng)) LOG("mqtt: tls error %d\n", br_ssl_engine_last_error(&cc.eng));
    close(sock); sock = -1;
}

static int mqtt_main(int pairfd, const char *cfgpath)
{
    pairfd_g = pairfd;
    if (load_config(cfgpath)) { LOG("mqtt: bad config\n"); sleep(30); return 1; }
    if (load_pinned_key(c_cert)) { LOG("mqtt: cannot load broker cert %s\n", c_cert); sleep(30); return 1; }
    int backoff = 2;
    for (;;) {
        long t = now_ms();
        session();
        if (now_ms() - t > 60000) backoff = 2;        /* had a good session */
        LOG("mqtt: disconnected, retry in %d s\n", backoff);
        /* keep draining ring events while offline so the relay never blocks */
        long until = now_ms() + backoff * 1000L;
        while (now_ms() < until) {
            struct pollfd p = { pairfd_g, POLLIN, 0 };
            if (poll(&p, 1, 500) > 0) {
                char tmp[64];
                if (read(pairfd_g, tmp, sizeof tmp) <= 0) _exit(0);
            }
        }
        if (backoff < 60) backoff *= 2;
    }
}
