/*
 * server.c
 *
 * Purpose:
 *   TCP server implementation for the Blackjack project.
 *
 * Responsibilities:
 *   - Accept client connections and run one thread per client.
 *   - Perform handshake (name registration) and lobby selection.
 *   - Start game threads when lobbies become full.
 *   - Support keep-alive (PING/PONG) and reconnect into a running game.
 *   - Maintain a global "active name" registry to prevent duplicates and to
 *     coordinate "back to lobby" requests across threads.
 *
 * Table of contents:
 *   - Signal handling: on_sigint()
 *   - Active name registry: active_name_*()
 *   - Parsing helpers: parse_name_only(), parse_name_lobby(), is_back_request_for()
 *   - Client thread state machine: client_thread()
 *   - Server loop: run_server()
 */

#define _GNU_SOURCE
#include "server.h"
#include "protocol.h"
#include "game.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define ACTIVE_MAX 256
#define CLIENT_FD_MAX 1024
#define WAITING_INTERVAL_SEC 5
// Reserving names among all active connections (until disconnect)
static pthread_mutex_t g_names_mtx = PTHREAD_MUTEX_INITIALIZER;
static char g_active_names[ACTIVE_MAX][MAX_NAME_LEN];
static int  g_active_fds[ACTIVE_MAX];
static int  g_active_back_req[ACTIVE_MAX];
static unsigned long long g_active_tokens[ACTIVE_MAX];
static unsigned long long g_token_seq = 1;
static int  g_active_cnt = 0;

// Connected client sockets (including those who haven't completed handshake yet).
static pthread_mutex_t g_clients_mtx = PTHREAD_MUTEX_INITIALIZER;
static int  g_client_fds[CLIENT_FD_MAX];
static int  g_client_cnt = 0;

/**
 * Register a connected client socket file descriptor.
 *
 * This registry is used to broadcast server shutdown notifications and to
 * force-disconnect all clients promptly (including those still in handshake).
 *
 * @param fd Connected socket file descriptor.
 */
static void client_fd_add(int fd) {
    pthread_mutex_lock(&g_clients_mtx);
    if (g_client_cnt < CLIENT_FD_MAX) {
        // Prevent duplicates (should not happen, but keeps the registry robust).
        for (int i = 0; i < g_client_cnt; ++i) {
            if (g_client_fds[i] == fd) {
                pthread_mutex_unlock(&g_clients_mtx);
                return;
            }
        }
        g_client_fds[g_client_cnt++] = fd;
    }
    pthread_mutex_unlock(&g_clients_mtx);
}

/**
 * Unregister a client socket file descriptor.
 *
 * @param fd Socket file descriptor to remove.
 */
static void client_fd_remove(int fd) {
    pthread_mutex_lock(&g_clients_mtx);
    for (int i = 0; i < g_client_cnt; ++i) {
        if (g_client_fds[i] == fd) {
            g_client_fds[i] = g_client_fds[g_client_cnt - 1];
            g_client_cnt--;
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_mtx);
}

/**
 * Best-effort notify all connected clients about server shutdown and disconnect them.
 *
 * This is used when the server is stopping (SIGINT) or when it detects that its
 * bind address is no longer available (network/interface changes).
 *
 * The notification is best-effort: if a socket can't be written to, we still
 * shutdown() it to unblock per-client threads quickly.
 *
 * @param reason Optional single-token reason (e.g. "SIGINT", "NETWORK_LOST").
 */
static void server_notify_and_disconnect_all(const char* reason) {
    int fds[CLIENT_FD_MAX];
    int cnt = 0;

    pthread_mutex_lock(&g_clients_mtx);
    cnt = g_client_cnt;
    if (cnt > CLIENT_FD_MAX) cnt = CLIENT_FD_MAX;
    for (int i = 0; i < cnt; ++i) fds[i] = g_client_fds[i];
    pthread_mutex_unlock(&g_clients_mtx);

    char msg[128];
    if (reason && *reason) snprintf(msg, sizeof(msg), "C45SERVER_DOWN %s\n", reason);
    else snprintf(msg, sizeof(msg), "C45SERVER_DOWN\n");
    size_t len = strlen(msg);

    for (int i = 0; i < cnt; ++i) {
        int fd = fds[i];
        (void)send(fd, msg, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        (void)shutdown(fd, SHUT_RDWR);
    }
}

/**
 * Check whether the server bind IPv4 address is still present on any interface.
 *
 * This helps detect cases where the server process keeps running but becomes
 * unreachable because the configured bind address disappeared (e.g. Wi‑Fi off,
 * VPN/WSL IP change).
 *
 * For "localhost" this check is skipped.
 *
 * For "0.0.0.0" (INADDR_ANY) we consider "network is gone" to be "no non-loopback IPv4
 * interface is UP". Use "localhost" for a local-only server.
 *
 * @param bind_ip Bind address passed to run_server().
 * @return 1 if the bind IP is available (or check is skipped); 0 if missing.
 */
static int is_bind_ip_available(const char* bind_ip) {
    struct ifaddrs* ifaddr = NULL;

    // INADDR_ANY: treat "network is gone" as "no non-loopback IPv4 interface is UP".
    // If you want a local-only server that doesn't depend on external network state, bind to "localhost".
    if (!bind_ip || strcmp(bind_ip, "0.0.0.0") == 0) {
        if (getifaddrs(&ifaddr) != 0) return 1; // best-effort: assume OK if we can't query
        int ok = 0;
        for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP)) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            ok = 1;
            break;
        }
        freeifaddrs(ifaddr);
        return ok;
    }

    if (strcmp(bind_ip, "localhost") == 0) return 1;

    struct in_addr target;
    if (inet_pton(AF_INET, bind_ip, &target) != 1) return 1;

    if (getifaddrs(&ifaddr) != 0) return 1; // best-effort: assume OK if we can't query

    int found = 0;
    for (struct ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        struct sockaddr_in* sa = (struct sockaddr_in*)ifa->ifa_addr;
        if (sa->sin_addr.s_addr == target.s_addr) {
            found = 1;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return found;
}

/**
 * Check whether a received line matches a protocol token exactly.
 *
 * This prevents prefix collisions (e.g. a player name starting with "PING")
 * by requiring the token to be followed by end-of-string or whitespace.
 *
 * @param line Full received line (NUL-terminated).
 * @param tok  Token string to match (e.g. "C45PING").
 * @return 1 if @p line begins with @p tok and is followed by end/whitespace; 0 otherwise.
 */
static int is_token(const char* line, const char* tok) {
    if (!line || !tok) return 0;
    size_t n = strlen(tok);
    if (strncmp(line, tok, n) != 0) return 0;
    char c = line[n];
    return (c == '\0' || c == '\n' || c == '\r' || c == ' ' || c == '\t');
}

/* --- Signal handling --- */
/**
 * SIGINT handler: marks the server loop as stopped.
 *
 * @param sig Signal number (unused).
 */
static void on_sigint(int sig) {
    (void)sig;
    g_server_running = 0;
}

/**
 * Check whether a player name exists in the active connection registry.
 *
 * @param n Player name.
 * @return 1 if present; 0 otherwise.
 */
int active_name_has(const char* n) {
    for (int i = 0; i < g_active_cnt; ++i)
        if (strncmp(g_active_names[i], n, MAX_NAME_LEN) == 0) return 1;
    return 0;
}
/**
 * Add a player name to the active connection registry.
 *
 * @param n Player name.
 * @return 0 on success; -1 if the registry is full.
 */
int active_name_add(const char* n) {
    if (g_active_cnt >= ACTIVE_MAX) return -1;
    strncpy(g_active_names[g_active_cnt++], n, MAX_NAME_LEN);
    g_active_names[g_active_cnt-1][MAX_NAME_LEN-1] = '\0';
    g_active_fds[g_active_cnt-1] = -1;
    g_active_back_req[g_active_cnt-1] = 0;
    g_active_tokens[g_active_cnt-1] = 0;
    return 0;
}
/**
 * Remove a player name from the active connection registry.
 *
 * @param n Player name.
 */
void active_name_remove(const char* n) {
    for (int i = 0; i < g_active_cnt; ++i) {
        if (strncmp(g_active_names[i], n, MAX_NAME_LEN) == 0) {
            // compact
            if (i != g_active_cnt - 1) {
                memcpy(g_active_names[i], g_active_names[g_active_cnt-1], MAX_NAME_LEN);
                g_active_fds[i] = g_active_fds[g_active_cnt-1];
                g_active_back_req[i] = g_active_back_req[g_active_cnt-1];
                g_active_tokens[i] = g_active_tokens[g_active_cnt-1];
            }
            g_active_back_req[g_active_cnt-1] = 0;
            g_active_tokens[g_active_cnt-1] = 0;
            g_active_cnt--;
            return;
        }
    }
}

/**
 * Find an index of a name in the active registry.
 *
 * @param n Player name.
 * @return Index in internal arrays, or -1 if not found.
 */
static int active_name_find(const char* n) {
    for (int i = 0; i < g_active_cnt; ++i) {
        if (strncmp(g_active_names[i], n, MAX_NAME_LEN) == 0) return i;
    }
    return -1;
}

/**
 * Update the socket fd for an active name and assign a new token.
 *
 * The token is used to prevent stale cleanup when the same name reconnects
 * on a different socket.
 *
 * @param n  Player name (must exist in registry).
 * @param fd Connected socket file descriptor.
 * @return New token value, or 0 if the name is not found.
 */
static unsigned long long active_name_set_fd(const char* n, int fd) {
    int i = active_name_find(n);
    if (i >= 0) {
        g_active_fds[i] = fd;
        g_active_tokens[i] = g_token_seq++;
        return g_active_tokens[i];
    }
    return 0;
}

/**
 * Remove an active name only if its current token matches the provided token.
 *
 * @param n     Player name.
 * @param token Token value previously returned by active_name_set_fd().
 */
static void active_name_remove_if_token(const char* n, unsigned long long token) {
    int i = active_name_find(n);
    if (i < 0) return;
    if (g_active_tokens[i] != token) return;
    active_name_remove(n);
}

/**
 * Mark a pending "back to lobby" request for an active name.
 *
 * @param n  Player name.
 * @param fd Optional socket fd for additional safety (pass -1 to ignore fd).
 */
void active_name_mark_back(const char* n, int fd) {
    if (!n || !*n) return;
    pthread_mutex_lock(&g_names_mtx);
    int i = active_name_find(n);
    if (i >= 0 && (fd < 0 || g_active_fds[i] == fd)) {
        g_active_back_req[i] = 1;
    }
    pthread_mutex_unlock(&g_names_mtx);
}

/**
 * Consume (clear) a pending "back to lobby" request.
 *
 * @param n  Player name.
 * @param fd Optional socket fd for additional safety (pass -1 to ignore fd).
 * @return 1 if a pending request was found and cleared; 0 otherwise.
 */
int active_name_take_back(const char* n, int fd) {
    if (!n || !*n) return 0;
    pthread_mutex_lock(&g_names_mtx);
    int i = active_name_find(n);
    int ok = (i >= 0 &&
              g_active_back_req[i] &&
              (fd < 0 || g_active_fds[i] == fd));
    if (ok) g_active_back_req[i] = 0;
    pthread_mutex_unlock(&g_names_mtx);
    return ok;
}

/**
 * Try to reattach a TCP connection to a player in a running lobby game.
 *
 * The reconnect is allowed only if:
 *   - the lobby index is valid,
 *   - the lobby is currently running,
 *   - a matching player record exists with fd == -1 (previously disconnected).
 *
 * @param lobby_index Zero-based lobby index.
 * @param name        Player name.
 * @param fd          New connected socket file descriptor.
 * @return 0 on success; -1 on failure.
 */
static int lobby_try_reconnect(int lobby_index, const char* name, int fd) {
    if (lobby_index < 0 || lobby_index >= g_lobby_count) return -1;
    if (!name || !*name) return -1;

    Lobby* L = &g_lobbies[lobby_index];
    pthread_mutex_lock(&L->mtx);

    int ok = -1;
    if (L->is_running) {
        for (int p = 0; p < LOBBY_SIZE; ++p) {
            Player* pl = &L->players[p];
            if (pl->connected &&
                pl->fd == -1 &&
                strncmp(pl->name, name, MAX_NAME_LEN) == 0) {
                pl->fd = fd;
                ok = 0;
                break;
            }
        }
    }

    pthread_mutex_unlock(&L->mtx);
    return ok;
}

/**
 * Parse a lobby selection line in the legacy format: "C45<name><lobby>\n".
 *
 * Note: for backward compatibility this parser treats ONLY the last digit as the lobby number.
 * This means lobbies above 9 are not representable in this legacy command.
 *
 * @param line        Full received line.
 * @param out_name    Output buffer for the extracted player name.
 * @param out_name_sz Size of @p out_name in bytes.
 * @param out_lobby   Output: parsed lobby number (1-based).
 *
 * @return 0 on success; negative value on parse/validation error.
 */
static int parse_name_lobby(const char* line, char* out_name, int out_name_sz, int* out_lobby) {
    if (!is_c45_prefix(line)) return -1;

    /* cut off “C45” and trailing \r\n and spaces */
    const char* s = line + 3;
    while (*s == ' ' || *s == '\t') s++;

    char tmp[READ_BUF];
    strncpy(tmp, s, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';

    // Trim trailing CR/LF and whitespace.
    for (int i = (int)strlen(tmp)-1; i >= 0 &&
             (tmp[i] == '\r' || tmp[i] == '\n' || tmp[i] == ' ' || tmp[i] == '\t'); --i) {
        tmp[i] = '\0';
    }
    if (tmp[0] == '\0') return -2;

    /* Legacy format: use ONLY the last digit as the lobby number. */
    int len = (int)strlen(tmp);
    int pos = len - 1;
    if (pos < 0) return -3;

    // The last non-whitespace character must be a digit.
    if (tmp[pos] < '0' || tmp[pos] > '9') return -4;  // no digit at the end

    int lobby = tmp[pos] - '0';
    tmp[pos] = '\0';          // cut the lobby digit
    pos--;

    if (lobby < 1 || lobby > g_lobby_count) return -5;

    /* Now tmp contains only the name (may still have trailing whitespace). */
    while (pos >= 0 && (tmp[pos] == ' ' || tmp[pos] == '\t')) {
        tmp[pos] = '\0';
        pos--;
    }
    if (pos < 0) return -6;

    for (int i = 0; tmp[i]; ++i) {
        if (isspace((unsigned char)tmp[i])) return -8; // protocol uses whitespace delimiters
    }
    if ((int)strlen(tmp) >= out_name_sz) return -7;

    strcpy(out_name, tmp);
    *out_lobby = lobby;
    return 0;
}

/**
 * Parse a client name line in the format: "C45<name>\n".
 *
 * @param line        Full received line.
 * @param out_name    Output buffer for the extracted name.
 * @param out_name_sz Size of @p out_name in bytes.
 * @return 0 on success; negative value on parse/validation error.
 */
static int parse_name_only(const char* line, char* out_name, int out_name_sz) {
    if (!is_c45_prefix(line)) return -1;

    const char* s = line + 3;
    while (*s == ' ' || *s == '\t') s++;

    char tmp[READ_BUF];
    strncpy(tmp, s, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (int i = (int)strlen(tmp) - 1; i >= 0 &&
             (tmp[i] == '\r' || tmp[i] == '\n' || tmp[i] == ' ' || tmp[i] == '\t'); --i) {
        tmp[i] = '\0';
    }
    if (tmp[0] == '\0') return -2;
    for (int i = 0; tmp[i]; ++i) {
        if (isspace((unsigned char)tmp[i])) return -4; // protocol uses whitespace delimiters
    }
    if ((int)strlen(tmp) >= out_name_sz) return -3;

    strcpy(out_name, tmp);
    return 0;
}

/**
 * Check whether a line is a "back to lobby" request for a specific name.
 *
 * Expected format:
 *   "C45<name>back\n"
 *
 * @param line          Full received line.
 * @param expected_name Player name to match.
 *
 * @return  1 Line matches a valid back request for @p expected_name.
 * @return  0 Line is not a back request.
 * @return -1 Line looks like a back request but for another name or invalid.
 */
static int is_back_request_for(const char* line, const char* expected_name) {
    if (!is_c45_prefix(line)) return 0;
    if (!expected_name || expected_name[0] == '\0') return 0;

    const char* s = line + 3;
    while (*s == ' ' || *s == '\t') s++;

    char tmp[READ_BUF];
    strncpy(tmp, s, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (int i = (int)strlen(tmp) - 1; i >= 0 &&
             (tmp[i] == '\r' || tmp[i] == '\n' || tmp[i] == ' ' || tmp[i] == '\t'); --i) {
        tmp[i] = '\0';
    }

    const char* suffix = "back";
    size_t len = strlen(tmp);
    size_t slen = strlen(suffix);
    if (len <= slen) return 0;
    if (strcmp(tmp + (len - slen), suffix) != 0) return 0;

    tmp[len - slen] = '\0';
    for (int i = (int)strlen(tmp) - 1; i >= 0 && (tmp[i] == ' ' || tmp[i] == '\t'); --i) {
        tmp[i] = '\0';
    }
    if (tmp[0] == '\0') return -1;

    return (strncmp(tmp, expected_name, MAX_NAME_LEN) == 0) ? 1 : -1;
}

/**
 * Busy-wait until a lobby changes its running state.
 *
 * @param lobby_index    Zero-based lobby index.
 * @param target_running Desired boolean state (0 or 1).
 */
static void wait_lobby_running_change(int lobby_index, int target_running) {
    for (;;) {
        pthread_mutex_lock(&g_lobbies[lobby_index].mtx);
        int running = g_lobbies[lobby_index].is_running;
        pthread_mutex_unlock(&g_lobbies[lobby_index].mtx);
        if (!!running == !!target_running) return;
        usleep(100000);
    }
}


/**
 * Per-client thread entry point.
 *
 * This function runs the complete client state machine:
 * handshake -> lobby selection -> waiting/game -> post-game.
 *
 * @param arg Socket fd passed as (void*)(intptr_t)fd.
 * @return NULL.
 */
static void* client_thread(void* arg) {
    int cfd = (int)(intptr_t)arg;
    printf("[NET] Client start (fd=%d)\n", cfd);

    client_fd_add(cfd);

    /* timeouts */
    struct timeval tv; tv.tv_sec = 120; tv.tv_usec = 0;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char line[READ_BUF];
    char name[MAX_NAME_LEN] = {0};
    int lobby_num = -1;
    unsigned long long my_token = 0;

    /* --- Handshake --- */
    int n;
    for (;;) {
        n = read_line(cfd, line, sizeof(line));
        if (n <= 0) {
            printf("[NET] Client fd=%d closed during handshake\n", cfd);
            client_fd_remove(cfd);
            close(cfd);
            return NULL;
        }
        if (!is_c45_prefix(line)) {
            printf("[PROTO] Wrong handshake from fd=%d: \"%s\" -> C45WRONG\n", cfd, line);
            write_all(cfd, "C45WRONG\n");
            client_fd_remove(cfd);
            close(cfd);
            return NULL;
        }

        // Allow keep-alive before a name is entered (client may keep the TCP connection open
        // while waiting on the "enter nickname" screen).
        if (is_token(line, "C45PING")) {
            (void)write_all(cfd, "C45PONG\n");
            continue;
        }
        if (is_token(line, "C45PONG")) continue;

        break; // real handshake line
    }

    // Reconnect path: first line is "C45RECONNECT <name> <lobby>\n"
    if (strncmp(line, "C45RECONNECT ", 13) == 0) {
        if (sscanf(line, "C45RECONNECT %63s %d", name, &lobby_num) != 2 ||
            lobby_num < 1 || lobby_num > g_lobby_count) {
            write_all(cfd, "C45WRONG RECONNECT\n");
            client_fd_remove(cfd);
            close(cfd);
            return NULL;
        }

        if (lobby_try_reconnect(lobby_num - 1, name, cfd) == 0) {
            pthread_mutex_lock(&g_names_mtx);
            if (!active_name_has(name)) {
                if (active_name_add(name) != 0) {
                    pthread_mutex_unlock(&g_names_mtx);
                    write_all(cfd, "C45WRONG RECONNECT\n");
                    client_fd_remove(cfd);
                    close(cfd);
                    return NULL;
                }
            }
            my_token = active_name_set_fd(name, cfd);
            pthread_mutex_unlock(&g_names_mtx);

            write_all(cfd, "C45RECONNECT_OK\n");
            printf("[NET] Reconnected '%s' to lobby #%d (fd=%d)\n", name, lobby_num, cfd);
            goto game_wait;
        }

        // If reconnection is impossible (game already finished) treat it as a normal login
        // and immediately send the lobby snapshot instead of disconnecting the client.
        if (lobby_name_exists(name)) {
            write_all(cfd, "C45WRONG NAME_TAKEN\n");
            client_fd_remove(cfd);
            close(cfd);
            return NULL;
        }

        pthread_mutex_lock(&g_names_mtx);
        if (!active_name_has(name)) {
            if (active_name_add(name) != 0) {
                pthread_mutex_unlock(&g_names_mtx);
                write_all(cfd, "C45WRONG\n");
                client_fd_remove(cfd);
                close(cfd);
                return NULL;
            }
        }
        my_token = active_name_set_fd(name, cfd);
        pthread_mutex_unlock(&g_names_mtx);

        if (write_all(cfd, "C45OK\n") < 0) {
            pthread_mutex_lock(&g_names_mtx);
            active_name_remove_if_token(name, my_token);
            pthread_mutex_unlock(&g_names_mtx);
            client_fd_remove(cfd);
            close(cfd);
            return NULL;
        }
        if (send_lobbies_snapshot(cfd) < 0) {
            printf("[ERR] Cannot send snapshot lobbies (fd=%d)\n", cfd);
            pthread_mutex_lock(&g_names_mtx);
            active_name_remove_if_token(name, my_token);
            pthread_mutex_unlock(&g_names_mtx);
            client_fd_remove(cfd);
            close(cfd);
            return NULL;
        }

        printf("[NET] Reconnect fallback -> lobby list for '%s' (fd=%d)\n", name, cfd);
        goto lobby_select;
    }

    if (parse_name_only(line, name, sizeof(name)) != 0) {
        printf("[PROTO] Bad name in handshake from fd=%d: \"%s\" -> C45WRONG\n", cfd, line);
        write_all(cfd, "C45WRONG\n");
        client_fd_remove(cfd);
        close(cfd);
        return NULL;
    }
    printf("[PROTO] Handshake OK '%s' from fd=%d\n", name, cfd);

    if (lobby_name_exists(name)) {
        write_all(cfd, "C45WRONG NAME_TAKEN\n");
        client_fd_remove(cfd);
        close(cfd);
        return NULL;
    }

    // Reserve name for the whole lifetime of this connection
    pthread_mutex_lock(&g_names_mtx);
    int taken = active_name_has(name);
    if (!taken) {
        taken = (active_name_add(name) != 0);
        if (!taken) my_token = active_name_set_fd(name, cfd);
    }
    pthread_mutex_unlock(&g_names_mtx);
    if (taken) {
        write_all(cfd, "C45WRONG NAME_TAKEN\n");
        client_fd_remove(cfd);
        close(cfd);
        return NULL;
    }

    // Acknowledge handshake for the Java client (its first OK)
    if (write_all(cfd, "C45OK\n") < 0) {
        pthread_mutex_lock(&g_names_mtx);
        active_name_remove_if_token(name, my_token);
        pthread_mutex_unlock(&g_names_mtx);
        client_fd_remove(cfd);
        close(cfd);
        return NULL;
    }

    /* --- send a screenshot of the lobby --- */
    if (send_lobbies_snapshot(cfd) < 0) {
        printf("[ERR] Cannot send snapshot lobbies (fd=%d)\n", cfd);
        pthread_mutex_lock(&g_names_mtx);
        active_name_remove_if_token(name, my_token);
        pthread_mutex_unlock(&g_names_mtx);
        client_fd_remove(cfd);
        close(cfd);
        return NULL;
    }

lobby_select:
    for (;;) {
        /* --- waiting for player selection: C45<name><lobby>\n --- */
        lobby_num = -1;
        for (;;) {
            n = read_line(cfd, line, sizeof(line));
            if (n <= 0) {
                printf("[NET] Client fd=%d closed before lobby choise\n", cfd);
                goto disconnect;
            }
            // printf("[PROTO] Take: \"%s\" (fd=%d)\n", line, cfd);

            // Client keep-alive (bidirectional): reply and continue waiting for real commands.
            if (is_token(line, "C45PING")) {
                (void)write_all(cfd, "C45PONG\n");
                continue;
            }
            if (is_token(line, "C45PONG")) continue;
            
            int br = is_back_request_for(line, name);
            if (br == 1) {
                // Allow requesting the snapshot at any time outside the game loop
                if (send_lobbies_snapshot(cfd) < 0) goto disconnect;
                continue;
            } else if (br < 0) {
                write_all(cfd, "C45WRONG\n");
                goto disconnect; // invalid back request
            }

            char join_name[MAX_NAME_LEN];
            if (parse_name_lobby(line, join_name, sizeof(join_name), &lobby_num) != 0) {
                printf("[PROTO] Wrong format of choise -> C45WRONG (fd=%d)\n", cfd);
                write_all(cfd, "C45WRONG\n");
                goto disconnect;
            }
            if (strncmp(join_name, name, MAX_NAME_LEN) != 0) {
                printf("[PROTO] Join name mismatch '%s' != '%s' (fd=%d)\n", join_name, name, cfd);
                write_all(cfd, "C45WRONG\n");
                goto disconnect;
            }
            break;
        }

        printf("[USER] Player '%s' ask for lobby #%d (fd=%d)\n", name, lobby_num, cfd);

        /* --- Attempting to add to the selected lobby --- */
        if (lobby_try_add_player(lobby_num - 1, name) != 0) {
            write_all(cfd, "C45WRONG\n");
            printf("[LOBBY] Cannot take from '%s' — Lobby #%d status full (fd=%d)\n", name, lobby_num, cfd);
            // stay connected and allow choosing another lobby
            continue;
        }
        lobby_attach_fd(lobby_num - 1, name, cfd);

        // Join OK for the Java client (its subsequent OKs)
        if (write_all(cfd, "C45OK\n") < 0) {
            printf("[ERR] Cannot send C45OK after adding (fd=%d)\n", cfd);
            lobby_remove_player_by_name(name);
            goto disconnect;
        }

        printf("[PROTO] -> C45OK '%s' in Lobby #%d (fd=%d)\n", name, lobby_num, cfd);
        start_game_if_ready(lobby_num - 1);

	        printf("[WAIT] '%s' Waiting for player in lobby #%d (fd=%d)\n", name, lobby_num, cfd);

	        // wait until the game actually starts (or client cancels/disconnects)
	        time_t last_waiting_sent = 0;
	        for (;;) {
	            pthread_mutex_lock(&g_lobbies[lobby_num - 1].mtx);
	            int running = g_lobbies[lobby_num - 1].is_running;
	            pthread_mutex_unlock(&g_lobbies[lobby_num - 1].mtx);
	            if (running) break;

	            time_t now = time(NULL);
	            if (now - last_waiting_sent >= WAITING_INTERVAL_SEC) {
	                if (write_all(cfd, "C45WAITING\n") < 0) {
	                    printf("[WAIT] write C45WAITING failed (fd=%d)\n", cfd);
	                    lobby_remove_player_by_name(name);
	                    goto disconnect;
	                }
	                last_waiting_sent = now;
	            }

	            struct pollfd pfd = { .fd = cfd, .events = POLLIN | POLLHUP | POLLERR };
	            int pr = poll(&pfd, 1, 1000);
	            if (pr == 0) continue;
	            if (pr < 0) {
	                if (errno == EINTR) continue;
	                printf("[WAIT] poll failed while waiting (fd=%d)\n", cfd);
	                lobby_remove_player_by_name(name);
	                goto disconnect;
	            }
	            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
	                printf("[WAIT] '%s' disconnected while waiting (fd=%d)\n", name, cfd);
	                lobby_remove_player_by_name(name);
	                goto disconnect;
	            }
	            if (!(pfd.revents & POLLIN)) continue;

	            // Re-check running before consuming any input to avoid stealing game traffic.
	            pthread_mutex_lock(&g_lobbies[lobby_num - 1].mtx);
	            running = g_lobbies[lobby_num - 1].is_running;
	            pthread_mutex_unlock(&g_lobbies[lobby_num - 1].mtx);
	            if (running) break;

	            char peekbuf[READ_BUF];
	            ssize_t rr = recv(cfd, peekbuf, sizeof(peekbuf) - 1, MSG_PEEK | MSG_DONTWAIT);
	            if (rr == 0) {
	                printf("[WAIT] '%s' disconnected while waiting (fd=%d)\n", name, cfd);
	                lobby_remove_player_by_name(name);
	                goto disconnect;
	            }
	            if (rr < 0) {
	                if (errno == EINTR) continue;
	                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
	                printf("[WAIT] recv peek failed while waiting (fd=%d)\n", cfd);
	                lobby_remove_player_by_name(name);
	                goto disconnect;
	            }
	            peekbuf[rr] = '\0';
	            if (!strchr(peekbuf, '\n')) continue; // wait for a complete line

		            int r = read_line(cfd, line, sizeof(line));
		            if (r <= 0) {
		                printf("[WAIT] '%s' disconnected while waiting (fd=%d)\n", name, cfd);
		                lobby_remove_player_by_name(name);
		                goto disconnect;
		            }

		            if (is_token(line, "C45PING")) {
		                (void)write_all(cfd, "C45PONG\n");
		                continue;
		            }
		            if (is_token(line, "C45PONG")) continue;
		            if (strncmp(line, "C45YES", 6) == 0) continue;

		            int br = is_back_request_for(line, name);
		            if (br == 1) {
	                // Cancel waiting: remove from lobby and return to lobby selection
	                lobby_remove_player_by_name(name);
	                if (send_lobbies_snapshot(cfd) < 0) goto disconnect;
	                goto next_round;
	            }
	            // Any other line while waiting is a protocol error.
	            write_all(cfd, "C45WRONG\n");
	            lobby_remove_player_by_name(name);
	            goto disconnect;
	        }

        printf("[GAME] '%s' Game started in lobby #%d (fd=%d)\n", name, lobby_num, cfd);
game_wait:
        wait_lobby_running_change(lobby_num - 1, 0); // wait game end
        // Ensure the player has been removed from the lobby by the game thread
        while (lobby_name_exists(name)) usleep(10000);

        printf("[GAME] '%s' Game finished, waiting for '%sback' (fd=%d)\n", name, name, cfd);
        if (active_name_take_back(name, cfd)) {
            if (send_lobbies_snapshot(cfd) < 0) goto disconnect;
            goto next_round;
        }
	        for (;;) {
	            n = read_line(cfd, line, sizeof(line));
	            if (n <= 0) goto disconnect;
	            if (is_token(line, "C45PING")) {
	                (void)write_all(cfd, "C45PONG\n");
	                continue;
	            }
	            if (is_token(line, "C45PONG")) continue;
	            if (strncmp(line, "C45YES", 6) == 0) continue;
	            int br = is_back_request_for(line, name);
	            if (br == 1) break;
            // Any other line after game end is a protocol error.
            write_all(cfd, "C45WRONG\n");
            goto disconnect;
        }

        if (send_lobbies_snapshot(cfd) < 0) goto disconnect;

next_round:
        continue;
    }

disconnect:
    pthread_mutex_lock(&g_names_mtx);
    active_name_remove_if_token(name, my_token);
    pthread_mutex_unlock(&g_names_mtx);
    client_fd_remove(cfd);
    close(cfd);
    return NULL;
}

/**
 * Start the TCP server accept loop and spawn a thread per client.
 *
 * The loop runs until @p g_server_running becomes 0 (SIGINT).
 *
 * @param bind_ip Bind address (NULL/"0.0.0.0" binds on all interfaces; "localhost" uses loopback).
 * @param port    TCP port to listen on.
 * @return 0 on normal shutdown; non-zero on fatal error.
 */
int run_server(const char* bind_ip, int port) {
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    
    // Bind IP
    if (!bind_ip || strcmp(bind_ip, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (strcmp(bind_ip, "localhost") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
            fprintf(stderr, "Invalid bind IP: %s\n", bind_ip);
            close(srv);
            return 1;
        }
    }


    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv); return 1;
    }
    if (listen(srv, 64) < 0) {
        perror("listen"); close(srv); return 1;
    }

    int ret = 0;
    const char* stop_reason = NULL;
    time_t last_ip_check = 0;

    printf("Server listening on %s:%d\n", bind_ip, port);
    while (g_server_running) {
        time_t now = time(NULL);
        if (now - last_ip_check >= 2) {
            last_ip_check = now;
            if (!is_bind_ip_available(bind_ip)) {
                fprintf(stderr,
                        "[NET] Bind IP %s is no longer available; stopping the server.\n",
                        bind_ip);
                stop_reason = "NETWORK_LOST";
                g_server_running = 0;
                break;
            }
        }

        struct pollfd spfd = { .fd = srv, .events = POLLIN | POLLERR | POLLHUP };
        int spr = poll(&spfd, 1, 1000);
        if (spr == 0) continue;
        if (spr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            ret = 1;
            stop_reason = "LISTEN_ERROR";
            break;
        }
        if (spfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "[NET] Listen socket error (revents=%d)\n", spfd.revents);
            ret = 1;
            stop_reason = "LISTEN_ERROR";
            break;
        }
        if (!(spfd.revents & POLLIN)) continue;

        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int cfd = accept(srv, (struct sockaddr*)&cli, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            ret = 1;
            stop_reason = "ACCEPT_ERROR";
            break;
        }

        printf("[NET] Connecting %s:%d (fd=%d)\n",
               inet_ntoa(cli.sin_addr), ntohs(cli.sin_port), cfd);

        pthread_t th;
        pthread_create(&th, NULL, client_thread, (void*)(intptr_t)cfd);
        pthread_detach(th);
    }

    if (!stop_reason) stop_reason = "SIGINT";
    server_notify_and_disconnect_all(stop_reason);
    close(srv);
    printf("Server stopped\n");
    return ret;
}
