#ifndef GAME_H
#define GAME_H

/*
 * game.h
 *
 * Purpose:
 *   Core game/lobby data structures and APIs for the Blackjack server.
 *   This header is used by both the network thread (server.c) and the game
 *   thread (game.c).
 *
 * Table of contents:
 *   - Constants and global configuration
 *   - Card/deck types and helpers
 *   - Lobby/player structures
 *   - Lobby lifecycle and game helpers
 */

#include <pthread.h>
#include <stdatomic.h>

#ifdef __cplusplus
#endif

#define MAX_NAME_LEN 64
#define LOBBY_SIZE  2
#define DECK_SIZE   52

/* --- Server network configuration (loaded from config.txt) --- */
extern char g_server_ip[64];   /* Bind address, e.g. "0.0.0.0" or "127.0.0.1" */
extern int  g_server_port;     /* 1..65535 */

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
    int    is_running;    /* 0 = not running, 1 = running */
    Deck   deck;
    pthread_mutex_t mtx;
} Lobby;

/* --- Global lobby pool and server lifecycle flag --- */
extern atomic_int  g_server_running;
/* Dynamic lobby configuration */
extern int   g_lobby_count;
extern Lobby *g_lobbies;

/**
 * Load server configuration from a text file.
 *
 * Recognized keys:
 *   - LOBBY_COUNT (1..1000)
 *   - IP (bind address)
 *   - PORT (1..65535)
 *
 * @param filename Path to config file.
 * @return 0 on success (including "file missing" fallback); -1 on fatal error.
 */
int  load_config(const char* filename);

/**
 * Initialize the global lobby array (@p g_lobbies) using @p g_lobby_count.
 *
 * @return 0 on success; -1 on allocation failure.
 */
int lobbies_init(void);

/**
 * Free the global lobby array (@p g_lobbies) and associated mutexes.
 */
void lobbies_free(void);

/**
 * Compute the Blackjack value of a hand.
 *
 * Aces count as 11 until the sum would exceed 21, then they become 1.
 *
 * @param hand Array of cards.
 * @param n    Number of cards in @p hand.
 * @return Hand value (0..).
 */
int  hand_value(const Card* hand, int n);

/**
 * Start a game thread if the lobby has enough players and is not already running.
 *
 * @param lobby_index Zero-based lobby index.
 * @return 0 on success.
 */
int  start_game_if_ready(int lobby_index);

/**
 * Check whether a player name currently exists in any lobby.
 *
 * @param name Player name.
 * @return 1 if present; 0 otherwise.
 */
int lobby_name_exists(const char* name);

/**
 * Try to add a player into a lobby.
 *
 * @param lobby_index Zero-based lobby index.
 * @param name        Player name (MAX_NAME_LEN limit applies).
 * @return 0 on success; -1 if the lobby is full or index invalid.
 */
int  lobby_try_add_player(int lobby_index, const char* name);

/**
 * Attach a connected socket fd to a previously-added player in a lobby.
 *
 * @param lobby_index Zero-based lobby index.
 * @param name        Player name.
 * @param fd          Connected socket file descriptor.
 * @return 0 on success; -1 if no such player/lobby.
 */
int  lobby_attach_fd(int lobby_index, const char* name, int fd);

/**
 * Remove a player from any lobby by name (if present).
 *
 * @param name Player name.
 */
void lobby_remove_player_by_name(const char* name);


/**
 * Initialize a deck in a known ordered state.
 *
 * @param d Deck to initialize.
 */
void deck_init(Deck* d);

/**
 * Shuffle a deck in-place.
 *
 * @param d Deck to shuffle.
 */
void deck_shuffle(Deck* d);

/**
 * Convert a card to a two-character string representation.
 *
 * @param c   Card to format.
 * @param out Output buffer of size 3 (two chars + '\0').
 */
void card_to_str(Card c, char out[3]);

/**
 * Draw one card from the deck (auto-shuffles when exhausted).
 *
 * @param d Deck to draw from.
 * @return Drawn card.
 */
Card deck_draw(Deck* d);

#ifdef __cplusplus

#endif
#endif /* GAME_H */
