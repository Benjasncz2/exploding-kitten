#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cjson/cJSON.h>
#include <ncurses.h>
#include "net_handler.h"
#include "ui_cli.h"

static int         g_fd      = -1;
static const char *g_host;
static int         g_port;
static uint32_t    g_last_ts = 0;

/* Declaración anticipada */
static void send_json(cJSON *obj);

static void ms_sleep(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int do_connect(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(g_port),
    };
    inet_pton(AF_INET, g_host, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
    close(fd);
    return -1;
}

static void reconnect(void) {
    ui_set_status("Servidor caído. Reconectando con backoff exponencial...");
    int delay_ms = 500;
    for (int i = 0; i < 20; i++) {
        ms_sleep(delay_ms);
        int fd = do_connect();
        if (fd > 0) {
            g_fd = fd;
            /* Reenviar JOIN con el último Lamport conocido */
            cJSON *join = cJSON_CreateObject();
            cJSON_AddStringToObject(join, "type",        "JOIN");
            cJSON_AddStringToObject(join, "player_name", g_ui.my_name);
            cJSON_AddNumberToObject(join, "ts",          (double)g_last_ts);
            send_json(join);
            char buf[64];
            snprintf(buf, sizeof(buf), "Reconectado tras %d intento(s).", i + 1);
            ui_set_status(buf);
            return;
        }
        char buf[80];
        snprintf(buf, sizeof(buf), "Reintento %d/20 en %d ms...", i + 1, delay_ms);
        ui_set_status(buf);
        if (delay_ms < 8000) delay_ms *= 2;
    }
    ui_cleanup();
    fprintf(stderr, "No se pudo reconectar. Saliendo.\n");
    exit(1);
}

/* Hilo lector: recibe mensajes del servidor y actualiza la UI */
static void *reader_thread(void *arg) {
    (void)arg;
    while (1) {
        char *msg = recv_message(g_fd);
        if (!msg) { reconnect(); continue; }

        /* Actualizar Lamport local */
        cJSON *j = cJSON_Parse(msg);
        if (j) {
            cJSON *ts_j = cJSON_GetObjectItem(j, "ts");
            if (ts_j && ts_j->valuedouble > g_last_ts)
                g_last_ts = (uint32_t)ts_j->valuedouble;
            cJSON_Delete(j);
        }

        ui_handle_message(msg);
        free(msg);
    }
    return NULL;
}

static void send_json(cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    send_message(g_fd, s);
    free(s);
    cJSON_Delete(obj);
}

/*
 * Construye y envía un mensaje de acción al servidor.
 * type:    "JOIN", "START", "PLAY_CARD", "DRAW", "NOPE", "DEFUSE_POS"
 * payload: contenido del campo "payload" (NULL si no aplica)
 * name:    campo "player_name" usado solo en JOIN
 */
static void send_action(const char *type, const char *payload) {
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "type", type);
    if (payload && payload[0])
        cJSON_AddStringToObject(a, "payload", payload);
    cJSON_AddNumberToObject(a, "ts", (double)g_last_ts);
    send_json(a);
}

int main(int argc, char *argv[]) {
    g_host = argc > 1 ? argv[1] : "127.0.0.1";
    g_port = argc > 2 ? atoi(argv[2]) : 9090;
    const char *name = argc > 3 ? argv[3] : "Jugador1";

    g_fd = do_connect();
    if (g_fd < 0) { perror("connect"); return 1; }

    ui_init(name);
    ui_redraw();

    /* Enviar JOIN */
    cJSON *join = cJSON_CreateObject();
    cJSON_AddStringToObject(join, "type",        "JOIN");
    cJSON_AddStringToObject(join, "player_name", name);
    cJSON_AddNumberToObject(join, "ts",          0);
    send_json(join);

    /* Hilo lector */
    pthread_t rtid;
    pthread_create(&rtid, NULL, reader_thread, NULL);
    pthread_detach(rtid);

    /* Buffer de entrada para posición DEFUSE (puede ser >9, ej. "12") */
    char input_buf[16] = "";
    int  input_len     = 0;
    int  idle_ticks    = 0;    /* contador para PING keepalive */

    /* Selección de carta con buffer (soporta N°>9) y modo par */
    char card_buf[8]   = "";
    int  card_buf_len  = 0;
    char pair_buf[32]  = "";
    int  pair_len      = 0;
    bool pair_mode     = false;

    /* Bucle principal ncurses */
    while (1) {
        /* Redibujar si el hilo lector actualizó el estado */
        pthread_mutex_lock(&g_ui.lock);
        bool rd = g_ui.needs_redraw;
        pthread_mutex_unlock(&g_ui.lock);
        if (rd) ui_redraw();

        int ch = ui_getch();   /* retorna ERR cada 200 ms sin input */
        if (ch == ERR) {
            /* Enviar PING cada ~20 s para mantener vivo el socket (SO_RCVTIMEO=120s) */
            if (++idle_ticks >= 100) {
                idle_ticks = 0;
                send_action("PING", NULL);
            }
            continue;
        }
        idle_ticks = 0;

        /* Salir siempre */
        if (ch == 'q' || ch == 'Q') break;

        /* Leer fase actual con mutex */
        pthread_mutex_lock(&g_ui.lock);
        char cur_phase[32];
        snprintf(cur_phase, sizeof(cur_phase), "%s", g_ui.phase);
        int  hand_size = g_ui.hand_size;
        pthread_mutex_unlock(&g_ui.lock);

        /* ── LOBBY ─────────────────────────────────────────────── */
        if (strcmp(cur_phase, "LOBBY") == 0) {
            if (ch == 's' || ch == 'S') {
                send_action("START", NULL);
                ui_set_status("Solicitando inicio de partida...");
            }

        /* ── DEFUSE_PLACE: ingreso de posición numérica ─────────── */
        } else if (strcmp(cur_phase, "DEFUSE_PLACE") == 0) {
            if (ch >= '0' && ch <= '9') {
                if (input_len < (int)sizeof(input_buf) - 1) {
                    input_buf[input_len++] = (char)ch;
                    input_buf[input_len]   = '\0';
                    char buf[64];
                    snprintf(buf, sizeof(buf),
                             "Posicion: %s  (Enter para confirmar)", input_buf);
                    ui_set_status(buf);
                }
            } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                if (input_len > 0) input_buf[--input_len] = '\0';
                char buf[48];
                snprintf(buf, sizeof(buf), "Posicion: %s", input_buf);
                ui_set_status(buf);
            } else if (ch == '\n' || ch == KEY_ENTER) {
                if (input_len > 0) {
                    send_action("DEFUSE_POS", input_buf);
                    input_buf[0] = '\0';
                    input_len    = 0;
                    ui_set_status("DEFUSE_POS enviado.");
                }
            }

        /* ── NOPE_WINDOW: solo se permite jugar NOPE ───────────── */
        } else if (strcmp(cur_phase, "NOPE_WINDOW") == 0) {
            if (ch == 'n' || ch == 'N') {
                send_action("NOPE", NULL);
                ui_set_status("Jugando NOPE...");
            }
            /* Ignorar cualquier otra tecla para no resetear el timer */

        /* ── PLAYING ────────────────────────────────────────────── */
        } else {

            /* — Modo par: [P] → "N°carta N°objetivo" + Enter — */
            if (pair_mode) {
                if ((ch >= '0' && ch <= '9') || ch == ' ') {
                    if (pair_len < (int)sizeof(pair_buf) - 1) {
                        pair_buf[pair_len++] = (char)ch;
                        pair_buf[pair_len]   = '\0';
                        char sbuf[96];
                        snprintf(sbuf, sizeof(sbuf),
                                 "Par: \"%s\"  Enter=confirmar  Esc=cancelar", pair_buf);
                        ui_set_status(sbuf);
                    }
                } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                    if (pair_len > 0) pair_buf[--pair_len] = '\0';
                    char sbuf[64];
                    snprintf(sbuf, sizeof(sbuf), "Par: \"%s\"", pair_buf);
                    ui_set_status(sbuf);
                } else if (ch == '\n' || ch == KEY_ENTER) {
                    int card_n = 0, target_n = 0;
                    if (sscanf(pair_buf, "%d %d", &card_n, &target_n) == 2) {
                        pthread_mutex_lock(&g_ui.lock);
                        char card_name[32] = "";
                        int cidx = card_n - 1;
                        if (cidx >= 0 && cidx < hand_size)
                            strncpy(card_name, g_ui.hand[cidx], sizeof(card_name) - 1);
                        pthread_mutex_unlock(&g_ui.lock);
                        if (card_name[0]) {
                            char payload[64];
                            snprintf(payload, sizeof(payload), "%s %d",
                                     card_name, target_n - 1);
                            send_action("PLAY_PAIR", payload);
                            char sbuf[80];
                            snprintf(sbuf, sizeof(sbuf),
                                     "Par de %s → jugador %d enviado.", card_name, target_n);
                            ui_set_status(sbuf);
                        } else {
                            ui_set_status("Numero de carta invalido.");
                        }
                    } else {
                        ui_set_status("Formato: N°carta N°objetivo  (ej: 3 2)");
                    }
                    pair_mode   = false;
                    pair_buf[0] = '\0';
                    pair_len    = 0;
                } else if (ch == 27) { /* ESC */
                    pair_mode   = false;
                    pair_buf[0] = '\0';
                    pair_len    = 0;
                    ui_set_status("Par cancelado.");
                }

            /* — Selección de carta: dígitos acumulados + Enter — */
            } else if (ch >= '0' && ch <= '9') {
                if (card_buf_len < (int)sizeof(card_buf) - 1) {
                    card_buf[card_buf_len++] = (char)ch;
                    card_buf[card_buf_len]   = '\0';
                    char sbuf[80];
                    snprintf(sbuf, sizeof(sbuf),
                             "Carta: %s  Enter=jugar  Bksp=borrar  Esc=cancelar", card_buf);
                    ui_set_status(sbuf);
                }
            } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                if (card_buf_len > 0) card_buf[--card_buf_len] = '\0';
                char sbuf[48];
                snprintf(sbuf, sizeof(sbuf), "Carta: %s", card_buf);
                ui_set_status(sbuf);
            } else if (ch == '\n' || ch == KEY_ENTER) {
                if (card_buf_len > 0) {
                    int idx = atoi(card_buf) - 1;
                    card_buf[0]  = '\0';
                    card_buf_len = 0;
                    pthread_mutex_lock(&g_ui.lock);
                    char card_name[32] = "";
                    if (idx >= 0 && idx < hand_size)
                        strncpy(card_name, g_ui.hand[idx], sizeof(card_name) - 1);
                    pthread_mutex_unlock(&g_ui.lock);
                    if (card_name[0]) {
                        send_action("PLAY_CARD", card_name);
                        char sbuf[64];
                        snprintf(sbuf, sizeof(sbuf), "Jugando %s...", card_name);
                        ui_set_status(sbuf);
                    } else {
                        ui_set_status("Numero de carta invalido.");
                    }
                }
            } else if (ch == 27) { /* ESC: cancelar selección */
                card_buf[0]  = '\0';
                card_buf_len = 0;
                ui_set_status("Seleccion cancelada.");

            } else if (ch == 'p' || ch == 'P') {
                /* Entrar en modo par — mostrar jugadores disponibles */
                pthread_mutex_lock(&g_ui.lock);
                char hint[160] = "Par: N°carta N°objetivo";
                int np = g_ui.num_players;
                for (int i = 0; i < np; i++) {
                    if (!g_ui.players[i].alive) continue;
                    if (g_ui.players[i].id == g_ui.my_pid) continue;
                    char seg[40];
                    snprintf(seg, sizeof(seg), "  [%d]%s", i + 1, g_ui.players[i].name);
                    strncat(hint, seg, sizeof(hint) - strlen(hint) - 1);
                }
                pthread_mutex_unlock(&g_ui.lock);
                strncat(hint, "  Esc=cancelar", sizeof(hint) - strlen(hint) - 1);
                ui_set_status(hint);
                pair_mode   = true;
                pair_buf[0] = '\0';
                pair_len    = 0;

            } else if (ch == 'd' || ch == 'D') {
                card_buf[0]  = '\0';
                card_buf_len = 0;
                send_action("DRAW", NULL);
                ui_set_status("Robando carta...");

            } else if (ch == 'n' || ch == 'N') {
                send_action("NOPE", NULL);
                ui_set_status("Jugando NOPE...");
            }
        }
    }

    ui_cleanup();
    close(g_fd);
    return 0;
}
