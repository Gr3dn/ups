#ifndef SERVER_H
#define SERVER_H

/*
 * server.h
 *
 * Purpose:
 *   Public API of the TCP server and the global "active name" registry used
 *   for session coordination (prevent duplicate names, handle "back to lobby"
 *   requests across threads).
 *
 * Table of contents:
 *   - Server entry point: run_server()
 *   - Active name registry: active_name_*()
 */

/**
 * Start the TCP server loop (blocks until SIGINT or fatal error).
 *
 * @param bind_ip Bind address (e.g. "0.0.0.0" or "localhost").
 * @param port    Bind port (1..65535).
 * @return 0 on normal shutdown; non-zero on error.
 */
int run_server(const char* bind_ip, int port);

/**
 * Check whether a player name is currently reserved by an active connection.
 *
 * @param n Player name.
 * @return 1 if reserved; 0 otherwise.
 */
int active_name_has(const char* n);

/**
 * Reserve a player name in the global active registry.
 *
 * @param n Player name.
 * @return 0 on success; -1 on failure (registry full).
 */
int active_name_add(const char* n);

/**
 * Remove a player name from the global active registry.
 *
 * @param n Player name.
 */
void active_name_remove(const char* n); 

/**
 * Mark a "back to lobby" request for a player name.
 *
 * This is used to coordinate requests that happen inside the game thread
 * (where the network thread may not be waiting for input).
 *
 * @param n  Player name.
 * @param fd Optional socket fd for additional safety (pass -1 to ignore fd).
 */
void active_name_mark_back(const char* n, int fd);

/**
 * Consume a previously marked "back to lobby" request (if any).
 *
 * @param n  Player name.
 * @param fd Optional socket fd for additional safety (pass -1 to ignore fd).
 * @return 1 if a pending request was found and cleared; 0 otherwise.
 */
int  active_name_take_back(const char* n, int fd);
#endif
