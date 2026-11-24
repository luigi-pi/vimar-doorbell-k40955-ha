/*
 * vimarlog - non-blocking sender for the local Vimar diagnostic socket.
 *
 * Usage: vimarlog <component> <level> <message>
 * The MQTT child of vimarbridge owns /tmp/vb/log.sock and forwards accepted
 * messages to <base>/log. Missing/full socket: drop the log; never block the
 * intercom or the supervisor.
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define LOG_SOCKET "/tmp/vb/log.sock"
#define MAX_PACKET 768

static int allowed(const char *s, const char *const *values, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) if (!strcmp(s, values[i])) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    static const char *const components[] = {"supervisor", "audio", "bridge", "camera"};
    static const char *const levels[] = {"ERROR", "WARNING", "INFO", "DEBUG"};
    struct sockaddr_un addr;
    char packet[MAX_PACKET];
    size_t used, i;
    int fd;
    ssize_t sent;

    if (argc < 4 || !allowed(argv[1], components, sizeof components / sizeof components[0]) ||
        !allowed(argv[2], levels, sizeof levels / sizeof levels[0]))
        return 0; /* Logging must never make the caller fail. */

    used = (size_t)snprintf(packet, sizeof packet, "%s\t%s\t", argv[1], argv[2]);
    if (used >= sizeof packet) return 0;
    for (i = 3; i < (size_t)argc && used + 1 < sizeof packet; i++) {
        const unsigned char *p = (const unsigned char *)argv[i];
        while (*p && used + 1 < sizeof packet) {
            char c = (char)*p++;
            packet[used++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
        }
        if (i + 1 < (size_t)argc && used + 1 < sizeof packet) packet[used++] = ' ';
    }
    packet[used] = '\0';

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", LOG_SOCKET);
    sent = sendto(fd, packet, used, MSG_DONTWAIT, (struct sockaddr *)&addr, sizeof addr);
    (void)sent;
    close(fd);
    return 0;
}
