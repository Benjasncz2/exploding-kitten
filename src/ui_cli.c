#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include <cjson/cJSON.h>
#include "ui_cli.h"

UIState g_ui;

/* ── Internal helpers ────────────────────────────────────────── */

static void add_log(const char *line) {
    for (int i = UI_LOG_LINES - 1; i > 0; i--)
        strncpy(g_ui.log_lines[i], g_ui.log_lines[i - 1],
                sizeof(g_ui.log_lines[0]) - 1);
    strncpy(g_ui.log_lines[0], line, sizeof(g_ui.log_lines[0]) - 1);
    if (g_ui.log_count < UI_LOG_LINES) g_ui.log_count++;
}

/* ── Public API ──────────────────────────────────────────────── */

void ui_init(const char *my_name) {
    memset(&g_ui, 0, sizeof(g_ui));
    strncpy(g_ui.my_name, my_name, sizeof(g_ui.my_name) - 1);
    g_ui.my_pid = -1;
    pthread_mutex_init(&g_ui.lock, NULL);
    strncpy(g_ui.phase, "LOBBY", sizeof(g_ui.phase) - 1);
    snprintf(g_ui.status, sizeof(g_ui.status),
             "Conectado como '%s'. Esperando jugadores...", my_name);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    halfdelay(2);   /* getch() devuelve ERR tras 200 ms si no hay input */
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_YELLOW, -1);            /* título */
        init_pair(2, COLOR_GREEN,  -1);            /* vivo / ok */
        init_pair(3, COLOR_RED,    -1);            /* muerto / error */
        init_pair(4, COLOR_CYAN,   -1);            /* cartas de mano */
        init_pair(5, COLOR_BLACK, COLOR_YELLOW);   /* es mi turno */
    }
}

void ui_cleanup(void) {
    endwin();
    pthread_mutex_destroy(&g_ui.lock);
}

/*
 * Llamado desde el hilo lector. Parsea el JSON del servidor y actualiza
 * UIState. No toca ncurses — solo establece needs_redraw = true.
 */
void ui_handle_message(const char *json) {
    cJSON *msg = cJSON_Parse(json);
    if (!msg) return;

    cJSON *type_j = cJSON_GetObjectItem(msg, "type");
    const char *type = (type_j && type_j->valuestring) ? type_j->valuestring : "";

    pthread_mutex_lock(&g_ui.lock);

    if (strcmp(type, "GAME_STATE") == 0) {
        cJSON *j;
        if ((j = cJSON_GetObjectItem(msg, "phase")) && j->valuestring)
            strncpy(g_ui.phase, j->valuestring, sizeof(g_ui.phase) - 1);
        if ((j = cJSON_GetObjectItem(msg, "current_player")))
            g_ui.current_player = (int)j->valuedouble;
        if ((j = cJSON_GetObjectItem(msg, "attack_count")))
            g_ui.attack_count = (int)j->valuedouble;
        if ((j = cJSON_GetObjectItem(msg, "deck_size")))
            g_ui.deck_size = (int)j->valuedouble;
        if ((j = cJSON_GetObjectItem(msg, "num_alive")))
            g_ui.num_alive = (int)j->valuedouble;
        if ((j = cJSON_GetObjectItem(msg, "ts")))
            g_ui.lamport_ts = (uint32_t)j->valuedouble;
        if ((j = cJSON_GetObjectItem(msg, "event")) && j->valuestring) {
            strncpy(g_ui.last_event, j->valuestring, sizeof(g_ui.last_event) - 1);
            add_log(j->valuestring);
        }

        cJSON *players_j = cJSON_GetObjectItem(msg, "players");
        if (cJSON_IsArray(players_j)) {
            int np = cJSON_GetArraySize(players_j);
            g_ui.num_players = np < UI_MAX_PLAYERS ? np : UI_MAX_PLAYERS;
            g_ui.hand_size   = 0;

            for (int i = 0; i < g_ui.num_players; i++) {
                cJSON *p  = cJSON_GetArrayItem(players_j, i);
                cJSON *id  = cJSON_GetObjectItem(p, "id");
                cJSON *nm  = cJSON_GetObjectItem(p, "name");
                cJSON *al  = cJSON_GetObjectItem(p, "alive");
                cJSON *bt  = cJSON_GetObjectItem(p, "bot");
                cJSON *hs  = cJSON_GetObjectItem(p, "hand_size");
                cJSON *hd  = cJSON_GetObjectItem(p, "hand");

                if (id) g_ui.players[i].id       = (int)id->valuedouble;
                if (nm && nm->valuestring)
                    strncpy(g_ui.players[i].name, nm->valuestring, 31);
                g_ui.players[i].alive    = cJSON_IsTrue(al);
                g_ui.players[i].is_bot   = cJSON_IsTrue(bt);
                if (hs) g_ui.players[i].hand_size = (int)hs->valuedouble;

                /* Actualizar mi pid por nombre en cada estado (soporta reconexión) */
                if (nm && nm->valuestring &&
                    strcmp(nm->valuestring, g_ui.my_name) == 0) {
                    g_ui.my_pid = g_ui.players[i].id;
                }

                /* Extraer mi mano (comparar DESPUÉS de actualizar my_pid) */
                if (g_ui.players[i].id == g_ui.my_pid && cJSON_IsArray(hd)) {
                    int n = cJSON_GetArraySize(hd);
                    g_ui.hand_size = n < UI_MAX_HAND ? n : UI_MAX_HAND;
                    for (int c = 0; c < g_ui.hand_size; c++) {
                        cJSON *card = cJSON_GetArrayItem(hd, c);
                        if (card && card->valuestring)
                            strncpy(g_ui.hand[c], card->valuestring, 31);
                    }
                }
            }
        }

    } else if (strcmp(type, "ERROR") == 0) {
        cJSON *m = cJSON_GetObjectItem(msg, "msg");
        if (m && m->valuestring)
            snprintf(g_ui.status, sizeof(g_ui.status), "ERROR: %s", m->valuestring);

    } else if (strcmp(type, "PEEK") == 0) {
        /* SEE_FUTURE: las 3 cartas del tope del mazo */
        cJSON *cards = cJSON_GetObjectItem(msg, "cards");
        char buf[192] = "PEEK: ";
        if (cJSON_IsArray(cards)) {
            int n = cJSON_GetArraySize(cards);
            for (int i = 0; i < n; i++) {
                cJSON *c = cJSON_GetArrayItem(cards, i);
                if (i > 0) strncat(buf, " > ", sizeof(buf) - strlen(buf) - 1);
                if (c && c->valuestring)
                    strncat(buf, c->valuestring, sizeof(buf) - strlen(buf) - 1);
            }
        }
        strncpy(g_ui.status, buf, sizeof(g_ui.status));
        g_ui.status[sizeof(g_ui.status) - 1] = '\0';
        add_log(buf);
    }

    g_ui.needs_redraw = true;
    pthread_mutex_unlock(&g_ui.lock);
    cJSON_Delete(msg);
}

void ui_set_status(const char *msg) {
    pthread_mutex_lock(&g_ui.lock);
    strncpy(g_ui.status, msg, sizeof(g_ui.status) - 1);
    g_ui.needs_redraw = true;
    pthread_mutex_unlock(&g_ui.lock);
}

/*
 * Dibuja toda la pantalla. Solo debe llamarse desde el hilo principal.
 * Toma el mutex internamente para leer UIState de forma segura.
 */
void ui_redraw(void) {
    pthread_mutex_lock(&g_ui.lock);

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (cols < 40 || rows < 10) { pthread_mutex_unlock(&g_ui.lock); return; }
    clear();

    int row = 0;

    /* ── Barra de título ─────────────────────────────────────── */
    char title[128];
    snprintf(title, sizeof(title),
             " EXPLODING KITTENS   [%s]   Lamport: %u ",
             g_ui.phase, g_ui.lamport_ts);
    attron(COLOR_PAIR(1) | A_BOLD | A_REVERSE);
    mvprintw(row, 0, "%-*s", cols, title);
    attroff(COLOR_PAIR(1) | A_BOLD | A_REVERSE);
    row++;

    /* ── Info de partida ─────────────────────────────────────── */
    /* current_player es un slot index (0..n-1), NO un player_id */
    const char *cur_name = "?";
    if (g_ui.current_player >= 0 && g_ui.current_player < g_ui.num_players)
        cur_name = g_ui.players[g_ui.current_player].name;

    bool my_turn = (g_ui.my_pid >= 0 &&
                    g_ui.current_player >= 0 &&
                    g_ui.current_player < g_ui.num_players &&
                    g_ui.players[g_ui.current_player].id == g_ui.my_pid &&
                    strcmp(g_ui.phase, "PLAYING") == 0);

    if (my_turn) attron(COLOR_PAIR(5) | A_BOLD);
    mvprintw(row, 0, " Turno: %-12s  Mazo: %2d  Ataques: %d  Vivos: %d/%d",
             cur_name, g_ui.deck_size, g_ui.attack_count,
             g_ui.num_alive, g_ui.num_players);
    if (my_turn) attroff(COLOR_PAIR(5) | A_BOLD);
    row++;

    /* Badges de jugadores */
    int cx = 1;
    for (int i = 0; i < g_ui.num_players && cx < cols - 18; i++) {
        char seg[48];
        char marker = (g_ui.players[i].id == g_ui.my_pid) ? '*'
                    : g_ui.players[i].is_bot               ? '~' : ' ';
        snprintf(seg, sizeof(seg), "[%c%s:%c] ",
                 marker, g_ui.players[i].name,
                 g_ui.players[i].alive ? '+' : 'x');
        if (g_ui.players[i].alive) attron(COLOR_PAIR(2));
        else                        attron(COLOR_PAIR(3));
        mvprintw(row, cx, "%s", seg);
        attroff(COLOR_PAIR(2) | COLOR_PAIR(3));
        cx += (int)strlen(seg);
    }
    row++;

    mvhline(row, 0, ACS_HLINE, cols); row++;

    /* ── Mano del jugador ────────────────────────────────────── */
    mvprintw(row, 0, " Mano (pid=%d, %d cartas):", g_ui.my_pid, g_ui.hand_size);
    row++;

    int card_w    = 20;
    int per_row   = (cols - 2) / card_w;
    if (per_row < 1) per_row = 1;

    cx = 1;
    int hand_rows_start = row;
    for (int i = 0; i < g_ui.hand_size && row < rows - 5; i++) {
        if (i > 0 && i % per_row == 0) { row++; cx = 1; }
        attron(COLOR_PAIR(4) | A_BOLD);
        mvprintw(row, cx, "[%d]%-17s", i + 1, g_ui.hand[i]);
        attroff(COLOR_PAIR(4) | A_BOLD);
        cx += card_w;
    }
    if (g_ui.hand_size == 0) mvprintw(hand_rows_start, 1, "(vacía)");
    row++;

    if (row < rows - 4) { mvhline(row, 0, ACS_HLINE, cols); row++; }

    /* ── Controles ───────────────────────────────────────────── */
    if (row < rows - 3) {
        attron(A_DIM);
        if (strcmp(g_ui.phase, "LOBBY") == 0) {
            mvprintw(row, 0, " [S]Iniciar partida   [Q]Salir");
        } else if (strcmp(g_ui.phase, "DEFUSE_PLACE") == 0) {
            mvprintw(row, 0,
                     " DEFUSE: escribe posicion [0-%d] + Enter  [Q]Salir",
                     g_ui.deck_size);
        } else if (strcmp(g_ui.phase, "NOPE_WINDOW") == 0) {
            mvprintw(row, 0, " [N]Jugar NOPE   [Q]Salir   (ventana 2s)");
        } else {
            mvprintw(row, 0,
                     " [N°]+Enter=jugar   [P]Par(2x)   [D]Robar   [N]Nope   [Q]Salir");
        }
        attroff(A_DIM);
        row++;
    }

    /* ── Último evento ───────────────────────────────────────── */
    if (row < rows - 2) {
        attron(A_BOLD);
        mvprintw(row, 0, " >> %-*s", cols - 5, g_ui.last_event);
        attroff(A_BOLD);
        row++;
    }

    if (row < rows - 1) { mvhline(row, 0, ACS_HLINE, cols); row++; }

    /* ── Log de eventos ──────────────────────────────────────── */
    for (int i = 0; i < g_ui.log_count && row < rows - 1; i++, row++)
        mvprintw(row, 2, "%s", g_ui.log_lines[i]);

    /* ── Barra de estado (última fila) ───────────────────────── */
    attron(COLOR_PAIR(3) | A_REVERSE);
    mvprintw(rows - 1, 0, "%-*s", cols, g_ui.status);
    attroff(COLOR_PAIR(3) | A_REVERSE);

    g_ui.needs_redraw = false;
    pthread_mutex_unlock(&g_ui.lock);
    refresh();
}

int ui_getch(void) {
    return getch();  /* halfdelay(2): ERR si no hay input en 200 ms */
}
