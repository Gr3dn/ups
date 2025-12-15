#include "game.h"
#include "protocol.h"
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

Lobby      g_lobbies[LOBBY_COUNT];
atomic_int g_server_running = 1;
static void* lobby_game_thread(void* arg);
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
void lobbies_init(void) {
    srand((unsigned)time(NULL));
    for (int i = 0; i < LOBBY_COUNT; ++i) {
        g_lobbies[i].player_count = 0;
        g_lobbies[i].is_running   = 0;
        pthread_mutex_init(&g_lobbies[i].mtx, NULL);
        deck_init(&g_lobbies[i].deck);
        deck_shuffle(&g_lobbies[i].deck);
        for (int p = 0; p < LOBBY_SIZE; ++p) {
            g_lobbies[i].players[p].name[0] = '\0';
            g_lobbies[i].players[p].hand_size = 0;
            g_lobbies[i].players[p].connected = 0;
        }
    }
}

int lobby_try_add_player(int lobby_index, const char* name) {
    if (lobby_index < 0 || lobby_index >= LOBBY_COUNT) return -1;

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
    for (int i = 0; i < LOBBY_COUNT; ++i) {
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
    if (li < 0 || li >= LOBBY_COUNT) return -1;
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

static void* lobby_game_thread(void* arg) {
    int li = *(int*)arg; free(arg);
    Lobby* L = &g_lobbies[li];

    // preparing deck and hands
    pthread_mutex_lock(&L->mtx);
    deck_shuffle(&L->deck);
    for (int p = 0; p < LOBBY_SIZE; ++p) {
        L->players[p].hand_size = 0;
        L->players[p].stood = 0;    
        L->players[p].busted = 0;
    }
    int p0 = rand() % 2;
    int p1 = 1 - p0;
    Player *A = &L->players[p0], *B = &L->players[p1];

    // deals 2 cards
    for (int p = 0; p < 2; ++p) {
        A->hand[A->hand_size++] = deck_draw(&L->deck);
        B->hand[B->hand_size++] = deck_draw(&L->deck);
    }
    char c1[3], c2[3], line[64];
    card_to_str(A->hand[0], c1); card_to_str(A->hand[1], c2);
    snprintf(line, sizeof(line), "C45DEAL %s %s\n", c1, c2); write_all(A->fd, line);
    card_to_str(B->hand[0], c1); card_to_str(B->hand[1], c2);
    snprintf(line, sizeof(line), "C45DEAL %s %s\n", c1, c2); write_all(B->fd, line);
    pthread_mutex_unlock(&L->mtx);

    int turn = p0; // hit for player 0
    for (;;) {
        pthread_mutex_lock(&L->mtx);
        Player* P = &L->players[turn];
        if ((A->stood||A->busted) && (B->stood||B->busted)) { pthread_mutex_unlock(&L->mtx); break; }
        snprintf(line, sizeof(line), "C45TURN %s 30\n", P->name);
        write_all(A->fd, line); write_all(B->fd, line);
        pthread_mutex_unlock(&L->mtx);

        // waiting
        char buf[READ_BUF];
        int r = read_line_timeout(P->fd, buf, sizeof(buf), 30);
        if (r == -2) { // timeout => auto-stand
            write_all(P->fd, "C45TIMEOUT\n");
            pthread_mutex_lock(&L->mtx); P->stood = 1; pthread_mutex_unlock(&L->mtx);
            turn = 1 - turn; continue;
        }
        if (r <= 0) { // disconnect
            pthread_mutex_lock(&L->mtx); P->busted = 1; pthread_mutex_unlock(&L->mtx);
            turn = 1 - turn; continue;
        }
        if (strncmp(buf, "C45HIT", 6) == 0) {
            pthread_mutex_lock(&L->mtx);
            Card nc = deck_draw(&L->deck);
            P->hand[P->hand_size++] = nc;
            char cs[3]; card_to_str(nc, cs);
            pthread_mutex_unlock(&L->mtx);
            char msg[32]; snprintf(msg, sizeof(msg), "C45CARD %s\n", cs);
            write_all(P->fd, msg);
            // check for overhand
            pthread_mutex_lock(&L->mtx);
            int v = hand_value(P->hand, P->hand_size);
            if (v > 21) { P->busted = 1; pthread_mutex_unlock(&L->mtx);
                snprintf(line, sizeof(line), "C45BUST %s %d\n", P->name, v);
                write_all(A->fd, line); write_all(B->fd, line);
                turn = 1 - turn;
            } else { 
                pthread_mutex_unlock(&L->mtx); /* the same player moves again (the timer restarts in a loop) */
            }
            continue;
        }
        if (strncmp(buf, "C45STAND", 8) == 0) {
            pthread_mutex_lock(&L->mtx); P->stood = 1; pthread_mutex_unlock(&L->mtx);
            turn = 1 - turn;
            continue;
        }
    }

    // count the points and announce the result
    pthread_mutex_lock(&L->mtx);
    int va = A->busted ? -1 : hand_value(A->hand, A->hand_size);
    int vb = B->busted ? -1 : hand_value(B->hand, B->hand_size);
    const char* winner = "PUSH";
    if (va > vb) winner = A->name;
    else if (vb > va) winner = B->name;
    pthread_mutex_unlock(&L->mtx);

    char res[128];
    snprintf(res, sizeof(res), "C45RESULT %s %d %s %d WINNER %s\n",
             A->name, va, B->name, vb, winner);
    write_all(A->fd, res); write_all(B->fd, res);

    pthread_mutex_lock(&L->mtx);
    L->is_running = 0; // end for game
    pthread_mutex_unlock(&L->mtx);


    for (int p = 0; p < LOBBY_SIZE; ++p) {
        char* n = L->players[p].name;
        active_name_remove(n);
        lobby_remove_player_by_name(n);
    }

    return NULL;
}

int lobby_name_exists(const char* name) {
    if (!name || !*name) return 0;
    for (int i = 0; i < LOBBY_COUNT; ++i) {
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
