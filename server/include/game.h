#ifndef GAME_H
#define GAME_H

#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
#endif

#define MAX_NAME_LEN 64
#define LOBBY_SIZE  2
#define DECK_SIZE   52

// Server network config (loaded from config.txt)
extern char g_server_ip[64];   // e.g. "0.0.0.0" or "192.168.1.106"
extern int  g_server_port;     // 1..65535

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
extern atomic_int  g_server_running;
// Dynamic lobby configuration
extern int   g_lobby_count;
extern Lobby *g_lobbies;

// Loads config (LOBBY_COUNT) from file
int  load_config(const char* filename);

// Frees allocated lobbies
void lobbies_free(void);

int lobbies_init(void);
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
