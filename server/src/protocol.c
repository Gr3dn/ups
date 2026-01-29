/*
 * protocol.c
 *
 * Purpose:
 *   Implementation of low-level line I/O and protocol helpers used by the server:
 *   - Safe "write all" for TCP sockets.
 *   - Line-oriented reads (blocking and timed).
 *   - C45 framed-message helpers.
 *   - Lobby snapshot serialization.
 *
 * Table of contents:
 *   - write_all()
 *   - read_line(), read_line_timeout()
 *   - is_c45_prefix()
 *   - send_lobbies_snapshot()
 *   - C45 framing: c45_parse_line(), c45_build_frame(), send_c45(), read_c45()
 */

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

/**
 * Write an entire NUL-terminated string to a socket.
 *
 * @param fd Connected socket file descriptor.
 * @param s  NUL-terminated string to send.
 *
 * @return 0 on success; -1 on error.
 */
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

/**
 * Read a single line from a socket into a buffer (byte-by-byte).
 *
 * @param fd      Connected socket file descriptor.
 * @param buf     Destination buffer.
 * @param buf_sz  Size of @p buf in bytes.
 *
 * @return >=0 Length of data stored in @p buf (including '\n' if present).
 * @return  0  Peer closed the connection.
 * @return -1  Error.
 */
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

/**
 * Check whether the string starts with "C45".
 *
 * @param s NUL-terminated string.
 * @return 1 if prefix matches; 0 otherwise.
 */
int is_c45_prefix(const char* s) {
    return s && strncmp(s, "C45", 3) == 0;
}

/**
 * Send the lobby list snapshot to a client.
 *
 * @param fd Connected socket file descriptor.
 * @return 0 on success; -1 on error.
 */
int send_lobbies_snapshot(int fd) {
    // Compact snapshot (single line) to keep the protocol usable under extreme
    // fragmentation/delay (e.g., 1 byte per packet, high RTT).
    //
    // Format:
    //   C45L <n> <pairs>\n
    // where <pairs> is 2*n digits, each pair is:
    //   players (0..2) + status (0/1)
    //
    // Example for 3 lobbies:
    //   C45L 3 001020\n
    char out[512];
    int n = g_lobby_count;
    if (n < 0) n = 0;
    if (n > 200) n = 200; // bound the line length; the Java client also limits lobby count

    int pos = snprintf(out, sizeof(out), "C45L %d ", n);
    if (pos < 0 || (size_t)pos >= sizeof(out)) return -1;

    for (int i = 0; i < n; ++i) {
        int players, status;
        pthread_mutex_lock(&g_lobbies[i].mtx);
        players = g_lobbies[i].player_count;
        status  = g_lobbies[i].is_running ? 1 : 0;
        pthread_mutex_unlock(&g_lobbies[i].mtx);

        if (players < 0) players = 0;
        if (players > 9) players = 9;
        status = status ? 1 : 0;

        if ((size_t)(pos + 2) >= sizeof(out)) return -1;
        out[pos++] = (char)('0' + players);
        out[pos++] = (char)('0' + status);
    }

    if ((size_t)(pos + 2) >= sizeof(out)) return -1;
    out[pos++] = '\n';
    out[pos] = '\0';

    if (write_all(fd, out) < 0) return -1;
    printf("[PROTO] -> Send lobby snapshot to client (fd=%d)\n", fd);
    return 0;
}

/**
 * Read a single line with a poll()-based timeout.
 *
 * @param fd            Connected socket file descriptor.
 * @param buf           Destination buffer.
 * @param sz            Size of @p buf in bytes.
 * @param timeout_sec   Timeout in seconds for the first read attempt.
 *
 * @return >=0 Length of data stored in @p buf (including '\n' if present).
 * @return  0  Peer closed the connection.
 * @return -2  Timeout expired.
 * @return -1  Error.
 */
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

/**
 * Parse a C45 framed line into a payload buffer.
 *
 * @param line   NUL-terminated input line.
 * @param out    Destination payload buffer.
 * @param out_sz Size of @p out in bytes.
 *
 * @return  1  Parsed successfully.
 * @return  0  Not a C45 frame.
 * @return -2  Invalid length digits or length mismatch.
 * @return -3  Output buffer too small.
 */
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

/**
 * Build a C45 framed line from a payload.
 *
 * @param payload NUL-terminated payload string (NULL treated as empty string).
 * @param out     Destination buffer for the resulting frame.
 * @param out_sz  Size of @p out in bytes.
 *
 * @return 0 on success; -1 on error.
 */
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

/**
 * Send a payload as a C45 framed line.
 *
 * @param fd      Connected socket file descriptor.
 * @param payload Payload string.
 *
 * @return 0 on success; -1 on error.
 */
int send_c45(int fd, const char* payload) {
    char buf[3 + 2 + C45_MAX_PAYLOAD + 2]; // "C45" + len + payload + "\n\0"
    if (c45_build_frame(payload, buf, sizeof(buf)) != 0) return -1;
    return write_all(fd, buf) < 0 ? -1 : 0;
}

/**
 * Read a line from a socket and decode it as a C45 frame.
 *
 * @param fd          Connected socket file descriptor.
 * @param out_payload Destination buffer for payload.
 * @param out_sz      Size of @p out_payload in bytes.
 *
 * @return >0   Payload length.
 * @return  0   EOF (peer closed).
 * @return -100 Not a C45 frame.
 * @return -101 Length error.
 * @return -1   Other I/O error.
 */
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
