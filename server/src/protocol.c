#include "protocol.h"
#include "game.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int write_all(int fd, const char* s) {
    size_t n = strlen(s);
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, s + off, n - off, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

int read_line(int fd, char* buf, size_t buf_sz) {
    if (buf_sz == 0) return -1;
    size_t pos = 0;
    while (pos < buf_sz - 1) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r == 0) { /* peer closed */
            buf[pos] = '\0';
            return 0;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf[pos++] = c;
        if (c == '\n') break;
    }
    buf[pos] = '\0';
    return (int)pos;
}

int is_c45_prefix(const char* s) {
    return s && strncmp(s, "C45", 3) == 0;
}

int send_lobbies_snapshot(int fd) {
    char line[128];
    sleep(2);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "C45LOBBIES %d\n", g_lobby_count);
    if (write_all(fd, hdr) < 0) return -1;

    for (int i = 0; i < g_lobby_count; ++i) {
        int players, status;
        pthread_mutex_lock(&g_lobbies[i].mtx);
        players = g_lobbies[i].player_count;
        status  = g_lobbies[i].is_running ? 1 : 0;
        pthread_mutex_unlock(&g_lobbies[i].mtx);

        snprintf(line, sizeof(line),
                 "C45LOBBY %d players=%d/%d status=%d\n",
                 i+1, players, LOBBY_SIZE, status);
        if (write_all(fd, line) < 0) return -1;
    }
    if (write_all(fd, "C45END\n") < 0) return -1;

    printf("[PROTO] -> Send snapsho of lobby to client (fd=%d)\n", fd);
    return 0;
}

int read_line_timeout(int fd, char* buf, size_t sz, int t) {
    size_t pos = 0;
    while (pos < sz - 1) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int pr = poll(&p, 1, t * 1000);
        if (pr == 0) return -2;       // timeout
        if (pr < 0) return -1;
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return r;
        buf[pos++] = c;
        if (c == '\n') break;
        t = 30;
    }
    buf[pos] = '\0';
    return (int)pos;
}

int c45_parse_line(const char* line, char* out, size_t out_sz) {
    if (!line) return 0;
    if (strncmp(line, "C45", 3) != 0) return 0;
    if (line[3] < '0' || line[3] > '9' || line[4] < '0' || line[4] > '9') {
        return -2;
    }


    int len = (line[3]-'0')*10 + (line[4]-'0');
    if (len < 0 || len > C45_MAX_PAYLOAD) return -2;

    const char* payload = line + 5;
    int actual = (int)strlen(payload);
    if (actual != len) return -2;
    if ((size_t)(len+1) > out_sz) return -3;

    memcpy(out, payload, (size_t)len);
    out[len] = '\0';
    return 1;
}

int c45_build_frame(const char* payload, char* out, size_t out_sz) {
    if (!payload) payload = "";
    int len = (int)strlen(payload);
    if (len < 0 || len > C45_MAX_PAYLOAD) return -1;
    // "C45" + 2 lenDig + payload + "\n" + '\0'
    size_t need = 3 + 2 + (size_t)len + 1 + 1;
    if (out_sz < need) return -1;
    snprintf(out, out_sz, "C45%02d%s\n", len, payload);
    return 0;
}

int send_c45(int fd, const char* payload) {
    char buf[3 + 2 + C45_MAX_PAYLOAD + 2]; // "C45" + len + payload + "\n\0"
    if (c45_build_frame(payload, buf, sizeof(buf)) != 0) return -1;
    return write_all(fd, buf) < 0 ? -1 : 0;
}

int read_c45(int fd, char* out_payload, size_t out_sz) {
    char line[READ_BUF];
    int r = read_line(fd, line, sizeof(line));
    if (r == 0) return 0;
    if (r < 0)  return -1;
    int pr = c45_parse_line(line, out_payload, out_sz);
    if (pr == 1) return (int)strlen(out_payload);
    if (pr == 0)  return -100;
    if (pr == -2) return -101;
    return -1;
}
