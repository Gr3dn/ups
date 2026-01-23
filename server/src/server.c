#define _GNU_SOURCE
#include "server.h"
#include "protocol.h"
#include "game.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
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
#define WAITING_INTERVAL_SEC 5
// Reserving names among all active connections (until disconnect)
static pthread_mutex_t g_names_mtx = PTHREAD_MUTEX_INITIALIZER;
static char g_active_names[ACTIVE_MAX][MAX_NAME_LEN];
static int  g_active_fds[ACTIVE_MAX];
static int  g_active_back_req[ACTIVE_MAX];
static unsigned long long g_active_tokens[ACTIVE_MAX];
static unsigned long long g_token_seq = 1;
static int  g_active_cnt = 0;

/* --- Help --- */
static void on_sigint(int sig) {
    (void)sig;
    g_server_running = 0;
}

int active_name_has(const char* n) {
    for (int i = 0; i < g_active_cnt; ++i)
        if (strncmp(g_active_names[i], n, MAX_NAME_LEN) == 0) return 1;
    return 0;
}
int active_name_add(const char* n) {
    if (g_active_cnt >= ACTIVE_MAX) return -1;
    strncpy(g_active_names[g_active_cnt++], n, MAX_NAME_LEN);
    g_active_names[g_active_cnt-1][MAX_NAME_LEN-1] = '\0';
    g_active_fds[g_active_cnt-1] = -1;
    g_active_back_req[g_active_cnt-1] = 0;
    g_active_tokens[g_active_cnt-1] = 0;
    return 0;
}
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

static int active_name_find(const char* n) {
    for (int i = 0; i < g_active_cnt; ++i) {
        if (strncmp(g_active_names[i], n, MAX_NAME_LEN) == 0) return i;
    }
    return -1;
}

static unsigned long long active_name_set_fd(const char* n, int fd) {
    int i = active_name_find(n);
    if (i >= 0) {
        g_active_fds[i] = fd;
        g_active_tokens[i] = g_token_seq++;
        return g_active_tokens[i];
    }
    return 0;
}

static void active_name_remove_if_token(const char* n, unsigned long long token) {
    int i = active_name_find(n);
    if (i < 0) return;
    if (g_active_tokens[i] != token) return;
    active_name_remove(n);
}

void active_name_mark_back(const char* n, int fd) {
    if (!n || !*n) return;
    pthread_mutex_lock(&g_names_mtx);
    int i = active_name_find(n);
    if (i >= 0 && (fd < 0 || g_active_fds[i] == fd)) {
        g_active_back_req[i] = 1;
    }
    pthread_mutex_unlock(&g_names_mtx);
}

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


// /* Parsing the selection string: “C45<name><lobby>\n”
//    Return: 0=OK (out_name and out_lobby are filled), -1=format/range error. */
// static int parse_name_lobby(const char* line, char* out_name, int out_name_sz, int* out_lobby) {
//     if (!is_c45_prefix(line)) return -1;

//     /* cut off “C45” and trailing \r\n and spaces */
//     const char* s = line + 3;
//     while (*s == ' ' || *s == '\t') s++;

//     char tmp[READ_BUF];
//     strncpy(tmp, s, sizeof(tmp)-1);
//     tmp[sizeof(tmp)-1] = '\0';
//     for (int i = (int)strlen(tmp)-1; i >=0 && (tmp[i]=='\r'||tmp[i]=='\n'||tmp[i]==' '||tmp[i]=='\t'); --i) {
//         tmp[i] = '\0';
//     }
//     if (tmp[0] == '\0') return -2;

//     /* remove the lobby number from the end */
//     int len = (int)strlen(tmp);
//     int pos = len - 1;
//     if (pos < 0) return -3;

//     /* collect lobby numbers in reverse order */
//     int lobby = 0;
//     int base = 1;
//     int digits = 0;
//     while (pos >= 0 && tmp[pos] >= '0' && tmp[pos] <= '9') {
//         lobby = (tmp[pos] - '0') * base + lobby;
//         base *= 10;
//         digits++;
//         pos--;
//     }
//     if (digits == 0) return -4; /* without a number */
//     printf("%d", lobby);
//     if (lobby < 1 || lobby > LOBBY_COUNT) return -5;

//     /* name */
//     while (pos >= 0 && (tmp[pos] == ' ' || tmp[pos] == '\t')) pos--;
//     tmp[pos+1] = '\0'; /* cutting spaces */
//     if (pos+1 <= 0) return -6;

//     if ((int)strlen(tmp) >= out_name_sz) return -7;
//     strcpy(out_name, tmp);
//     *out_lobby = lobby;
//     return 0;
// }

/* Parsing the selection string: “C45<name><lobby>\n”
   Return: 0=OK (out_name and out_lobby are filled), -1=format/range error. */
static int parse_name_lobby(const char* line, char* out_name, int out_name_sz, int* out_lobby) {
    if (!is_c45_prefix(line)) return -1;

    /* cut off “C45” and trailing \r\n and spaces */
    const char* s = line + 3;
    while (*s == ' ' || *s == '\t') s++;

    char tmp[READ_BUF];
    strncpy(tmp, s, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';

    // убираем \r \n и пробелы с конца
    for (int i = (int)strlen(tmp)-1; i >= 0 &&
             (tmp[i] == '\r' || tmp[i] == '\n' || tmp[i] == ' ' || tmp[i] == '\t'); --i) {
        tmp[i] = '\0';
    }
    if (tmp[0] == '\0') return -2;

    /* берем ТОЛЬКО последнюю цифру как номер лобби */
    int len = (int)strlen(tmp);
    int pos = len - 1;
    if (pos < 0) return -3;

    // последняя значащая позиция должна быть цифрой
    if (tmp[pos] < '0' || tmp[pos] > '9') return -4;  // нет цифры в конце

    int lobby = tmp[pos] - '0';
    tmp[pos] = '\0';          // отрезаем цифру лобби
    pos--;

    if (lobby < 1 || lobby > g_lobby_count) return -5;

    /* теперь в tmp осталось только имя (с возможными пробелами в конце) */
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

/* Parsing a client name line: "C45<name>\n"
   Return: 0=OK, <0=error. */
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

/* Checks "C45<name>back\n" for a specific expected name.
   Return: 1=match, 0=not a back request, -1=back request with other name/bad. */
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

static void wait_lobby_running_change(int lobby_index, int target_running) {
    for (;;) {
        pthread_mutex_lock(&g_lobbies[lobby_index].mtx);
        int running = g_lobbies[lobby_index].is_running;
        pthread_mutex_unlock(&g_lobbies[lobby_index].mtx);
        if (!!running == !!target_running) return;
        usleep(100000);
    }
}


/* Client stream */
static void* client_thread(void* arg) {
    int cfd = (int)(intptr_t)arg;
    printf("[NET] Client start (fd=%d)\n", cfd);

    /* timeouts */
    struct timeval tv; tv.tv_sec = 120; tv.tv_usec = 0;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char line[READ_BUF];
    char name[MAX_NAME_LEN] = {0};
    int lobby_num = -1;
    unsigned long long my_token = 0;

    /* --- Handshake --- */
    int n = read_line(cfd, line, sizeof(line));
    if (n <= 0 || !is_c45_prefix(line)) {
        printf("[PROTO] Wrong handshake from fd=%d: \"%s\" -> C45WRONG\n", cfd, line);
        write_all(cfd, "C45WRONG\n");
        close(cfd);
        return NULL;
    }

    // Reconnect path: first line is "C45RECONNECT <name> <lobby>\n"
    if (strncmp(line, "C45RECONNECT ", 13) == 0) {
        if (sscanf(line, "C45RECONNECT %63s %d", name, &lobby_num) != 2 ||
            lobby_num < 1 || lobby_num > g_lobby_count) {
            write_all(cfd, "C45WRONG RECONNECT\n");
            close(cfd);
            return NULL;
        }

        if (lobby_try_reconnect(lobby_num - 1, name, cfd) == 0) {
            pthread_mutex_lock(&g_names_mtx);
            if (!active_name_has(name)) {
                if (active_name_add(name) != 0) {
                    pthread_mutex_unlock(&g_names_mtx);
                    write_all(cfd, "C45WRONG RECONNECT\n");
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
            close(cfd);
            return NULL;
        }

        pthread_mutex_lock(&g_names_mtx);
        if (!active_name_has(name)) {
            if (active_name_add(name) != 0) {
                pthread_mutex_unlock(&g_names_mtx);
                write_all(cfd, "C45WRONG\n");
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
            close(cfd);
            return NULL;
        }
        if (send_lobbies_snapshot(cfd) < 0) {
            printf("[ERR] Cannot send snapshot lobbies (fd=%d)\n", cfd);
            pthread_mutex_lock(&g_names_mtx);
            active_name_remove_if_token(name, my_token);
            pthread_mutex_unlock(&g_names_mtx);
            close(cfd);
            return NULL;
        }

        printf("[NET] Reconnect fallback -> lobby list for '%s' (fd=%d)\n", name, cfd);
        goto lobby_select;
    }

    if (parse_name_only(line, name, sizeof(name)) != 0) {
        printf("[PROTO] Bad name in handshake from fd=%d: \"%s\" -> C45WRONG\n", cfd, line);
        write_all(cfd, "C45WRONG\n");
        close(cfd);
        return NULL;
    }
    printf("[PROTO] Handshake OK '%s' from fd=%d\n", name, cfd);

    if (lobby_name_exists(name)) {
        write_all(cfd, "C45WRONG NAME_TAKEN\n");
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
        close(cfd);
        return NULL;
    }

    // Acknowledge handshake for the Java client (its first OK)
    if (write_all(cfd, "C45OK\n") < 0) {
        pthread_mutex_lock(&g_names_mtx);
        active_name_remove_if_token(name, my_token);
        pthread_mutex_unlock(&g_names_mtx);
        close(cfd);
        return NULL;
    }

    /* --- send a screenshot of the lobby --- */
    if (send_lobbies_snapshot(cfd) < 0) {
        printf("[ERR] Cannot send snapshot lobbies (fd=%d)\n", cfd);
        pthread_mutex_lock(&g_names_mtx);
        active_name_remove_if_token(name, my_token);
        pthread_mutex_unlock(&g_names_mtx);
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
            printf("[PROTO] Take: \"%s\" (fd=%d)\n", line, cfd);

            // Client keep-alive (bidirectional): reply and continue waiting for real commands.
            if (strncmp(line, "C45PING", 7) == 0) {
                (void)write_all(cfd, "C45PONG\n");
                continue;
            }
            if (strncmp(line, "C45PONG", 7) == 0) continue;

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

		            if (strncmp(line, "C45PING", 7) == 0) {
		                (void)write_all(cfd, "C45PONG\n");
		                continue;
		            }
		            if (strncmp(line, "C45PONG", 7) == 0) continue;
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
	            if (strncmp(line, "C45PING", 7) == 0) {
	                (void)write_all(cfd, "C45PONG\n");
	                continue;
	            }
	            if (strncmp(line, "C45PONG", 7) == 0) continue;
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
    close(cfd);
    return NULL;
}

int run_server(const char* bind_ip, int port) {
    signal(SIGINT, on_sigint);
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

    printf("Server listening on %s:%d\n", bind_ip, port);
    while (g_server_running) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int cfd = accept(srv, (struct sockaddr*)&cli, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        printf("[NET] Connecting %s:%d (fd=%d)\n",
               inet_ntoa(cli.sin_addr), ntohs(cli.sin_port), cfd);

        pthread_t th;
        pthread_create(&th, NULL, client_thread, (void*)(intptr_t)cfd);
        pthread_detach(th);
    }

    close(srv);
    printf("Server stopped\n");
    return 0;
}
