// blackjack_server.c
// Простой сервер лобби для Blackjack (5 лобби по 2 игрока).
// Протокол подключения: клиент шлёт одну строку вида "C45name\n".
// Если строка начинается с "C45" и name не пустой — сервер отвечает "C45OK\n" и добавляет игрока в лобби.
// Иначе отвечает "C45WRONG\n" и разрывает соединение.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_NAME_LEN 64
#define READ_BUF 256
#define LOBBY_COUNT 5
#define LOBBY_SIZE 2
#define DECK_SIZE 52

// --------- Игровые структуры ----------
typedef enum { CLUBS, DIAMONDS, HEARTS, SPADES } Suit;
typedef struct { int rank; Suit suit; } Card; // rank: 1..13 (A..K)

typedef struct {
    Card cards[DECK_SIZE];
    int top; // индекс следующей карты к выдаче (0..52)
} Deck;

typedef struct {
    char name[MAX_NAME_LEN];
    Card hand[12];
    int hand_size;
    int connected; // 0/1
} Player;

typedef struct {
    Player players[LOBBY_SIZE];
    int player_count;
    Deck deck;
    pthread_mutex_t mtx;
} Lobby;

// --------- Глобалы ----------
static Lobby lobbies[LOBBY_COUNT];
static atomic_int server_running = 1;

// --------- Утилиты ----------
static void deck_init(Deck *d) {
    int idx = 0;
    for (int s = 0; s < 4; ++s) {
        for (int r = 1; r <= 13; ++r) {
            d->cards[idx].rank = r;
            d->cards[idx].suit = (Suit)s;
            idx++;
        }
    }
    d->top = 0;
}

static void deck_shuffle(Deck *d) {
    for (int i = DECK_SIZE - 1; i > 0; --i) {
        int j = rand() % (i + 1);
        Card tmp = d->cards[i];
        d->cards[i] = d->cards[j];
        d->cards[j] = tmp;
    }
    d->top = 0;
}

static Card deck_draw(Deck *d) {
    if (d->top >= DECK_SIZE) { // перетасовать заново при исчерпании
        deck_shuffle(d);
    }
    return d->cards[d->top++];
}

static void lobbies_init(void) {
    for (int i = 0; i < LOBBY_COUNT; ++i) {
        lobbies[i].player_count = 0;
        pthread_mutex_init(&lobbies[i].mtx, NULL);
        deck_init(&lobbies[i].deck);
        deck_shuffle(&lobbies[i].deck);
        for (int p = 0; p < LOBBY_SIZE; ++p) {
            lobbies[i].players[p].name[0] = '\0';
            lobbies[i].players[p].hand_size = 0;
            lobbies[i].players[p].connected = 0;
        }
    }
}

static int assign_player_to_lobby(const char *name) {
    for (int i = 0; i < LOBBY_COUNT; ++i) {
        pthread_mutex_lock(&lobbies[i].mtx);
        if (lobbies[i].player_count < LOBBY_SIZE) {
            // поместить на первое свободное место
            for (int p = 0; p < LOBBY_SIZE; ++p) {
                if (!lobbies[i].players[p].connected) {
                    Player *pl = &lobbies[i].players[p];
                    strncpy(pl->name, name, MAX_NAME_LEN - 1);
                    pl->name[MAX_NAME_LEN - 1] = '\0';
                    pl->hand_size = 0;
                    pl->connected = 1;

                    lobbies[i].player_count++;
                    // LOG
                    printf("[LOBBY] Игрок '%s' добавлен в лобби #%d (занято %d/%d)\n",
                           pl->name, i, lobbies[i].player_count, LOBBY_SIZE);
                    pthread_mutex_unlock(&lobbies[i].mtx);
                    return i; // номер лобби
                }
            }
        }
        pthread_mutex_unlock(&lobbies[i].mtx);
    }
    // LOG
    printf("[LOBBY] Нет свободных мест для игрока '%s'\n", name);
    return -1; // нет места
}

static void remove_player_from_lobby(const char *name) {
    for (int i = 0; i < LOBBY_COUNT; ++i) {
        pthread_mutex_lock(&lobbies[i].mtx);
        for (int p = 0; p < LOBBY_SIZE; ++p) {
            Player *pl = &lobbies[i].players[p];
            if (pl->connected && strncmp(pl->name, name, MAX_NAME_LEN) == 0) {
                pl->connected = 0;
                pl->name[0] = '\0';
                pl->hand_size = 0;
                lobbies[i].player_count--;
                // LOG
                printf("[LOBBY] Игрок '%s' удалён из лобби #%d (теперь %d/%d)\n",
                       name, i, lobbies[i].player_count, LOBBY_SIZE);
                pthread_mutex_unlock(&lobbies[i].mtx);
                return;
            }
        }
        pthread_mutex_unlock(&lobbies[i].mtx);
    }
}

// безопасная запись строки целиком
static int write_all(int fd, const char *s) {
    size_t n = strlen(s);
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, s + off, n - off, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static void *client_thread(void *arg) {
    int client_fd = (int)(intptr_t)arg;
    // LOG
    printf("[NET] Обработчик клиента запущен (fd=%d)\n", client_fd);

    char buf[READ_BUF];
    int pos = 0;

    // таймауты на чтение/запись
    struct timeval tv; tv.tv_sec = 10; tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // читаем до '\n' или переполнения буфера
    while (pos < READ_BUF - 1) {
        char c;
        ssize_t r = recv(client_fd, &c, 1, 0);
        if (r == 0) { // клиент закрыл соединение
            // LOG
            printf("[NET] Клиент fd=%d закрыл соединение до отправки строки\n", client_fd);
            close(client_fd);
            return NULL;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            printf("[ERR] recv(fd=%d) errno=%d (%s)\n", client_fd, errno, strerror(errno)); // LOG
            write_all(client_fd, "C45WRONG\n");
            printf("[PROTO] -> C45WRONG (fd=%d)\n", client_fd); // LOG
            close(client_fd);
            return NULL;
        }
        buf[pos++] = c;
        if (c == '\n') break;
    }
    buf[pos] = '\0';
    // LOG
    printf("[PROTO] Получено от fd=%d: \"%s\"\n", client_fd, buf);

    // проверка префикса "C45"
    if (pos < 4 || strncmp(buf, "C45", 3) != 0) {
        printf("[PROTO] Неверный префикс от fd=%d -> C45WRONG\n", client_fd); // LOG
        write_all(client_fd, "C45WRONG\n");
        close(client_fd);
        return NULL;
    }

    // извлекаем name (между "C45" и '\n'), обрезаем пробелы/переводы строк
    char name_raw[READ_BUF];
    strncpy(name_raw, buf + 3, sizeof(name_raw) - 1);
    name_raw[sizeof(name_raw) - 1] = '\0';

    // убрать завершающие \r\n и ведущие пробелы
    char *start = name_raw;
    while (*start == ' ' || *start == '\t') start++;
    for (int i = (int)strlen(start) - 1; i >= 0 && (start[i] == '\r' || start[i] == '\n' || start[i] == ' ' || start[i] == '\t'); --i) {
        start[i] = '\0';
    }

    if (*start == '\0') {
        printf("[PROTO] Пустое имя от fd=%d -> C45WRONG\n", client_fd); // LOG
        write_all(client_fd, "C45WRONG\n");
        close(client_fd);
        return NULL;
    }

    // ограничим длину имени
    char name[MAX_NAME_LEN];
    strncpy(name, start, MAX_NAME_LEN - 1);
    name[MAX_NAME_LEN - 1] = '\0';

    printf("[USER] Имя '%s' от fd=%d — пытаемся назначить в лобби\n", name, client_fd); // LOG

    // пытаемся назначить в лобби
    int lobby_id = assign_player_to_lobby(name);
    if (lobby_id < 0) {
        printf("[LOBBY] Мест нет для '%s' -> C45WRONG (fd=%d)\n", name, client_fd); // LOG
        write_all(client_fd, "C45WRONG\n"); // мест нет
        close(client_fd);
        return NULL;
    }

    // всё хорошо
    if (write_all(client_fd, "C45OK\n") < 0) {
        printf("[ERR] Не удалось отправить C45OK игроку '%s' (fd=%d), удаляем из лобби\n", name, client_fd); // LOG
        remove_player_from_lobby(name);
        close(client_fd);
        return NULL;
    }
    printf("[PROTO] -> C45OK игроку '%s' (лобби #%d, fd=%d)\n", name, lobby_id, client_fd); // LOG

    // По текущему ТЗ — просто подтверждаем и закрываем.
    close(client_fd);
    printf("[NET] Соединение закрыто (fd=%d) для игрока '%s'\n", client_fd, name); // LOG
    // remove_player_from_lobby(name); // если нужно убирать сразу — раскомментируй
    return NULL;
}

// корректное завершение
static void on_sigint(int sig) {
    (void)sig;
    server_running = 0;
}

int main(int argc, char **argv) {
    int port = 10000;

    signal(SIGINT, on_sigint);
    srand((unsigned)time(NULL));
    lobbies_init();

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv); return 1;
    }
    if (listen(srv, 64) < 0) {
        perror("listen"); close(srv); return 1;
    }

    printf("Server listening on port %d\n", port);

    while (server_running) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int cfd = accept(srv, (struct sockaddr*)&cli, &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        // LOG: новое подключение
        printf("[NET] Подключился клиент %s:%d (fd=%d)\n",
               inet_ntoa(cli.sin_addr), ntohs(cli.sin_port), cfd);

        pthread_t th;
        pthread_create(&th, NULL, client_thread, (void*)(intptr_t)cfd);
        pthread_detach(th);
    }

    close(srv);
    printf("Server stopped\n");
    return 0;
}
