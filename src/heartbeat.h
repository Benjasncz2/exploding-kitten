#ifndef HEARTBEAT_H
#define HEARTBEAT_H

/* Hilo del Primary: envía HeartbeatMsg al Backup cada HB_INTERVAL_S segundos */
void *heartbeat_sender(void *arg);

/* Hilo del Backup: monitorea el timestamp del último heartbeat recibido */
void *heartbeat_monitor(void *arg);

#endif /* HEARTBEAT_H */
