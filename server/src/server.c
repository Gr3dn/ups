#define _GNU_SOURCE
#include "server.h"
#include "protocol.h"
#include "game.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define ACTIVE_MAX 256
// Reserving names among those connected but not yet in the lobby
static pthread_mutex_t g_names_mtx = PTHREAD_MUTEX_INITIALIZER;
static char g_active_names[ACTIVE_MAX][MAX_NAME_LEN];
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
    return 0;
}
void active_name_remove(const char* n) {
    for (int i = 0; i < g_active_cnt; ++i) {
        if (strncmp(g_active_names[i], n, MAX_NAME_LEN) == 0) {
            // compact
            if (i != g_active_cnt - 1)
                memcpy(g_active_names[i], g_active_names[g_active_cnt-1], MAX_NAME_LEN);
            g_active_cnt--;
            return;
        }
    }
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

    if ((int)strlen(tmp) >= out_name_sz) return -7;

    strcpy(out_name, tmp);
    *out_lobby = lobby;
    return 0;
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

    /* --- Handshake --- */
    int n = read_line(cfd, line, sizeof(line));
    printf("n=%d, is_c45=%d\n",n,is_c45_prefix(line));
    if (n <= 0 || !is_c45_prefix(line)) {
        printf("[PROTO] Wrong handshake from fd=%d: \"%s\" -> C45WRONG\n", cfd, line);
        write_all(cfd, "C45WRONG\n");
        close(cfd);
        return NULL;
    }
    printf("[PROTO] Handshake OK from fd=%d: \"%s\"", cfd, line);

    /* --- send a screenshot of the lobby --- */
    if (send_lobbies_snapshot(cfd) < 0) {
        printf("[ERR] Cannot send snapshot lobbies (fd=%d)\n", cfd);
        close(cfd);
        return NULL;
    }

    /* --- waiting for player selection: C45<name><lobby>\n --- */
    n = read_line(cfd, line, sizeof(line));
    if (n <= 0) {
        printf("[NET] Client fd=%d closed before choise lobby\n", cfd);
        close(cfd);
        return NULL;
    }
    printf("[PROTO] Take: \"%s\" (fd=%d)\n", line, cfd);

    char name[MAX_NAME_LEN];
    int lobby_num = -1;
    if (parse_name_lobby(line, name, sizeof(name), &lobby_num) != 0) {
        printf("Error=%d", parse_name_lobby(line, name, sizeof(name), &lobby_num));
        printf("[PROTO] Wrong format of choise -> C45WRONG (fd=%d)\n", cfd);
        write_all(cfd, "C45WRONG\n");
        close(cfd);
        return NULL;
    }

    printf("[USER] Player '%s' ask for lobby #%d (fd=%d)\n", name, lobby_num, cfd);
//========================================Name Control========================================
    // Is the name already reserved by another connection?
    pthread_mutex_lock(&g_names_mtx);
    int taken = active_name_has(name);
    pthread_mutex_unlock(&g_names_mtx);
    if (taken) {
        write_all(cfd, "C45WRONG NAME_TAKEN\n");
        
        close(cfd);
        printf("prvni vychod");
        return NULL;
    } else {
        pthread_mutex_lock(&g_names_mtx);
        active_name_add(name);
        pthread_mutex_unlock(&g_names_mtx);
    }
    


    if (write_all(cfd, "C45OK\n") < 0) {
        printf("[ERR] Cannot send C45OK to (fd=%d)\n", cfd);
        pthread_mutex_lock(&g_names_mtx);
        active_name_remove(name);
        pthread_mutex_unlock(&g_names_mtx);
        close(cfd);
        return NULL;
    }
//========================================End========================================
    /* --- Attempting to add to the selected lobby --- */
    if (lobby_try_add_player(lobby_num - 1, name) != 0) {
        write_all(cfd, "C45WRONG\n");
        printf("[LOBBY] Cannot take from '%s' — Lobby #%d status full (fd=%d)\n", name, lobby_num, cfd);

        pthread_mutex_lock(&g_names_mtx);
        active_name_remove(name);
        pthread_mutex_unlock(&g_names_mtx);
        close(cfd);
        return NULL;
    }

    if (write_all(cfd, "C45OK\n") < 0) {
        printf("[ERR] Cannot send C45OK after adding (fd=%d)\n", cfd);
        lobby_remove_player_by_name(name);

        pthread_mutex_lock(&g_names_mtx);
        active_name_remove(name);
        pthread_mutex_unlock(&g_names_mtx);
        close(cfd);
        return NULL;
    }

    printf("[PROTO] -> C45OK '%s' in Lobby #%d (fd=%d)\n", name, lobby_num, cfd);

    lobby_attach_fd(lobby_num - 1, name, cfd);

    start_game_if_ready(lobby_num - 1);

    printf("[WAIT] '%s' Waiting for player in lobby #%d (fd=%d)\n", name, lobby_num, cfd);
    int no_reply = 0;
    const int NOREPLY_KILL = 5;
    for (;;) {
        pthread_mutex_lock(&g_lobbies[lobby_num - 1].mtx);
        int cnt = g_lobbies[lobby_num - 1].player_count;
        int running = g_lobbies[lobby_num - 1].is_running;
        pthread_mutex_unlock(&g_lobbies[lobby_num - 1].mtx);

        if (running || cnt >= LOBBY_SIZE) {
            printf("[WAIT] Lobby #%d is full (%d/%d) — starting a game\n",
                   lobby_num, cnt, LOBBY_SIZE);
            break;
        }

        // Is the customer still on the stream?
        if (write_all(cfd, "C45WAITING\n") < 0) {
            printf("[WAIT] write C45WAITING failed (fd=%d)\n", cfd);
            lobby_remove_player_by_name(name);

            pthread_mutex_lock(&g_names_mtx);
            active_name_remove(name);
            pthread_mutex_unlock(&g_names_mtx);
            close(cfd);
            return NULL;
        }

        /* waiting "C45YES\n" */
        int r = read_line(cfd, line, sizeof(line));
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            if (++no_reply >= NOREPLY_KILL) {
                printf("[WAIT] '%s' Cannot answer on ping - disconnect\n", name);
                lobby_remove_player_by_name(name);
                pthread_mutex_lock(&g_names_mtx);
                active_name_remove(name);
                pthread_mutex_unlock(&g_names_mtx);
                close(cfd);
                return NULL;
            }
        }else {
            if (strncmp(line, "C45YES", 6) == 0) {
                no_reply = 0;
            }
            else{
                printf("[WAIT] '%s' Cannot answer on ping - disconnect\n", name);
                lobby_remove_player_by_name(name);
                pthread_mutex_lock(&g_names_mtx);
                active_name_remove(name);
                pthread_mutex_unlock(&g_names_mtx);
                close(cfd);
                return NULL;
            }
        }
        

    }
    printf("proverka");
    return NULL;
}

int run_server(const char* bind_ip, int port) {
    signal(SIGINT, SIG_IGN);

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
