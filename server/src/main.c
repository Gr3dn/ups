#include "server.h"
#include "game.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_help(const char* prog) {
    printf("Usage:\n");
    printf("  %s [-i IP] [-p PORT]\n", prog);
    printf("  %s -help\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  -i IP     Bind IP address (example: 0.0.0.0 or localhost)\n");
    printf("  -p PORT   Bind port (1..65535)\n");
    printf("  -help     Show this help and exit\n");
    printf("\n");
    printf("Notes:\n");
    printf("  - IP/PORT are taken from config.txt by default.\n");
    printf("  - To override via CLI, you must provide both -i and -p.\n");
    printf("  - If CLI IP/PORT are invalid, config.txt is used.\n");
    printf("  - If config.txt IP/PORT are invalid, defaults are used.\n");
}

static int parse_port_strict(const char* s, int* out_port) {
    if (!s || !*s) return -1;
    char* end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    if (v < 1 || v > 65535) return -1;
    *out_port = (int)v;
    return 0;
}

static int is_ip_valid(const char* ip) {
    if (!ip || !*ip) return 0;
    if (strcmp(ip, "localhost") == 0) return 1;
    struct in_addr tmp;
    return inet_pton(AF_INET, ip, &tmp) == 1;
}

static void set_server_net(const char* ip, int port) {
    snprintf(g_server_ip, sizeof(g_server_ip), "%s", ip);
    g_server_port = port;
}

typedef struct {
    int requested;
    int has_ip;
    int has_port;
    int ok;
    const char* ip_raw;
    const char* port_raw;
    char ip[64];
    int  port;
} CliNet;

typedef struct {
    int file_found;
    int has_ip;
    int has_port;
    int ok;
    char ip[64];
    char port_raw[32];
    int  port;
} ConfigNet;

// Returns: 0=ok, -1=fatal error (unknown option)
static int parse_cli_net(int argc, char** argv, CliNet* out) {
    memset(out, 0, sizeof(*out));

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];

        if (strcmp(a, "-i") == 0) {
            out->requested = 1;
            if (i + 1 >= argc) return 0;
            out->has_ip = 1;
            out->ip_raw = argv[++i];
            continue;
        }
        if (strcmp(a, "-p") == 0) {
            out->requested = 1;
            if (i + 1 >= argc) return 0;
            out->has_port = 1;
            out->port_raw = argv[++i];
            continue;
        }

        if (strcmp(a, "-help") == 0 || strcmp(a, "--help") == 0) {
            // handled before load_config()
            continue;
        }

        // Backward-compat positional arg (legacy): ./blackjack_server 10000
        // Treated as incomplete CLI net config (we require both -i and -p).
        if (argc == 2 && a[0] != '-') {
            out->requested = 1;
            out->has_port = 1;
            out->port_raw = a;
            continue;
        }

        if (a[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", a);
            return -1;
        }

        fprintf(stderr, "Unexpected argument: %s\n", a);
        return -1;
    }

    if (!out->requested) return 0;
    if (!out->has_ip || !out->has_port) return 0;

    if (!is_ip_valid(out->ip_raw)) return 0;
    int p = 0;
    if (parse_port_strict(out->port_raw, &p) != 0) return 0;

    strncpy(out->ip, out->ip_raw, sizeof(out->ip) - 1);
    out->ip[sizeof(out->ip) - 1] = '\0';
    out->port = p;
    out->ok = 1;
    return 0;
}

static void parse_config_net(const char* filename, ConfigNet* out) {
    memset(out, 0, sizeof(*out));

    FILE* f = fopen(filename, "r");
    if (!f) return;
    out->file_found = 1;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {0};
        char val[128] = {0};
        if (sscanf(line, " %63s %127s", key, val) != 2) continue;

        if (strcmp(key, "IP") == 0) {
            out->has_ip = 1;
            strncpy(out->ip, val, sizeof(out->ip) - 1);
            out->ip[sizeof(out->ip) - 1] = '\0';
        } else if (strcmp(key, "PORT") == 0) {
            out->has_port = 1;
            strncpy(out->port_raw, val, sizeof(out->port_raw) - 1);
            out->port_raw[sizeof(out->port_raw) - 1] = '\0';
        }
    }

    fclose(f);

    if (!out->has_ip || !out->has_port) return;
    if (!is_ip_valid(out->ip)) return;

    int p = 0;
    if (parse_port_strict(out->port_raw, &p) != 0) return;

    out->port = p;
    out->ok = 1;
}

int main(int argc, char** argv) {
    // Help must not depend on config.txt (and should not start the server).
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        }
    }

    // Save compile-time defaults (defined in src/game.c) for fallback.
    char default_ip[64];
    strncpy(default_ip, g_server_ip, sizeof(default_ip) - 1);
    default_ip[sizeof(default_ip) - 1] = '\0';
    int default_port = g_server_port;

    load_config("config.txt");

    CliNet cli = {0};
    if (parse_cli_net(argc, argv, &cli) < 0) {
        print_help(argv[0]);
        return 1;
    }

    ConfigNet cfg = {0};
    parse_config_net("config.txt", &cfg);

    if (cli.requested) {
        if (cli.ok) {
            set_server_net(cli.ip, cli.port);
        } else if (cfg.ok) {
            if (!cli.has_ip || !cli.has_port) {
                fprintf(stderr, "Invalid CLI IP/PORT: please provide both -i and -p\n");
            } else {
                if (!is_ip_valid(cli.ip_raw)) fprintf(stderr, "Invalid IP: %s\n", cli.ip_raw);
                int p = 0;
                if (parse_port_strict(cli.port_raw, &p) != 0) fprintf(stderr, "Invalid port: %s\n", cli.port_raw);
            }
            fprintf(stderr, "Using IP and PORT from config.txt instead.\n");
            set_server_net(cfg.ip, cfg.port);
        } else {
            if (!cli.has_ip || !cli.has_port) {
                fprintf(stderr, "Invalid CLI IP/PORT: please provide both -i and -p\n");
            } else {
                if (!is_ip_valid(cli.ip_raw)) fprintf(stderr, "Invalid IP: %s\n", cli.ip_raw);
                int p = 0;
                if (parse_port_strict(cli.port_raw, &p) != 0) fprintf(stderr, "Invalid port: %s\n", cli.port_raw);
            }

            if (!cfg.file_found) {
                fprintf(stderr, "config.txt not found.\n");
            } else if (!cfg.has_ip || !cfg.has_port) {
                fprintf(stderr, "Invalid config.txt IP/PORT: missing IP or PORT.\n");
            } else {
                if (!is_ip_valid(cfg.ip)) fprintf(stderr, "Invalid config.txt IP: %s\n", cfg.ip);
                int p = 0;
                if (parse_port_strict(cfg.port_raw, &p) != 0) fprintf(stderr, "Invalid config.txt port: %s\n", cfg.port_raw);
            }

            fprintf(stderr,
                    "Cannot use IP/PORT from CLI or config.txt; using defaults %s:%d.\n",
                    default_ip, default_port);
            set_server_net(default_ip, default_port);
        }
    } else {
        if (cfg.ok) {
            set_server_net(cfg.ip, cfg.port);
        } else {
            if (cfg.file_found) {
                if (!cfg.has_ip || !cfg.has_port) {
                    fprintf(stderr, "Invalid config.txt IP/PORT: missing IP or PORT.\n");
                } else {
                    if (!is_ip_valid(cfg.ip)) fprintf(stderr, "Invalid config.txt IP: %s\n", cfg.ip);
                    int p = 0;
                    if (parse_port_strict(cfg.port_raw, &p) != 0) fprintf(stderr, "Invalid config.txt port: %s\n", cfg.port_raw);
                }
            }
            fprintf(stderr, "Using defaults %s:%d.\n", default_ip, default_port);
            set_server_net(default_ip, default_port);
        }
    }

    if (lobbies_init() != 0) {
        fprintf(stderr, "Failed to init lobbies\n");
        return 1;
    }
    int ret = run_server(g_server_ip, g_server_port);
    lobbies_free();
    return ret;
}
