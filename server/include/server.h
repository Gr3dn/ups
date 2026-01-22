#ifndef SERVER_H
#define SERVER_H


/* Start TCP server (blocks until SIGINT) */
int run_server(const char* bind_ip, int port);
int active_name_has(const char* n);
int active_name_add(const char* n);
void active_name_remove(const char* n); 

// Used to coordinate "Back to Lobby" requests that happen inside the game thread.
void active_name_mark_back(const char* n, int fd);
int  active_name_take_back(const char* n, int fd);
#endif
