#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include "pb_node.h"
#include "heartbeat.h"

void *heartbeat_sender(void *arg) {
    (void)arg;
    printf("[primary] Heartbeat sender iniciado (cada %ds).\n", HB_INTERVAL_S);
    fflush(stdout);

    while (pb_node.role == ROLE_PRIMARY) {
        sleep(HB_INTERVAL_S);

        pthread_mutex_lock(&pb_mutex);
        pb_node.lamport_clock++;
        HeartbeatMsg hb = {
            .node_id    = 0,
            .lamport_ts = pb_node.lamport_clock,
            .log_index  = (uint32_t)pb_node.log_size,
        };
        bool alive = pb_node.backup_alive;
        int  bfd   = pb_node.backup_fd;
        pthread_mutex_unlock(&pb_mutex);

        if (!alive || bfd < 0) continue;

        ReplMsg msg = { .msg_type = REPL_HEARTBEAT, .hb = hb };
        if (send(bfd, &msg, sizeof(ReplMsg), MSG_NOSIGNAL) <= 0) {
            pthread_mutex_lock(&pb_mutex);
            pb_node.backup_alive = false;
            pthread_mutex_unlock(&pb_mutex);
            fprintf(stderr, "[primary] Backup DOWN (heartbeat falló)\n");
        }
    }
    return NULL;
}

void *heartbeat_monitor(void *arg) {
    (void)arg;
    printf("[backup] Heartbeat monitor iniciado (timeout=%ds).\n", HB_TIMEOUT_S);
    fflush(stdout);

    while (pb_node.role == ROLE_BACKUP) {
        sleep(HB_INTERVAL_S);

        pthread_mutex_lock(&pb_mutex);
        time_t elapsed = time(NULL) - pb_node.last_heartbeat;
        pthread_mutex_unlock(&pb_mutex);

        if (elapsed > HB_TIMEOUT_S) {
            fprintf(stderr, "[backup] Primary DOWN (sin heartbeat por %lds)\n", elapsed);
            handle_primary_failure();
            break;
        }
    }
    return NULL;
}
