#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#define SERVER_PORT          9090
#define MAX_PLAYERS          5
#define MAX_MSG_LEN          8192

#define FIFO_NET_TO_GAME     "/tmp/ek_net_to_game"
#define FIFO_GAME_TO_NET     "/tmp/ek_game_to_net"

/*
 * Acción que viaja por los FIFOs de servidor → game_logic.
 * sizeof(Action) = 244 bytes < PIPE_BUF (4096), garantiza escrituras atómicas.
 */
typedef struct {
    int      player_id;
    uint32_t lamport_ts;
    char     type[16];      /* "JOIN", "ACTION", "DRAW", "HB", ... */
    char     payload[220];  /* JSON adicional, e.g. nombre de carta */
} Action;

/* Slot por cliente en la tabla compartida del servidor */
typedef struct {
    int  fd;
    int  player_id;
    char name[32];
    bool active;
} ClientSlot;

/* Suprime warn_unused_result en write() sin perder la llamada */
#define WRITE(fd, buf, n)  do { ssize_t _w = write((fd),(buf),(n)); (void)_w; } while(0)

#endif /* PROTOCOL_H */
