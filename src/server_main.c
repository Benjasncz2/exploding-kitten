#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <cjson/cJSON.h>
#include "protocol.h"
#include "net_handler.h"
#include "pb_node.h"
#include "heartbeat.h"

void game_logic_loop(void);  /* game_logic.c */

/* ─── Tabla de clientes compartida ─────────────────────────────── */
static ClientSlot      clients[MAX_PLAYERS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             next_player_id = 0;
static int             fifo_in_fd  = -1;
static int             fifo_out_fd = -1;

/* ─── Señal de failover (Backup → main thread) ──────────────────── */
static volatile bool   failover_triggered = false;
static pthread_mutex_t failover_mutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  failover_cond      = PTHREAD_COND_INITIALIZER;

/* Llamado desde pb_replication.c cuando el Backup detecta caída del Primary */
void trigger_failover(void) {
    pthread_mutex_lock(&failover_mutex);
    failover_triggered = true;
    pthread_cond_signal(&failover_cond);
    pthread_mutex_unlock(&failover_mutex);
}

/* ─── Helpers de tabla de clientes ─────────────────────────────── */
static int add_client(int fd, const char *name) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!clients[i].active) {
            clients[i].fd        = fd;
            clients[i].player_id = next_player_id++;
            clients[i].active    = true;
            strncpy(clients[i].name, name, sizeof(clients[i].name) - 1);
            int pid = clients[i].player_id;
            pthread_mutex_unlock(&clients_mutex);
            return pid;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return -1;
}

static void remove_client(int fd) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (clients[i].active && clients[i].fd == fd) {
            clients[i].active = false;
            clients[i].fd     = -1;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

/* ─── Helpers de enrutado ───────────────────────────────────────── */
static void send_to_player(int player_id, const char *json) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (clients[i].active && clients[i].player_id == player_id) {
            send_message(clients[i].fd, json);
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

/* ─── Broadcast thread ──────────────────────────────────────────── */
/*
 * Lee la respuesta JSON de game_logic desde game_to_net y la enruta:
 *   - valid=false → error solo al player_id indicado
 *   - valid=true  → append_log (si Primary), broadcast estado, mensaje privado
 * El mutex se suelta ANTES del envío (invariante del plan).
 */
static void *broadcast_thread(void *arg) {
    (void)arg;
    while (1) {
        uint32_t len;
        if (read(fifo_out_fd, &len, sizeof(uint32_t)) != (ssize_t)sizeof(uint32_t))
            break;
        if (len == 0 || len > MAX_MSG_LEN) continue;

        char *raw = malloc(len + 1);
        if (!raw) continue;

        ssize_t got = 0;
        while (got < (ssize_t)len) {
            ssize_t n = read(fifo_out_fd, raw + got, len - got);
            if (n <= 0) { free(raw); return NULL; }
            got += n;
        }
        raw[len] = '\0';

        cJSON *resp = cJSON_Parse(raw);
        free(raw);
        if (!resp) continue;

        cJSON *valid_j   = cJSON_GetObjectItem(resp, "valid");
        cJSON *pid_j     = cJSON_GetObjectItem(resp, "player_id");
        cJSON *error_j   = cJSON_GetObjectItem(resp, "error");
        cJSON *bcast_j   = cJSON_GetObjectItem(resp, "broadcast");
        cJSON *ptarget_j = cJSON_GetObjectItem(resp, "private_target");
        cJSON *priv_j    = cJSON_GetObjectItem(resp, "private");
        cJSON *action_j  = cJSON_GetObjectItem(resp, "action_obj");

        bool valid     = cJSON_IsTrue(valid_j);
        int  player_id = pid_j     ? (int)pid_j->valuedouble     : -1;
        int  ptarget   = ptarget_j ? (int)ptarget_j->valuedouble : -1;

        if (!valid) {
            if (error_j && player_id >= 0) {
                cJSON *err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "type", "ERROR");
                cJSON_AddStringToObject(err, "msg",  error_j->valuestring ? error_j->valuestring : "");
                char *s = cJSON_PrintUnformatted(err);
                cJSON_Delete(err);
                send_to_player(player_id, s);
                free(s);
            }
            cJSON_Delete(resp);
            continue;
        }

        /* Solo Primary registra en el log */
        if (pb_node.role == ROLE_PRIMARY && action_j) {
            char *a_str = cJSON_PrintUnformatted(action_j);
            append_log_entry(a_str);
            free(a_str);
        }

        /* Broadcast estado a todos los clientes */
        if (bcast_j) {
            char *bcast_str = cJSON_PrintUnformatted(bcast_j);

            int fds[MAX_PLAYERS];
            int nfds = 0;
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (clients[i].active) fds[nfds++] = clients[i].fd;
            pthread_mutex_unlock(&clients_mutex);

            #pragma omp parallel for schedule(dynamic)
            for (int i = 0; i < nfds; i++)
                send_message(fds[i], bcast_str);

            free(bcast_str);
        }

        /* Mensaje privado (ej. SEE_FUTURE peek) */
        if (priv_j && ptarget >= 0) {
            char *priv_str = cJSON_PrintUnformatted(priv_j);
            send_to_player(ptarget, priv_str);
            free(priv_str);
        }

        cJSON_Delete(resp);
    }
    return NULL;
}

/* ─── Client thread ─────────────────────────────────────────────── */
static void *client_thread(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    struct timeval tv = {120, 0};   /* 2 min: tiempo para PING cada 20s */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int player_id = -1;

    while (1) {
        char *json = recv_message(fd);
        if (!json) break;

        cJSON *msg   = cJSON_Parse(json);
        free(json);
        if (!msg) break;

        cJSON *type_j = cJSON_GetObjectItem(msg, "type");
        cJSON *ts_j   = cJSON_GetObjectItem(msg, "ts");
        uint32_t client_ts = ts_j ? (uint32_t)ts_j->valuedouble : 0;

        /* Actualizar reloj Lamport del nodo */
        pthread_mutex_lock(&pb_mutex);
        if (client_ts > pb_node.lamport_clock)
            pb_node.lamport_clock = client_ts;
        pb_node.lamport_clock++;
        pthread_mutex_unlock(&pb_mutex);

        const char *type = (type_j && type_j->valuestring) ? type_j->valuestring : "UNKNOWN";

        /* Keepalive: no reenviar al game_logic */
        if (strcmp(type, "PING") == 0) {
            cJSON_Delete(msg);
            continue;
        }

        if (strcmp(type, "JOIN") == 0 && player_id < 0) {
            cJSON *name_j = cJSON_GetObjectItem(msg, "player_name");
            const char *name = (name_j && name_j->valuestring) ? name_j->valuestring : "Anon";
            player_id = add_client(fd, name);
            if (player_id < 0) {
                send_message(fd, "{\"type\":\"ERROR\",\"msg\":\"Sala llena\"}");
                cJSON_Delete(msg);
                break;
            }
            printf("[server] '%s' → player_id=%d\n", name, player_id);
            fflush(stdout);
        }

        /*
         * Construir Action y escribir al FIFO net_to_game.
         * El log se registra en broadcast_thread DESPUÉS de que game_logic
         * valide la acción (solo acciones válidas entran al log).
         */
        Action action = {0};
        action.player_id  = player_id;
        pthread_mutex_lock(&pb_mutex);
        action.lamport_ts = pb_node.lamport_clock;
        pthread_mutex_unlock(&pb_mutex);
        strncpy(action.type, type, sizeof(action.type) - 1);

        /* El payload puede venir del campo "payload" o de campos específicos */
        cJSON *payload_j = cJSON_GetObjectItem(msg, "payload");
        if (payload_j && payload_j->valuestring) {
            strncpy(action.payload, payload_j->valuestring, sizeof(action.payload) - 1);
        } else if (strcmp(type, "JOIN") == 0) {
            cJSON *name_j = cJSON_GetObjectItem(msg, "player_name");
            if (name_j && name_j->valuestring)
                strncpy(action.payload, name_j->valuestring, sizeof(action.payload) - 1);
        }

        WRITE(fifo_in_fd, &action, sizeof(Action));
        cJSON_Delete(msg);
    }

    printf("[server] player_id=%d desconectado.\n", player_id);
    fflush(stdout);
    remove_client(fd);
    close(fd);
    return NULL;
}

/* ─── Socket helpers ────────────────────────────────────────────── */
static int make_server_socket(int port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt  = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(port),
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return -1;
    }
    listen(srv, MAX_PLAYERS);
    return srv;
}

static void accept_loop(int srv) {
    const char *role_str = pb_node.role == ROLE_PRIMARY ? "PRIMARY" : "BACKUP→PRIMARY";
    printf("[%s] Aceptando clientes en :%d\n", role_str, SERVER_PORT);
    fflush(stdout);

    while (1) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) { perror("accept"); continue; }
        int *fd_ptr = malloc(sizeof(int));
        *fd_ptr = cfd;
        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, fd_ptr);
        pthread_detach(tid);
    }
}

/* ─── Hilo del Primary: escucha al Backup en REPL_PORT ──────────── */
static void *replication_server_thread(void *arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt  = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(REPL_PORT),
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("repl bind"); return NULL;
    }
    listen(srv, 1);
    printf("[primary] Esperando conexión del Backup en :%d...\n", REPL_PORT);
    fflush(stdout);

    pb_node.backup_fd = accept(srv, NULL, NULL);
    close(srv);

    if (pb_node.backup_fd < 0) { perror("repl accept"); return NULL; }

    pthread_mutex_lock(&pb_mutex);
    pb_node.backup_alive = true;
    pthread_mutex_unlock(&pb_mutex);

    printf("[primary] Backup conectado. Canal de replicación activo.\n");
    fflush(stdout);

    pthread_t hb_tid;
    pthread_create(&hb_tid, NULL, heartbeat_sender, NULL);
    pthread_detach(hb_tid);

    return NULL;
}

/* ─── Backup: conectar al Primary ──────────────────────────────── */
static int connect_to_primary(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[backup] IP inválida: %s\n", host); return -1;
    }
    for (int i = 0; i < 15; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            printf("[backup] Conectado a Primary %s:%d\n", host, port);
            fflush(stdout);
            return fd;
        }
        printf("[backup] Esperando Primary... (%d/15)\n", i + 1);
        fflush(stdout);
        sleep(1);
    }
    fprintf(stderr, "[backup] No se pudo conectar al Primary.\n");
    return -1;
}

/* ─── Inicialización común (FIFOs + fork game_logic) ────────────── */
static void init_fifos_and_game_logic(void) {
    unlink(FIFO_NET_TO_GAME);
    unlink(FIFO_GAME_TO_NET);
    if (mkfifo(FIFO_NET_TO_GAME, 0600) < 0 ||
        mkfifo(FIFO_GAME_TO_NET,  0600) < 0) {
        perror("mkfifo"); exit(1);
    }

    pid_t child = fork();
    if (child < 0) { perror("fork"); exit(1); }
    if (child == 0) { game_logic_loop(); _exit(0); }

    /* Abrir ambos extremos — el orden hace que no haya deadlock */
    fifo_in_fd  = open(FIFO_NET_TO_GAME, O_WRONLY);
    fifo_out_fd = open(FIFO_GAME_TO_NET,  O_RDONLY);
    if (fifo_in_fd < 0 || fifo_out_fd < 0) { perror("open FIFOs"); exit(1); }

    pthread_t bcast_tid;
    pthread_create(&bcast_tid, NULL, broadcast_thread, NULL);
    pthread_detach(bcast_tid);
}

/* ─── main ──────────────────────────────────────────────────────── */
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    /* Leer rol y peer desde variables de entorno */
    const char *role_env = getenv("ROLE");
    const char *peer_env = getenv("PEER_HOST");
    NodeRole role = (role_env && strcmp(role_env, "backup") == 0)
                    ? ROLE_BACKUP : ROLE_PRIMARY;
    const char *peer_host = peer_env ? peer_env : "127.0.0.1";

    pb_node_init(role, peer_host);
    init_fifos_and_game_logic();

    if (role == ROLE_PRIMARY) {
        printf("[PRIMARY pid=%d] Iniciando en :%d\n", getpid(), SERVER_PORT);
        fflush(stdout);

        /* Hilo que espera al Backup en REPL_PORT (no bloquea el accept loop) */
        pthread_t repl_tid;
        pthread_create(&repl_tid, NULL, replication_server_thread, NULL);
        pthread_detach(repl_tid);

        int srv = make_server_socket(SERVER_PORT);
        if (srv < 0) return 1;
        accept_loop(srv);

    } else {
        printf("[BACKUP pid=%d] Conectando a Primary %s:%d\n",
               getpid(), peer_host, REPL_PORT);
        fflush(stdout);

        pb_node.primary_fd = connect_to_primary(peer_host, REPL_PORT);
        if (pb_node.primary_fd < 0) return 1;

        /* Hilo que recibe log entries + heartbeats del Primary */
        pthread_t repl_tid;
        pthread_create(&repl_tid, NULL, backup_replication_thread, NULL);
        pthread_detach(repl_tid);

        /* Hilo que monitorea timeout de heartbeat */
        pthread_t hb_tid;
        pthread_create(&hb_tid, NULL, heartbeat_monitor, NULL);
        pthread_detach(hb_tid);

        /* Esperar señal de failover */
        pthread_mutex_lock(&failover_mutex);
        while (!failover_triggered)
            pthread_cond_wait(&failover_cond, &failover_mutex);
        pthread_mutex_unlock(&failover_mutex);

        printf("[backup→primary] Promovido. Iniciando en :%d\n", SERVER_PORT);
        fflush(stdout);

        int srv = make_server_socket(SERVER_PORT);
        if (srv < 0) return 1;
        accept_loop(srv);
    }

    return 0;
}
