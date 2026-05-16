#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include "routes.h"
#include "perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define DEFAULT_PORT    8080
#define BACKLOG         64

static volatile int g_running = 1;
static perf_ctx_t  *g_ctx     = NULL;

static void handle_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

/* Hilo por conexión */
typedef struct {
    int           fd;
    perf_ctx_t   *ctx;
} conn_args_t;

static void *connection_handler(void *arg)
{
    conn_args_t *ca = (conn_args_t *)arg;
    int fd = ca->fd;
    perf_ctx_t *ctx = ca->ctx;
    free(ca);

    http_request_t req;
    if (http_parse(fd, &req) == 0)
        routes_dispatch(ctx, &req, fd);

    close(fd);
    return NULL;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [--port <n>] [--period-ms <n>] [--source proc|cgroup|auto]\n"
        "  --port       <n>  Puerto TCP (defecto: %d)\n"
        "  --period-ms  <n>  Intervalo de muestreo en ms (defecto: 100)\n"
        "  --source     <s>  Fuente de datos: proc, cgroup, auto (defecto: auto)\n",
        prog, DEFAULT_PORT);
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    perf_config_t cfg = PERF_CONFIG_DEFAULT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--period-ms") == 0 && i + 1 < argc) {
            cfg.sample_period_ms = (unsigned int)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
            i++;
            if      (strcmp(argv[i], "proc")   == 0) cfg.source = PERF_SRC_PROC;
            else if (strcmp(argv[i], "cgroup") == 0) cfg.source = PERF_SRC_CGROUP;
            else                                      cfg.source = PERF_SRC_AUTO;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Argumento desconocido: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Puerto inválido: %d\n", port);
        return 1;
    }

    g_ctx = perf_init(&cfg);
    if (!g_ctx) {
        fprintf(stderr, "Error: perf_init falló (¿cgroup v2 no disponible?)\n");
        return 1;
    }

    /* Socket de escucha */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); perf_shutdown(g_ctx); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv); perf_shutdown(g_ctx); return 1;
    }
    if (listen(srv, BACKLOG) < 0) {
        perror("listen"); close(srv); perf_shutdown(g_ctx); return 1;
    }

    signal(SIGINT,  handle_sigint);
    signal(SIGTERM, handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stdout,
        "perfanalyzer server escuchando en 0.0.0.0:%d\n"
        "  fuente : %s\n"
        "  período: %u ms\n"
        "  cgroup : %s\n",
        port,
        cfg.source == PERF_SRC_PROC   ? "proc"   :
        cfg.source == PERF_SRC_CGROUP ? "cgroup" : "auto",
        cfg.sample_period_ms,
        g_ctx->has_cgroup ? g_ctx->cg_path : "no disponible");
    fflush(stdout);

    while (g_running) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int fd = accept(srv, (struct sockaddr *)&cli_addr, &cli_len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        conn_args_t *ca = malloc(sizeof(*ca));
        if (!ca) { close(fd); continue; }
        ca->fd  = fd;
        ca->ctx = g_ctx;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, connection_handler, ca) != 0) {
            free(ca);
            close(fd);
        }
        pthread_attr_destroy(&attr);
    }

    fprintf(stdout, "\nApagando servidor...\n");
    close(srv);
    perf_shutdown(g_ctx);
    return 0;
}
