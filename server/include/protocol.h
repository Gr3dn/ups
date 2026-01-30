#ifndef PROTOCOL_H
#define PROTOCOL_H

/*
 * protocol.h
 *
 * Purpose:
 *   Shared networking/protocol helpers for the Blackjack server:
 *   - Line-based TCP I/O helpers (read/write whole lines safely).
 *
 * Table of contents:
 *   - Constants: READ_BUF
 *   - Line I/O: write_all(), read_line(), read_line_timeout()
 *   - Misc: is_c45_prefix(), send_lobbies_snapshot()
 */

#include <stddef.h>

#define READ_BUF 256


/**
 * Write the full NUL-terminated string to a socket.
 *
 * @param fd  Connected socket file descriptor.
 * @param s   NUL-terminated string to send.
 *
 * @return 0 on success; -1 on error.
 */
int  write_all(int fd, const char* s);

/**
 * Read one text line (up to '\n') from a socket.
 *
 * The line is always NUL-terminated. If the input line is longer than the buffer,
 * it is truncated.
 *
 * @param fd      Connected socket file descriptor.
 * @param buf     Destination buffer.
 * @param buf_sz  Size of @p buf in bytes.
 *
 * @return >=0 Length of the stored line in @p buf (including '\n' if present).
 * @return  0  Peer closed the connection before any data was read.
 * @return -1  Error.
 */
int  read_line(int fd, char* buf, size_t buf_sz);

/**
 * Check whether a line starts with the "C45" prefix.
 *
 * @param s NUL-terminated string.
 * @return 1 if @p s starts with "C45"; 0 otherwise.
 */
int  is_c45_prefix(const char* s);

/**
 * Send the current lobby snapshot to a client.
 *
 * @param fd Connected socket file descriptor.
 * @return 0 on success; -1 on error.
 */
int  send_lobbies_snapshot(int fd);

/**
 * Read one line with an overall timeout.
 *
 * Internally uses poll() to wait for data and then reads byte-by-byte.
 *
 * @param fd           Connected socket file descriptor.
 * @param buf          Destination buffer.
 * @param sz           Size of @p buf in bytes.
 * @param timeout_sec  Initial timeout in seconds for the first byte.
 *
 * @return >=0 Length of the stored line in @p buf (including '\n' if present).
 * @return  0  Peer closed the connection.
 * @return -2  Timeout expired before the full line was read.
 * @return -1  Error.
 */
int read_line_timeout(int fd, char* buf, size_t sz, int timeout_sec);

#endif /* PROTOCOL_H */
