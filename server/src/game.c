#include "game.h"
#include "protocol.h"
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

#define TURN_TIMEOUT_SEC       30
#define RECONNECT_TIMEOUT_SEC  30
#define PING_INTERVAL_SEC      5
#define PONG_TIMEOUT_SEC       10

Lobby* g_lobbies = NULL;
int    g_lobby_count = 5; // default value
atomic_int g_server_running = 1;
static void* lobby_game_thread(void* arg);

static int is_token(const char* line, const char* tok) {
    if (!line || !tok) return 0;
    size_t n = strlen(tok);
    if (strncmp(line, tok, n) != 0) return 0;
    char c = line[n];
    return (c == '\0' || c == '\n' || c == '\r' || c == ' ' || c == '\t');
}

// Server network config (definitions)
char g_server_ip[64] = "0.0.0.0";
int  g_server_port   = 10000;
/* --------- Deck ---------- */
void deck_init(Deck *d) {
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

void deck_shuffle(Deck *d) {
    for (int i = DECK_SIZE - 1; i > 0; --i) {
        int j = rand() % (i + 1);
        Card tmp = d->cards[i];
        d->cards[i] = d->cards[j];
        d->cards[j] = tmp;
    }
    d->top = 0;
}

Card deck_draw(Deck *d) {
    if (d->top >= DECK_SIZE) {
        deck_shuffle(d);
    }
    return d->cards[d->top++];
}

/* --------- Lobbies ---------- */
int load_config(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) {
        // Config missing -> keep defaults
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {0};
        char val[128] = {0};

        // key + value (value may be string or number)
        if (sscanf(line, " %63s %127s", key, val) != 2) continue;

        if (strcmp(key, "LOBBY_COUNT") == 0) {
            int v = atoi(val);
            if (v >= 1 && v <= 1000) g_lobby_count = v;
            else printf("Lobby_Count cannot be over 1000. Used default value 5\n\n");
        } else if (strcmp(key, "PORT") == 0) {
            int p = atoi(val);
            if (p >= 1 && p <= 65535) g_server_port = p;
        } else if (strcmp(key, "IP") == 0) {
            // Accept "0.0.0.0" to bind on all interfaces
            strncpy(g_server_ip, val, sizeof(g_server_ip) - 1);
            g_server_ip[sizeof(g_server_ip) - 1] = '\0';
        }
    }

    fclose(f);
    return 0;
}


int lobbies_init(void) {
    srand((unsigned)time(NULL));

    g_lobbies = (Lobby*)calloc((size_t)g_lobby_count, sizeof(Lobby));
    if (!g_lobbies) return -1;

    for (int i = 0; i < g_lobby_count; ++i) {
        g_lobbies[i].player_count = 0;
        g_lobbies[i].is_running   = 0;
        pthread_mutex_init(&g_lobbies[i].mtx, NULL);

        deck_init(&g_lobbies[i].deck);
        deck_shuffle(&g_lobbies[i].deck);

        for (int p = 0; p < LOBBY_SIZE; ++p) {
            Player* pl = &g_lobbies[i].players[p];
            pl->name[0] = '\0';
            pl->hand_size = 0;
            pl->connected = 0;
            pl->fd = -1;
            pl->stood = 0;
            pl->busted = 0;
        }
    }

    return 0;
}


int lobby_try_add_player(int lobby_index, const char* name) {
    if (lobby_index < 0 || lobby_index >= g_lobby_count) return -1;

    Lobby* L = &g_lobbies[lobby_index];
    pthread_mutex_lock(&L->mtx);

    if (L->player_count >= LOBBY_SIZE) {
        pthread_mutex_unlock(&L->mtx);
        printf("[LOBBY] Lobyy #%d is full (%d/%d)\n", lobby_index+1, L->player_count, LOBBY_SIZE);
        return -1;
    }

    /* place in the first available space */
    for (int p = 0; p < LOBBY_SIZE; ++p) {
        if (!L->players[p].connected) {
            Player *pl = &L->players[p];
            strncpy(pl->name, name, MAX_NAME_LEN - 1);
            pl->name[MAX_NAME_LEN - 1] = '\0';
            pl->hand_size = 0;
            pl->connected = 1;
            L->player_count++;
            printf("[LOBBY] '%s' add in lobby #%d (status %d/%d)\n",
                   pl->name, lobby_index+1, L->player_count, LOBBY_SIZE);
            pthread_mutex_unlock(&L->mtx);
            return 0;
        }
    }

    pthread_mutex_unlock(&L->mtx);
    return -1;
}

void lobby_remove_player_by_name(const char* name) {
    for (int i = 0; i < g_lobby_count; ++i) {
        Lobby* L = &g_lobbies[i];
        pthread_mutex_lock(&L->mtx);
        for (int p = 0; p < LOBBY_SIZE; ++p) {
            Player *pl = &L->players[p];
            if (pl->connected && strncmp(pl->name, name, MAX_NAME_LEN) == 0) {
                pl->connected = 0;
                pl->name[0] = '\0';
                pl->hand_size = 0;
                L->player_count--;
                printf("[LOBBY] Player '%s' removed from lobby #%d (status %d/%d)\n",
                       name, i+1, L->player_count, LOBBY_SIZE);
                pthread_mutex_unlock(&L->mtx);
                return;
            }
        }
        pthread_mutex_unlock(&L->mtx);
    }
}

int hand_value(const Card* hand, int n) {
    int sum = 0, aces = 0;
    for (int i = 0; i < n; ++i) {
        int r = hand[i].rank;           // 1..13 (A..K)
        if (r == 1) { aces++; sum += 11; }
        else if (r >= 10) sum += 10;
        else sum += r;
    }
    while (sum > 21 && aces > 0) { sum -= 10; aces--; }
    return sum;
}

void card_to_str(Card c, char out[3]) {
    static const char R[] = "A23456789TJQK";
    static const char S[] = "CDHS"; // CLUBS,DIAMONDS,HEARTS,SPADES
    out[0] = R[c.rank == 1 ? 0 : c.rank - 1];
    out[1] = S[(int)c.suit];
    out[2] = '\0';
}

int lobby_attach_fd(int li, const char* name, int fd) {
    if (li < 0 || li >= g_lobby_count) return -1;
    Lobby* L = &g_lobbies[li];
    pthread_mutex_lock(&L->mtx);
    for (int p = 0; p < LOBBY_SIZE; ++p) {
        Player* pl = &L->players[p];
        if (pl->connected && strncmp(pl->name, name, MAX_NAME_LEN) == 0) {
            pl-> fd = fd;                 
            pthread_mutex_unlock(&L->mtx);
            return 0;
        }
    }
    pthread_mutex_unlock(&L->mtx);
    return -1;
}

int start_game_if_ready(int li) {
    Lobby* L = &g_lobbies[li];
    pthread_mutex_lock(&L->mtx);
    if (!L->is_running && L->player_count == LOBBY_SIZE) {
        L->is_running = 1;
        pthread_t th;
        int* box = malloc(sizeof(int)); *box = li;
        pthread_create(&th, NULL, lobby_game_thread, box);
        pthread_detach(th);
    }
    pthread_mutex_unlock(&L->mtx);
    return 0;
}

static void player_disconnect_fd(Lobby* L, int player_index) {
    int old_fd = -1;
    pthread_mutex_lock(&L->mtx);
    old_fd = L->players[player_index].fd;
    L->players[player_index].fd = -1;
    pthread_mutex_unlock(&L->mtx);
    if (old_fd >= 0) close(old_fd);
}

static void send_hand_snapshot(int fd, const Card* hand, int hand_size) {
    if (fd < 0) return;
    if (hand_size < 2) return;

    char c1[3], c2[3], line[128];
    card_to_str(hand[0], c1);
    card_to_str(hand[1], c2);
    snprintf(line, sizeof(line), "C45DEAL %s %s\n", c1, c2);
    write_all(fd, line);

    for (int i = 2; i < hand_size; ++i) {
        char cs[3];
        card_to_str(hand[i], cs);
        snprintf(line, sizeof(line), "C45CARD %s\n", cs);
        write_all(fd, line);
    }
}

static int is_back_request_for_name(const char* line, const char* expected_name) {
    if (!line || !expected_name || expected_name[0] == '\0') return 0;
    if (strncmp(line, "C45", 3) != 0) return 0;

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

// Reads and handles any pending input from the non-active player during a turn.
// Returns: 0=ok, 1=protocol violation -> end game (forced_winner_idx is set),
//         -1=disconnect/error -> pause and wait for reconnect.
static int drain_nonactive_player_input(Lobby* L,
                                        int other_idx,
                                        int other_fd,
                                        int active_idx,
                                        int* forced_winner_idx,
                                        char* inbuf,
                                        size_t* inlen) {
    for (;;) {
        if (*inlen >= READ_BUF - 1) {
            player_disconnect_fd(L, other_idx);
            *forced_winner_idx = active_idx;
            return 1;
        }

        ssize_t r = recv(other_fd,
                         inbuf + *inlen,
                         (READ_BUF - 1) - *inlen,
                         MSG_DONTWAIT);
        if (r > 0) {
            *inlen += (size_t)r;
            inbuf[*inlen] = '\0';

            for (;;) {
                char* nl = memchr(inbuf, '\n', *inlen);
                if (!nl) break;

                size_t line_len = (size_t)(nl - inbuf + 1);
                char line[READ_BUF];
                if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
                memcpy(line, inbuf, line_len);
                line[line_len] = '\0';

                memmove(inbuf, inbuf + line_len, *inlen - line_len);
                *inlen -= line_len;

                if (is_token(line, "C45PONG")) continue;
                if (is_token(line, "C45PING")) {
                    (void)write_all(other_fd, "C45PONG\n");
                    continue;
                }
                if (is_token(line, "C45YES")) continue;

                // Allow quitting the game from the non-active side too.
                pthread_mutex_lock(&L->mtx);
                char other_name[MAX_NAME_LEN];
                strncpy(other_name, L->players[other_idx].name, sizeof(other_name) - 1);
                other_name[sizeof(other_name) - 1] = '\0';
                pthread_mutex_unlock(&L->mtx);
                if (is_back_request_for_name(line, other_name) == 1) {
                    active_name_mark_back(other_name, other_fd);
                    player_disconnect_fd(L, other_idx);
                    *forced_winner_idx = active_idx;
                    return 1;
                }

                // Out-of-turn commands (or any other garbage) are a protocol violation.
                player_disconnect_fd(L, other_idx);
                *forced_winner_idx = active_idx;
                return 1;
            }
            continue;
        }

        if (r == 0) return -1; // peer closed
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

// Waits up to RECONNECT_TIMEOUT_SEC for missing_idx to reconnect (fd != -1).
// Returns: 0=reconnected, 1=timeout, -1=other player disconnected.
static int wait_for_reconnect(Lobby* L, int missing_idx, int other_idx) {
    char missing_name[MAX_NAME_LEN];
    char other_name[MAX_NAME_LEN];
    pthread_mutex_lock(&L->mtx);
    strncpy(missing_name, L->players[missing_idx].name, sizeof(missing_name) - 1);
    missing_name[sizeof(missing_name) - 1] = '\0';
    strncpy(other_name, L->players[other_idx].name, sizeof(other_name) - 1);
    other_name[sizeof(other_name) - 1] = '\0';
    int other_fd = L->players[other_idx].fd;
    pthread_mutex_unlock(&L->mtx);

    char msg[128];
    snprintf(msg, sizeof(msg), "C45OPPDOWN %s %d\n", missing_name, RECONNECT_TIMEOUT_SEC);
    if (other_fd >= 0) write_all(other_fd, msg);

    time_t deadline = time(NULL) + RECONNECT_TIMEOUT_SEC;
    time_t last_ping = 0;
    time_t last_pong = time(NULL);

    for (;;) {
        time_t now = time(NULL);

        pthread_mutex_lock(&L->mtx);
        int missing_fd = L->players[missing_idx].fd;
        other_fd = L->players[other_idx].fd;
        pthread_mutex_unlock(&L->mtx);

        if (missing_fd >= 0) {
            Card hand[12];
            int hand_size = 0;

            pthread_mutex_lock(&L->mtx);
            hand_size = L->players[missing_idx].hand_size;
            if (hand_size > (int)(sizeof(hand) / sizeof(hand[0]))) hand_size = (int)(sizeof(hand) / sizeof(hand[0]));
            memcpy(hand, L->players[missing_idx].hand, (size_t)hand_size * sizeof(Card));
            pthread_mutex_unlock(&L->mtx);

            send_hand_snapshot(missing_fd, hand, hand_size);

            snprintf(msg, sizeof(msg), "C45OPPBACK %s\n", missing_name);
            if (other_fd >= 0) write_all(other_fd, msg);
            return 0;
        }

        if (now >= deadline) return 1;
        if (other_fd < 0) return -1;

        if (now - last_ping >= PING_INTERVAL_SEC) {
            if (write_all(other_fd, "C45PING\n") < 0) return -1;
            last_ping = now;
        }

        char buf[READ_BUF];
        int r = read_line_timeout(other_fd, buf, sizeof(buf), 1);
        if (r == -2) {
            // no data
        } else if (r <= 0) {
            return -1;
        } else if (is_token(buf, "C45PONG")) {
            last_pong = now;
        } else if (is_token(buf, "C45PING")) {
            (void)write_all(other_fd, "C45PONG\n");
            last_pong = now;
        } else if (is_back_request_for_name(buf, other_name) == 1) {
            active_name_mark_back(other_name, other_fd);
            return 1; // treat as disconnect-timeout -> end game early
        }

        if (now - last_pong > PONG_TIMEOUT_SEC) return -1;
    }
}

static void* lobby_game_thread(void* arg) {
    int li = *(int*)arg; free(arg);
    Lobby* L = &g_lobbies[li];
    int forced_winner_idx = -1;
    char nonactive_inbuf[LOBBY_SIZE][READ_BUF] = {{0}};
    size_t nonactive_inlen[LOBBY_SIZE] = {0};

    // preparing deck and hands
    pthread_mutex_lock(&L->mtx);
    deck_shuffle(&L->deck);
    for (int p = 0; p < LOBBY_SIZE; ++p) {
        L->players[p].hand_size = 0;
        L->players[p].stood = 0;    
        L->players[p].busted = 0;
    }
    // Step-by-step: player #1 (slot 0) goes first, then player #2 (slot 1).
    int p0 = 0;
    int p1 = 1;
    Player *A = &L->players[p0], *B = &L->players[p1];

    // deals 2 cards
    for (int p = 0; p < 2; ++p) {
        A->hand[A->hand_size++] = deck_draw(&L->deck);
        B->hand[B->hand_size++] = deck_draw(&L->deck);
    }
    char c1[3], c2[3], line[128];
    card_to_str(A->hand[0], c1); card_to_str(A->hand[1], c2);
    snprintf(line, sizeof(line), "C45DEAL %s %s\n", c1, c2); write_all(A->fd, line);
    card_to_str(B->hand[0], c1); card_to_str(B->hand[1], c2);
    snprintf(line, sizeof(line), "C45DEAL %s %s\n", c1, c2); write_all(B->fd, line);
    pthread_mutex_unlock(&L->mtx);

    int turn = 0; // player #1 starts
    for (;;) {
	turn_loop:
	        pthread_mutex_lock(&L->mtx);
	        if ((A->stood || A->busted) && (B->stood || B->busted)) {
	            pthread_mutex_unlock(&L->mtx);
	            break;
	        }
	        // Skip players that already ended their round (stood/busted).
	        if (L->players[turn].stood || L->players[turn].busted) {
	            turn = 1 - turn;
	            pthread_mutex_unlock(&L->mtx);
	            continue;
	        }
	        char turn_name[MAX_NAME_LEN];
	        strncpy(turn_name, L->players[turn].name, sizeof(turn_name) - 1);
	        turn_name[sizeof(turn_name) - 1] = '\0';
	        int fdA = A->fd;
	        int fdB = B->fd;
        pthread_mutex_unlock(&L->mtx);

        snprintf(line, sizeof(line), "C45TURN %s %d\n", turn_name, TURN_TIMEOUT_SEC);
        if (fdA >= 0 && write_all(fdA, line) < 0) goto pause_a;
        if (fdB >= 0 && write_all(fdB, line) < 0) goto pause_b;

        time_t turn_start = time(NULL);
        time_t last_ping = 0;
        time_t last_pong = time(NULL);

	        for (;;) {
	            time_t now = time(NULL);

	            pthread_mutex_lock(&L->mtx);
	            int pfd = L->players[turn].fd;
	            int other_idx = 1 - turn;
	            int other_fd = L->players[other_idx].fd;
	            pthread_mutex_unlock(&L->mtx);

	            if (pfd < 0) goto pause_turn;

		            // If the other player disconnects during this turn, pause immediately and wait for reconnect.
		            if (other_fd >= 0) {
		                struct pollfd op = { .fd = other_fd, .events = POLLIN | POLLHUP | POLLERR };
		                int pr = poll(&op, 1, 0);
		                if (pr > 0) {
		                    if (op.revents & (POLLHUP | POLLERR | POLLNVAL)) {
		                        if (other_idx == p0) goto pause_a;
		                        else goto pause_b;
		                    }
		                    if (op.revents & POLLIN) {
		                        int dr = drain_nonactive_player_input(
		                            L, other_idx, other_fd, turn, &forced_winner_idx,
		                            nonactive_inbuf[other_idx], &nonactive_inlen[other_idx]);
		                        if (dr < 0) {
		                            if (other_idx == p0) goto pause_a;
		                            else goto pause_b;
		                        }
		                        if (dr > 0) goto end_game;
		                    }
		                }
		            }

		            // Keep the current player alive with PING/PONG.
		            // The non-active player's socket is handled non-blocking above (PING/PONG + violations).
		            if (now - last_ping >= PING_INTERVAL_SEC) {
	                if (write_all(pfd, "C45PING\n") < 0) goto pause_turn;
	                last_ping = now;
	            }

            char buf[READ_BUF];
	            int r = read_line_timeout(pfd, buf, sizeof(buf), 1);
	            if (r == -2) {
	                // no input this second
		            } else if (r <= 0) {
		                goto pause_turn;
		            } else if (is_token(buf, "C45PONG")) {
		                last_pong = now;
		                continue;
		            } else if (is_token(buf, "C45PING")) {
		                (void)write_all(pfd, "C45PONG\n");
		                last_pong = now;
		                continue;
		            } else if (is_token(buf, "C45YES")) {
		                // Can arrive late from the lobby waiting phase; ignore.
		                continue;
		            } else if (is_back_request_for_name(buf, L->players[turn].name) == 1) {
	                active_name_mark_back(L->players[turn].name, pfd);
	                forced_winner_idx = 1 - turn;
	                goto end_game;
	            } else if (strncmp(buf, "C45HIT", 6) == 0) {
            pthread_mutex_lock(&L->mtx);
            Card nc = deck_draw(&L->deck);
            Player* P = &L->players[turn];
            P->hand[P->hand_size++] = nc;
            char cs[3]; card_to_str(nc, cs);
            pthread_mutex_unlock(&L->mtx);
            char msg[32]; snprintf(msg, sizeof(msg), "C45CARD %s\n", cs);
            if (write_all(pfd, msg) < 0) goto pause_turn;
	            // check for overhand
	            pthread_mutex_lock(&L->mtx);
	            int v = hand_value(P->hand, P->hand_size);
		            if (v > 21) {
		                P->busted = 1;
		                pthread_mutex_unlock(&L->mtx);
		                snprintf(line, sizeof(line), "C45BUST %s %d\n", P->name, v);
		                // Send bust only to the player who busted (do not reveal to opponent mid-game).
		                if (write_all(pfd, line) < 0) goto pause_turn;
		            } else {
		                pthread_mutex_unlock(&L->mtx);
		            }
	            // Step-by-step: after HIT (bust or not) the turn goes to the other player.
	            turn = 1 - turn;
	            break;
	            } else if (strncmp(buf, "C45STAND", 8) == 0) {
	                pthread_mutex_lock(&L->mtx);
	                L->players[turn].stood = 1;
                pthread_mutex_unlock(&L->mtx);
                turn = 1 - turn;
                break;
	            } else {
	                // Any other line is a protocol violation: kick the current player and end the game.
	                player_disconnect_fd(L, turn);
	                forced_winner_idx = 1 - turn;
	                goto end_game;
	            }

	            if (now - last_pong > PONG_TIMEOUT_SEC) goto pause_turn;

	            if (now - turn_start >= TURN_TIMEOUT_SEC) {
                // If the client is alive (keeps answering pong) -> timeout means auto-stand.
                // If not -> treat as disconnect and allow reconnect.
                if (now - last_pong > PONG_TIMEOUT_SEC) goto pause_turn;

                pthread_mutex_lock(&L->mtx);
                L->players[turn].stood = 1;
                pthread_mutex_unlock(&L->mtx);
                if (pfd >= 0) write_all(pfd, "C45TIMEOUT\n");
                turn = 1 - turn;
                break;
            }
        }
    }

    goto end_game;

pause_a: {
        int missing = p0;
        int other = 1 - missing;
        player_disconnect_fd(L, missing);
        int rr = wait_for_reconnect(L, missing, other);
        if (rr == 0) goto turn_loop;
        if (rr == 1) forced_winner_idx = other;
        goto end_game;
    }
pause_b: {
        int missing = p1;
        int other = 1 - missing;
        player_disconnect_fd(L, missing);
        int rr = wait_for_reconnect(L, missing, other);
        if (rr == 0) goto turn_loop;
        if (rr == 1) forced_winner_idx = other;
        goto end_game;
    }
pause_turn: {
        int missing = turn;
        int other = 1 - missing;
        player_disconnect_fd(L, missing);
        int rr = wait_for_reconnect(L, missing, other);
        if (rr == 0) goto turn_loop;
        if (rr == 1) forced_winner_idx = other;
        goto end_game;
    }

end_game:
    // count the points and announce the result
    pthread_mutex_lock(&L->mtx);
    int va = A->busted ? -1 : hand_value(A->hand, A->hand_size);
    int vb = B->busted ? -1 : hand_value(B->hand, B->hand_size);
    char winner_name[MAX_NAME_LEN];
    if (forced_winner_idx == p0) strncpy(winner_name, A->name, sizeof(winner_name) - 1);
    else if (forced_winner_idx == p1) strncpy(winner_name, B->name, sizeof(winner_name) - 1);
    else if (va > vb) strncpy(winner_name, A->name, sizeof(winner_name) - 1);
    else if (vb > va) strncpy(winner_name, B->name, sizeof(winner_name) - 1);
    else strncpy(winner_name, "PUSH", sizeof(winner_name) - 1);
    winner_name[sizeof(winner_name) - 1] = '\0';
    pthread_mutex_unlock(&L->mtx);

    char res[256];
    int res_len = snprintf(res, sizeof(res), "C45RESULT %s %d %s %d WINNER %s\n",
                           A->name, va, B->name, vb, winner_name);
    if (res_len < 0 || (size_t)res_len >= sizeof(res)) {
        snprintf(res, sizeof(res), "C45RESULT %s %d %s %d WINNER %s\n",
                 "?", va, "?", vb, "PUSH");
    }
    if (A->fd >= 0) write_all(A->fd, res);
    if (B->fd >= 0) write_all(B->fd, res);

    pthread_mutex_lock(&L->mtx);
    L->is_running = 0; // end for game
    pthread_mutex_unlock(&L->mtx);


    for (int p = 0; p < LOBBY_SIZE; ++p) {
        // Keep the name reserved until the client disconnects;
        // just remove the player from the lobby after the game ends.
        char* n = L->players[p].name;
        lobby_remove_player_by_name(n);
    }

    return NULL;
}

int lobby_name_exists(const char* name) {
    if (!name || !*name) return 0;
    for (int i = 0; i < g_lobby_count; ++i) {
        Lobby* L = &g_lobbies[i];
        pthread_mutex_lock(&L->mtx);
        for (int p = 0; p < LOBBY_SIZE; ++p) {
            Player* pl = &L->players[p];
            if (pl->connected && pl->name[0] &&
                strncmp(pl->name, name, MAX_NAME_LEN) == 0) {
                pthread_mutex_unlock(&L->mtx);
                return 1;
            }
        }
        pthread_mutex_unlock(&L->mtx);
    }
    return 0;
}

void lobbies_free(void) {
    if (!g_lobbies) return;

    for (int i = 0; i < g_lobby_count; ++i) {
        pthread_mutex_destroy(&g_lobbies[i].mtx);
    }

    free(g_lobbies);
    g_lobbies = NULL;
}
