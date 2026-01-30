/*
 * protocol.c
 *
 * Purpose:
 *   Implementation of low-level line I/O and protocol helpers used by the server:
 *   - Safe "write all" for TCP sockets.
 *   - Line-oriented reads (blocking and timed).
 *   - Lobby snapshot serialization.
 *
 * Table of contents:
 *   - write_all()
 *   - read_line(), read_line_timeout()
 *   - is_c45_prefix()
 *   - send_lobbies_snapshot()
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
