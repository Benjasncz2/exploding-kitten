#ifndef PB_NODE_H
#define PB_NODE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>

#define REPL_PORT        9191
#define MAX_LOG_ENTRIES  4096
#define HB_INTERVAL_S    1
#define HB_TIMEOUT_S     3

/* Tipos de mensaje en el canal de replicación TCP */
#define REPL_LOG_ENTRY   1u
#define REPL_HEARTBEAT   2u
#define REPL_ACK         3u

typedef struct {
    uint32_t lamport_ts;
    uint32_t index;
    char     command[256];
} LogEntry;

typedef struct {
    uint32_t node_id;
    uint32_t lamport_ts;
    uint32_t log_index;
} HeartbeatMsg;

/* Mensaje unificado del canal de replicación */
typedef struct {
    uint32_t msg_type;
    union {
        LogEntry     entry;
        HeartbeatMsg hb;
        uint32_t     ack_index;
    };
} ReplMsg;

typedef enum { ROLE_PRIMARY = 0, ROLE_BACKUP = 1 } NodeRole;

typedef struct {
    NodeRole role;
    int      backup_fd;      /* socket TCP al Backup  (Primary usa esto) */
    int      primary_fd;     /* socket TCP al Primary (Backup usa esto)  */
    time_t   last_heartbeat;
    bool     backup_alive;
    LogEntry log[MAX_LOG_ENTRIES];
    int      log_size;
    uint32_t lamport_clock;
    char     peer_host[64];
    char     log_file[128];
} PBNode;

extern PBNode           pb_node;
extern pthread_mutex_t  pb_mutex;

/* Implementado en server_main.c — señala al hilo main que hay failover */
extern void trigger_failover(void);

void  pb_node_init(NodeRole role, const char *peer_host);
void  append_log_entry(const char *action_json);
void  replicate_to_backup(LogEntry *entry);
void  persist_log_to_disk(const LogEntry *entry);
void *backup_replication_thread(void *arg);
void  handle_primary_failure(void);

#endif /* PB_NODE_H */
