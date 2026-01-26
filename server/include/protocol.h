#ifndef PROTOCOL_H
#define PROTOCOL_H

/*
 * protocol.h
 *
 * Purpose:
 *   Shared networking/protocol helpers for the Blackjack server:
 *   - Line-based TCP I/O helpers (read/write whole lines safely).
 *   - Minimal C45 framed-message helpers (length-prefixed payload in one line).
 *
 * Table of contents:
 *   - Constants: READ_BUF, C45_MAX_PAYLOAD
 *   - C45 framing: c45_parse_line(), c45_build_frame(), send_c45(), read_c45()
 *   - Line I/O: write_all(), read_line(), read_line_timeout()
 *   - Misc: is_c45_prefix(), send_lobbies_snapshot()
 */

#include <stddef.h>

#define READ_BUF 256

/* --- C45 framed protocol --- */
#define C45_MAX_PAYLOAD 99

/**
 * Parse a C45 framed line into the payload buffer.
 *
 * Frame format (line must already include the "C45" prefix):
 *   "C45" + 2 decimal digits (payload length) + payload + "\n"
 *
 * @param line         NUL-terminated input line (may include trailing '\n').
 * @param out_payload  Destination buffer for the decoded payload.
 * @param out_sz       Size of @p out_payload in bytes.
 *
 * @return  1  Parsed successfully; payload is written to @p out_payload.
 * @return  0  Not a C45 frame (missing "C45" prefix).
 * @return -2  Invalid length digits or length mismatch.
 * @return -3  @p out_payload is too small.
 */
int c45_parse_line(const char* line, char* out_payload, size_t out_sz);

/**
 * Build a C45 framed line from a payload string.
 *
 * @param payload  NUL-terminated payload string (length must be <= C45_MAX_PAYLOAD).
 * @param out      Destination buffer for the resulting frame.
 * @param out_sz   Size of @p out in bytes.
 *
 * @return  0  Frame built successfully into @p out (NUL-terminated).
 * @return -1  Payload too long or @p out is too small.
 */
int c45_build_frame(const char* payload, char* out, size_t out_sz);

/**
 * Send a payload as a C45 frame.
 *
 * @param fd       Connected socket file descriptor.
 * @param payload  Payload string (length must be <= C45_MAX_PAYLOAD).
 *
 * @return  0 on success; -1 on socket write error.
 */
int send_c45(int fd, const char* payload);

/**
 * Read one line and decode it as a C45 frame.
 *
 * @param fd           Connected socket file descriptor.
 * @param out_payload  Destination buffer for payload.
 * @param out_sz       Size of @p out_payload in bytes.
 *
 * @return >0   Payload length (and payload is written to @p out_payload).
 * @return  0   Peer closed the connection (EOF).
 * @return -100 Line is not a C45 frame.
 * @return -101 C45 length digits/mismatch error.
 * @return -1   Other I/O error.
 */
int read_c45(int fd, char* out_payload, size_t out_sz);


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
