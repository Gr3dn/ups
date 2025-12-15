#include "server.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    int port = 10000;
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port < 1 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return 1;
        }
    }

    lobbies_init();
    return run_server(port);
}
