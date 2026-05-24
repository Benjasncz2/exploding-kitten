#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include "pb_node.h"
#include "protocol.h"

PBNode          pb_node;
pthread_mutex_t pb_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile bool failover_done = false;

void pb_node_init(NodeRole role, const char *peer_host) {
    memset(&pb_node, 0, sizeof(PBNode));
    pb_node.role           = role;
    pb_node.backup_fd      = -1;
    pb_node.primary_fd     = -1;
    pb_node.backup_alive   = false;
    pb_node.last_heartbeat = time(NULL);
    strncpy(pb_node.peer_host, peer_host, sizeof(pb_node.peer_host) - 1);
    snprintf(pb_node.log_file, sizeof(pb_node.log_file),
             "/tmp/ek_%s.log", role == ROLE_PRIMARY ? "primary" : "backup");
}

void persist_log_to_disk(const LogEntry *entry) {
    int fd = open(pb_node.log_file, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    ssize_t _w = write(fd, entry, sizeof(LogEntry)); (void)_w;
    fsync(fd);
    close(fd);
}

/* ─── Primary: agregar entrada al log y replicar ─────────────── */

void append_log_entry(const char *action_json) {
    pthread_mutex_lock(&pb_mutex);

    if (pb_node.log_size >= MAX_LOG_ENTRIES) {
        pthread_mutex_unlock(&pb_mutex);
        fprintf(stderr, "[pb] Log lleno!\n");
        return;
    }

    pb_node.lamport_clock++;
    LogEntry *e = &pb_node.log[pb_node.log_size++];
    e->lamport_ts = pb_node.lamport_clock;
    e->index      = (uint32_t)pb_node.log_size;
    strncpy(e->command, action_json, sizeof(e->command) - 1);

    LogEntry local = *e;   /* copia antes de soltar el mutex */
    pthread_mutex_unlock(&pb_mutex);

    printf("[primary] Log[%u] ts=%u  %.80s\n",
           local.index, local.lamport_ts, local.command);
    fflush(stdout);

    persist_log_to_disk(&local);
    replicate_to_backup(&local);
}

void replicate_to_backup(LogEntry *entry) {
    pthread_mutex_lock(&pb_mutex);
    bool alive = pb_node.backup_alive;
    int  bfd   = pb_node.backup_fd;
    pthread_mutex_unlock(&pb_mutex);

    if (!alive || bfd < 0) return;

    ReplMsg msg = { .msg_type = REPL_LOG_ENTRY, .entry = *entry };

    /* Timeout de 500ms para recibir ACK */
    struct timeval tv = {0, 500000};
    setsockopt(bfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (send(bfd, &msg, sizeof(ReplMsg), MSG_NOSIGNAL) <= 0) {
        pthread_mutex_lock(&pb_mutex);
        pb_node.backup_alive = false;
        pthread_mutex_unlock(&pb_mutex);
        fprintf(stderr, "[primary] Backup caído (send falló). Continuando sin replicar.\n");
        return;
    }

    ReplMsg ack;
    ssize_t n = recv(bfd, &ack, sizeof(ReplMsg), MSG_WAITALL);
    if (n <= 0 || ack.msg_type != REPL_ACK) {
        pthread_mutex_lock(&pb_mutex);
        pb_node.backup_alive = false;
        pthread_mutex_unlock(&pb_mutex);
        fprintf(stderr, "[primary] Backup no respondió ACK.\n");
    }
}

/* ─── Backup: recibir entradas de log + heartbeats del Primary ── */

void *backup_replication_thread(void *arg) {
    (void)arg;
    int fd = pb_node.primary_fd;

    while (1) {
        ReplMsg msg;
        ssize_t n = recv(fd, &msg, sizeof(ReplMsg), MSG_WAITALL);
        if (n <= 0) {
            fprintf(stderr, "[backup] Conexión con Primary perdida — failover.\n");
            handle_primary_failure();
            break;
        }

        if (msg.msg_type == REPL_LOG_ENTRY) {
            pthread_mutex_lock(&pb_mutex);
            /* Regla Lamport: L = max(L, T) + 1 */
            if (msg.entry.lamport_ts > pb_node.lamport_clock)
                pb_node.lamport_clock = msg.entry.lamport_ts;
            pb_node.lamport_clock++;
            if (pb_node.log_size < MAX_LOG_ENTRIES)
                pb_node.log[pb_node.log_size++] = msg.entry;
            pthread_mutex_unlock(&pb_mutex);

            persist_log_to_disk(&msg.entry);

            printf("[backup] Replicado Log[%u] ts=%u  %.60s\n",
                   msg.entry.index, msg.entry.lamport_ts, msg.entry.command);
            fflush(stdout);

            /* Responder ACK */
            ReplMsg ack = { .msg_type = REPL_ACK, .ack_index = msg.entry.index };
            send(fd, &ack, sizeof(ReplMsg), MSG_NOSIGNAL);

        } else if (msg.msg_type == REPL_HEARTBEAT) {
            pthread_mutex_lock(&pb_mutex);
            if (msg.hb.lamport_ts > pb_node.lamport_clock)
                pb_node.lamport_clock = msg.hb.lamport_ts;
            pb_node.lamport_clock++;
            pb_node.last_heartbeat = time(NULL);
            uint32_t my_log = (uint32_t)pb_node.log_size;
            pthread_mutex_unlock(&pb_mutex);

            printf("[backup] HB OK — Primary log=%u | Backup log=%u\n",
                   msg.hb.log_index, my_log);
            fflush(stdout);
        }
    }
    return NULL;
}

/* ─── Failover ───────────────────────────────────────────────── */

void handle_primary_failure(void) {
    if (failover_done) return;
    failover_done = true;

    printf("\n[backup] === FAILOVER INICIADO ===\n");
    printf("[backup] Lamport local: %u | Entradas de log: %d\n",
           pb_node.lamport_clock, pb_node.log_size);
    fflush(stdout);

    pb_node.role = ROLE_PRIMARY;
    trigger_failover();   /* señala al hilo main para iniciar accept loop */
}
