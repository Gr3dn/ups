#ifndef GAME_H
#define GAME_H

#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
#endif

#define MAX_NAME_LEN 64
#define LOBBY_COUNT 5
#define LOBBY_SIZE  2
#define DECK_SIZE   52

typedef enum { CLUBS, DIAMONDS, HEARTS, SPADES } Suit;

typedef struct { int rank; Suit suit; } Card; /* rank: 1..13 (A..K) */

typedef struct {
    Card cards[DECK_SIZE];
    int  top;
} Deck;

typedef struct {
    char name[MAX_NAME_LEN];
    Card hand[12];
    int  hand_size;
    int  connected;   /* 0/1 */
    int fd, stood, busted;
} Player;

typedef struct {
    Player players[LOBBY_SIZE];
    int    player_count;
    int    is_running;    /* 0 — not playing, 1 — playing */
    Deck   deck;
    pthread_mutex_t mtx;
} Lobby;

/* Global lobby pool and server flag */
extern Lobby       g_lobbies[LOBBY_COUNT];
extern atomic_int  g_server_running;

void lobbies_init(void);
int  hand_value(const Card* hand, int n);         // Ace 1/11
int  start_game_if_ready(int lobby_index);
int lobby_name_exists(const char* name);

int  lobby_try_add_player(int lobby_index, const char* name); /* 0=OK, -1=full */
int  lobby_attach_fd(int lobby_index, const char* name, int fd);
void lobby_remove_player_by_name(const char* name);


void deck_init(Deck* d);
void deck_shuffle(Deck* d);
void card_to_str(Card c, char out[3]);            // "AS","TD","7H"...
Card deck_draw(Deck* d);

#ifdef __cplusplus

#endif
#endif /* GAME_H */
