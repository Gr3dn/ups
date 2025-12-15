#include "server.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    load_config("config.txt");

    // Optional: CLI override for port (if you want)
    if (argc >= 2) {
        int p = atoi(argv[1]);
        if (p < 1 || p > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return 1;
        }
        g_server_port = p;
    }

    if (lobbies_init() != 0) {
        fprintf(stderr, "Failed to init lobbies\n");
        return 1;
    }
    int ret = run_server(g_server_ip, g_server_port);
    lobbies_free();
    return ret;
}

