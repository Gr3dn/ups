#ifndef SERVER_H
#define SERVER_H


/* Start TCP server (blocks until SIGINT) */
int run_server(int port);
int active_name_has(const char* n);
int active_name_add(const char* n);
void active_name_remove(const char* n); 
#endif
