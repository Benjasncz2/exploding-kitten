#ifndef UI_CLI_H
#define UI_CLI_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#define UI_LOG_LINES    5
#define UI_MAX_HAND    20
#define UI_MAX_PLAYERS  5

typedef struct {
    char my_name[32];
    int  my_pid;          /* -1 until server assigns it via GAME_STATE */

    char     phase[32];
    int      current_player;
    int      attack_count;
    int      deck_size;
    int      num_alive;
    uint32_t lamport_ts;
    char     last_event[160];

    struct {
        int  id;
        char name[32];
        bool alive;
        bool is_bot;
        int  hand_size;
    } players[UI_MAX_PLAYERS];
    int num_players;

    char hand[UI_MAX_HAND][32];
    int  hand_size;

    /* Circular log buffer (newest first) */
    char log_lines[UI_LOG_LINES][192];
    int  log_count;

    char            status[192];
    bool            needs_redraw;
    pthread_mutex_t lock;
} UIState;

extern UIState g_ui;

void ui_init(const char *my_name);
void ui_cleanup(void);
void ui_handle_message(const char *json);  /* reader thread */
void ui_set_status(const char *msg);       /* any thread */
void ui_redraw(void);                      /* main thread only */
int  ui_getch(void);                       /* main thread; ERR on 200 ms timeout */

#endif /* UI_CLI_H */
