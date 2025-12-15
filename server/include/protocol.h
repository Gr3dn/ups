#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

#define READ_BUF 256

// === C45 framed protocol ===
#define C45_MAX_PAYLOAD 99

// 1 — ok; 0 — without C45; -2 — lengt error; -3 — small buffer
int c45_parse_line(const char* line, char* out_payload, size_t out_sz);

// 0 — ok; -1 — payload > 99 or does not fit in the buffer
int c45_build_frame(const char* payload, char* out, size_t out_sz);

// 0 — ok; -1 — write_all() error
int send_c45(int fd, const char* payload);

// >0 — payload length; 0 — EOF; -100 — no C45; -101 — length error; -1 — other error
int read_c45(int fd, char* out_payload, size_t out_sz);


/* Safe recording of the entire string (0=OK, -1=err) */
int  write_all(int fd, const char* s);

/* Read one line up to ‘\n’ (truncates by buffer, always 0-terminates).
   Return: >=0 length, 0 — client closed before data, -1 — error. */
int  read_line(int fd, char* buf, size_t buf_sz);

/* Checking the prefix “C45” */
int  is_c45_prefix(const char* s);

int  send_lobbies_snapshot(int fd);

int read_line_timeout(int fd, char* buf, size_t sz, int timeout_sec);

#endif /* PROTOCOL_H */