#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <cjson/cJSON.h>
#include "protocol.h"
#include "game_state.h"

static GameState gs;
static int       g_snapshot_fd = -1;

void game_logic_set_snapshot_fd(int fd) { g_snapshot_fd = fd; }

/* ── Envío de respuesta al server via FIFO ───────────────────── */
/*
 * Formato del mensaje al broadcast_thread:
 * { "valid": bool, "player_id": int,
 *   "action_obj": { "action": str, "pid": int, "card": str, "ts": int },
 *   "broadcast": { ... GAME_STATE ... },
 *   "private_target": int,   // -1 si ninguno
 *   "private": { ... },      // mensaje privado (SEE_FUTURE peek)
 *   "error": str }
 */
static void send_response(int fd_out,
                          bool valid, int player_id,
                          const char *action, const char *card,
                          const char *error,
                          bool include_state,
                          int private_target, cJSON *private_msg) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "valid",          valid);
    cJSON_AddNumberToObject(root, "player_id",      player_id);
    cJSON_AddNumberToObject(root, "private_target", private_target);

    if (error) {
        cJSON_AddStringToObject(root, "error", error);
    } else {
        gs.lamport_clock++;
        cJSON *aj = cJSON_CreateObject();
        cJSON_AddStringToObject(aj, "action", action ? action : "");
        cJSON_AddNumberToObject(aj, "pid",    player_id);
        if (card) cJSON_AddStringToObject(aj, "card", card);
        cJSON_AddNumberToObject(aj, "ts",     (double)gs.lamport_clock);
        cJSON_AddItemToObject(root, "action_obj", aj);

        if (include_state) {
            char *state_str = gs_serialize(&gs, gs.lamport_clock);
            cJSON *bcast    = cJSON_Parse(state_str);
            free(state_str);
            if (bcast) cJSON_AddItemToObject(root, "broadcast", bcast);
        }
    }

    if (private_msg && private_target >= 0)
        cJSON_AddItemReferenceToObject(root, "private", private_msg);

    char    *s   = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    uint32_t len = (uint32_t)strlen(s);
    WRITE(fd_out, &len, sizeof(uint32_t));
    WRITE(fd_out, s,    len);
    free(s);
}

static void send_error(int fd_out, int pid, const char *msg) {
    send_response(fd_out, false, pid, NULL, NULL, msg, false, -1, NULL);
}

static void send_state(int fd_out, int pid, const char *action, const char *card) {
    send_response(fd_out, true, pid, action, card, NULL, true, -1, NULL);
    if (g_snapshot_fd >= 0) {
        ssize_t _w = write(g_snapshot_fd, &gs, sizeof(GameState));
        (void)_w;
    }
}

/* ── Helpers internos ────────────────────────────────────────── */

/*
 * slot_to_pid: convierte índice de slot → player_id real (para ruteo de red).
 * El player_id es asignado por el servidor; el slot es la posición en gs.players[].
 * En un servidor recién iniciado coinciden, pero si hay reconexiones divergen.
 */
static int slot_to_pid(int slot) {
    if (slot >= 0 && slot < gs.num_players)
        return gs.players[slot].player_id;
    return slot; /* fallback */
}

/*
 * check_turn: verifica que sea el turno del jugador con player_id dado.
 * Busca su slot con gs_find_slot y lo compara con gs.current_player (slot).
 */
static bool check_turn(int fd_out, int player_id) {
    if (gs.phase != PHASE_PLAYING) {
        send_error(fd_out, player_id, "El juego no está en fase PLAYING");
        return false;
    }
    if (gs_find_slot(&gs, player_id) != gs.current_player) {
        send_error(fd_out, player_id, "No es tu turno");
        return false;
    }
    return true;
}

/* Aplica el efecto de una carta (ya retirada de la mano). slot = índice en gs.players[] */
static void apply_card(int fd_out, int slot, CardType card, const char *payload);
static void maybe_bot_turn(int fd_out);

/* do_draw: slot = índice en gs.players[]. Internamente resuelve player_id para red. */
static void do_draw(int fd_out, int slot) {
    int player_id = slot_to_pid(slot);
    CardType drawn = gs_handle_draw(&gs, slot);

    if (drawn == EXPLODING_KITTEN) {
        if (gs_has_card(&gs, slot, DEFUSE)) {
            gs_remove_card(&gs, slot, DEFUSE);
            gs.phase          = PHASE_DEFUSE_PLACE;
            gs.pending_player = slot; /* guardamos SLOT, no player_id */
            strncpy(gs.pending_card, "EXPLODING_KITTEN", sizeof(gs.pending_card) - 1);
            snprintf(gs.last_event, sizeof(gs.last_event),
                     "%s robó EXPLODING KITTEN y usó DEFUSE — elige posición (0-%d)",
                     gs.players[slot].name, gs.deck_top + 1);
            send_state(fd_out, player_id, "DRAW", "EXPLODING_KITTEN");
        } else {
            gs.players[slot].is_alive = false;
            gs.num_alive--;
            if (gs.num_alive <= 1) {
                gs.phase = PHASE_GAME_OVER;
                for (int i = 0; i < gs.num_players; i++) {
                    if (gs.players[i].is_alive) {
                        snprintf(gs.last_event, sizeof(gs.last_event),
                                 "¡%s ganó!", gs.players[i].name);
                        break;
                    }
                }
            } else {
                snprintf(gs.last_event, sizeof(gs.last_event),
                         "%s EXPLOTÓ 💥 (%d jugadores quedan)",
                         gs.players[slot].name, gs.num_alive);
                gs_advance_turn(&gs);
            }
            send_state(fd_out, player_id, "DRAW", "EXPLODING_KITTEN");
        }
    } else {
        send_state(fd_out, player_id, "DRAW",
                   drawn == CARD_NONE ? "NONE" : gs_card_name(drawn));
        maybe_bot_turn(fd_out);
    }
}

/* apply_pending: gs.pending_player ya es un slot (establecido así en handle_play_card) */
static void apply_pending(int fd_out) {
    gs.phase = PHASE_PLAYING;
    CardType c    = gs_card_from_str(gs.pending_card);
    int      slot = gs.pending_player; /* slot */
    apply_card(fd_out, slot, c, gs.pending_card);
}

/* apply_card: slot = índice en gs.players[]. Deriva player_id internamente para red. */
static void apply_card(int fd_out, int slot, CardType card, const char *payload) {
    int player_id = slot_to_pid(slot);
    switch (card) {
    case ATTACK:
        gs_apply_attack(&gs, slot);
        send_state(fd_out, player_id, "PLAY_CARD", "ATTACK");
        maybe_bot_turn(fd_out);
        break;

    case SKIP:
        gs_apply_skip(&gs, slot);
        send_state(fd_out, player_id, "PLAY_CARD", "SKIP");
        maybe_bot_turn(fd_out);
        break;

    case SEE_FUTURE: {
        gs_apply_see_future(&gs, slot);
        cJSON *peek = cJSON_CreateObject();
        cJSON_AddStringToObject(peek, "type", "PEEK");
        cJSON *cards = cJSON_CreateArray();
        for (int i = 0; i < gs.future_count; i++)
            cJSON_AddItemToArray(cards, cJSON_CreateString(gs_card_name(gs.future_peek[i])));
        cJSON_AddItemToObject(peek, "cards", cards);
        /* private_target usa player_id real para ruteo correcto */
        send_response(fd_out, true, player_id, "PLAY_CARD", "SEE_FUTURE",
                      NULL, true, player_id, peek);
        cJSON_Delete(peek);
        break;
    }

    case ALTER_FUTURE: {
        CardType new_top[3] = {CARD_NONE, CARD_NONE, CARD_NONE};
        if (payload && strchr(payload, ',')) {
            char buf[96]; strncpy(buf, payload, sizeof(buf) - 1);
            char *tok = strtok(buf, ",");
            for (int i = 0; i < 3 && tok; i++, tok = strtok(NULL, ","))
                new_top[i] = gs_card_from_str(tok);
        }
        gs_apply_alter_future(&gs, slot, new_top);
        send_state(fd_out, player_id, "PLAY_CARD", "ALTER_FUTURE");
        break;
    }

    case SHUFFLE:
        gs_apply_shuffle(&gs, slot);
        send_state(fd_out, player_id, "PLAY_CARD", "SHUFFLE");
        break;

    case NOPE:
        send_error(fd_out, player_id, "NOPE solo se puede durante la ventana NOPE");
        break;

    default:
        snprintf(gs.last_event, sizeof(gs.last_event),
                 "%s jugó %s (sin efecto solitario)",
                 gs.players[slot].name, gs_card_name(card));
        send_state(fd_out, player_id, "PLAY_CARD", gs_card_name(card));
        break;
    }
}

/* Bot: ejecuta su turno completo (jugar carta óptima o robar) */
static void execute_bot_turn(int fd_out) {
    int      pid    = gs.current_player;
    CardType chosen = gs_bot_choose(&gs, pid);

    struct timespec bot_delay = {0, 400000000L}; /* 0.4s */
    nanosleep(&bot_delay, NULL);

    if (chosen != CARD_NONE) {
        gs_remove_card(&gs, pid, chosen);
        apply_card(fd_out, pid, chosen, gs_card_name(chosen));
    } else {
        do_draw(fd_out, pid);
    }
}

/* Si el jugador actual es bot, auto-juega en bucle */
static void maybe_bot_turn(int fd_out) {
    while (gs.num_players > 0) {
        if (gs.phase == PHASE_PLAYING &&
            gs.players[gs.current_player].is_bot) {
            execute_bot_turn(fd_out);

        } else if (gs.phase == PHASE_DEFUSE_PLACE &&
                   gs.pending_player >= 0 &&
                   gs.pending_player < gs.num_players &&
                   gs.players[gs.pending_player].is_bot) {
            /* Bot coloca el EK en posición aleatoria inmediatamente */
            int bot = gs.pending_player;
            int pos = (gs.deck_top >= 0) ? rand() % (gs.deck_top + 2) : 0;
            gs_insert_at(&gs, EXPLODING_KITTEN, pos);
            gs.phase = PHASE_PLAYING;
            if (gs.attack_count == 0) gs_advance_turn(&gs);
            else { gs.attack_count--; if (gs.attack_count == 0) gs_advance_turn(&gs); }
            snprintf(gs.last_event, sizeof(gs.last_event),
                     "%s (bot) usó DEFUSE, EK colocado en posición %d",
                     gs.players[bot].name, pos);
            send_state(fd_out, slot_to_pid(bot), "DEFUSE_POS", NULL);
        } else {
            break;
        }
    }
}

/* ── Manejadores de acciones entrantes ───────────────────────── */

static void handle_join(const Action *a, int fd_out) {
    const char *name = a->payload[0] ? a->payload : "Anon";

    /* Reconexión en cualquier fase: buscar slot por nombre primero */
    for (int i = 0; i < gs.num_players; i++) {
        if (strcmp(gs.players[i].name, name) == 0) {
            gs.players[i].player_id = a->player_id;
            snprintf(gs.last_event, sizeof(gs.last_event),
                     "%.30s reconectado", name);
            printf("[game_logic] Reconexion: '%s' → player_id=%d (slot=%d)\n",
                   name, a->player_id, i);
            fflush(stdout);
            send_state(fd_out, a->player_id, "JOIN", NULL);
            return;
        }
    }

    /* Nombre no existe aún — solo se admiten nuevos jugadores en LOBBY */
    if (gs.phase != PHASE_LOBBY) {
        send_error(fd_out, a->player_id, "La partida ya comenzó");
        return;
    }
    bool is_bot = (strncasecmp(name, "bot", 3) == 0);
    if (gs_add_player(&gs, a->player_id, name, is_bot) < 0) {
        send_error(fd_out, a->player_id, "Sala llena");
        return;
    }
    snprintf(gs.last_event, sizeof(gs.last_event),
             "%.30s se unió (%d de 5)", name, gs.num_players);
    send_state(fd_out, a->player_id, "JOIN", NULL);
}

static void handle_start(const Action *a, int fd_out) {
    if (gs.phase != PHASE_LOBBY) {
        send_error(fd_out, a->player_id, "La partida ya comenzó");
        return;
    }
    if (gs.num_players < 2) {
        send_error(fd_out, a->player_id, "Se necesitan al menos 2 jugadores");
        return;
    }
    uint32_t seed = (uint32_t)time(NULL) ^ (uint32_t)getpid();
    gs_start(&gs, seed);
    snprintf(gs.last_event, sizeof(gs.last_event),
             "Partida iniciada — %d jugadores, seed=%u", gs.num_players, seed);
    printf("[game_logic] Partida iniciada. Seed=%u, %d jugadores, %d cartas en mazo.\n",
           seed, gs.num_players, gs.deck_top + 1);
    fflush(stdout);
    send_state(fd_out, a->player_id, "START", NULL);
    maybe_bot_turn(fd_out);
}

static void handle_play_card(const Action *a, int fd_out) {
    int pid  = a->player_id;
    int slot = gs_find_slot(&gs, pid);
    if (!check_turn(fd_out, pid)) return;

    CardType card = gs_card_from_str(a->payload);
    if (card == CARD_NONE) { send_error(fd_out, pid, "Carta desconocida"); return; }
    if (!gs_has_card(&gs, slot, card)) { send_error(fd_out, pid, "No tienes esa carta"); return; }
    if (card == DEFUSE) { send_error(fd_out, pid, "No puedes jugar DEFUSE voluntariamente"); return; }
    if (card == EXPLODING_KITTEN) { send_error(fd_out, pid, "No puedes jugar EXPLODING KITTEN"); return; }

    gs_remove_card(&gs, slot, card);

    if (gs_is_nopeable(card)) {
        gs.phase = PHASE_NOPE_WINDOW;
        snprintf(gs.pending_card, sizeof(gs.pending_card), "%s", gs_card_name(card));
        gs.pending_player = slot; /* guardamos slot */
        snprintf(gs.last_event, sizeof(gs.last_event),
                 "%s jugó %s — ventana NOPE abierta (2s)", gs.players[slot].name, gs_card_name(card));
        send_state(fd_out, pid, "PLAY_CARD", gs_card_name(card));
    } else {
        apply_card(fd_out, slot, card, a->payload);
    }
}

static void handle_draw(const Action *a, int fd_out) {
    if (!check_turn(fd_out, a->player_id)) return;
    do_draw(fd_out, gs_find_slot(&gs, a->player_id));
}

static void handle_nope(const Action *a, int fd_out) {
    int pid  = a->player_id;
    int slot = gs_find_slot(&gs, pid);
    if (gs.phase != PHASE_NOPE_WINDOW) {
        send_error(fd_out, pid, "No hay ventana NOPE activa");
        return;
    }
    if (!gs_has_card(&gs, slot, NOPE)) {
        send_error(fd_out, pid, "No tienes NOPE");
        return;
    }
    gs_remove_card(&gs, slot, NOPE);
    gs.phase = PHASE_PLAYING;
    snprintf(gs.last_event, sizeof(gs.last_event),
             "%s jugó NOPE — %s cancelado", gs.players[slot].name, gs.pending_card);
    send_state(fd_out, pid, "NOPE", "NOPE");
    maybe_bot_turn(fd_out);
}

static void handle_defuse_pos(const Action *a, int fd_out) {
    int pid  = a->player_id;
    int slot = gs_find_slot(&gs, pid);
    if (gs.phase != PHASE_DEFUSE_PLACE || gs.pending_player != slot) {
        send_error(fd_out, pid, "No estás en fase DEFUSE");
        return;
    }
    int pos = atoi(a->payload);
    int max  = gs.deck_top + 1;
    if (pos < 0 || pos > max) pos = rand() % (max + 1);

    gs_insert_at(&gs, EXPLODING_KITTEN, pos);
    gs.phase = PHASE_PLAYING;
    if (gs.attack_count == 0) gs_advance_turn(&gs);
    else { gs.attack_count--; if (gs.attack_count == 0) gs_advance_turn(&gs); }

    snprintf(gs.last_event, sizeof(gs.last_event),
             "%s insertó EK en posición %d (de %d)", gs.players[slot].name, pos, gs.deck_top);
    send_state(fd_out, pid, "DEFUSE_POS", NULL);
    maybe_bot_turn(fd_out);
}

static void handle_alter_order(const Action *a, int fd_out) {
    int pid  = a->player_id;
    int slot = gs_find_slot(&gs, pid);
    if (!check_turn(fd_out, pid)) return;
    if (!gs_has_card(&gs, slot, ALTER_FUTURE)) {
        send_error(fd_out, pid, "No tienes ALTER_FUTURE"); return;
    }
    gs_remove_card(&gs, slot, ALTER_FUTURE);
    apply_card(fd_out, slot, ALTER_FUTURE, a->payload);
}

static void handle_play_pair(const Action *a, int fd_out) {
    int pid  = a->player_id;
    int slot = gs_find_slot(&gs, pid);
    if (!check_turn(fd_out, pid)) return;

    /* payload: "CARD_NAME TARGET_SLOT" (target_slot 0-based) */
    char buf[64];
    strncpy(buf, a->payload, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *sp = strchr(buf, ' ');
    if (!sp) { send_error(fd_out, pid, "Formato: CARTA N°OBJETIVO"); return; }
    *sp = '\0';
    int target_slot = atoi(sp + 1);

    CardType card = gs_card_from_str(buf);
    if (card < RAINBOW_KITTEN || card > CATTERMELON) {
        send_error(fd_out, pid, "Solo cat cards se pueden jugar en pares"); return;
    }

    /* Verificar que tiene al menos 2 de esa carta */
    int count = 0;
    for (int i = 0; i < gs.players[slot].hand_size; i++)
        if (gs.players[slot].hand[i] == card) count++;
    if (count < 2) {
        send_error(fd_out, pid, "Necesitas 2 de esa carta para jugar par"); return;
    }

    /* Verificar target: vivo, distinto del jugador actual, tiene cartas */
    if (target_slot < 0 || target_slot >= gs.num_players ||
        !gs.players[target_slot].is_alive || target_slot == slot) {
        send_error(fd_out, pid, "Objetivo invalido"); return;
    }
    if (gs.players[target_slot].hand_size == 0) {
        send_error(fd_out, pid, "El objetivo no tiene cartas"); return;
    }

    /* Quitar 2 cartas del jugador */
    gs_remove_card(&gs, slot, card);
    gs_remove_card(&gs, slot, card);

    /* Robar carta aleatoria del objetivo */
    Player *tgt     = &gs.players[target_slot];
    int     ridx    = rand() % tgt->hand_size;
    CardType stolen = tgt->hand[ridx];
    tgt->hand[ridx] = tgt->hand[--tgt->hand_size]; /* swap & remove */
    gs_add_card(&gs, slot, stolen);

    snprintf(gs.last_event, sizeof(gs.last_event),
             "%s jugó par de %s y robó una carta a %s",
             gs.players[slot].name, gs_card_name(card), gs.players[target_slot].name);
    send_state(fd_out, pid, "PLAY_PAIR", gs_card_name(card));
    maybe_bot_turn(fd_out);
}

/* ── Timeouts ────────────────────────────────────────────────── */

static void handle_timeout(int fd_out) {
    switch (gs.phase) {
    case PHASE_NOPE_WINDOW:
        printf("[game_logic] Timeout NOPE — aplicando %s de pid=%d\n",
               gs.pending_card, gs.pending_player);
        fflush(stdout);
        apply_pending(fd_out);
        break;

    case PHASE_DEFUSE_PLACE: {
        int slot = gs.pending_player; /* slot */
        int pid  = slot_to_pid(slot);
        int pos  = (gs.deck_top >= 0) ? rand() % (gs.deck_top + 2) : 0;
        gs_insert_at(&gs, EXPLODING_KITTEN, pos);
        gs.phase = PHASE_PLAYING;
        gs_advance_turn(&gs);
        snprintf(gs.last_event, sizeof(gs.last_event),
                 "%s no eligió posición — EK insertado en %d (auto)", gs.players[slot].name, pos);
        send_state(fd_out, pid, "DEFUSE_POS", NULL);
        maybe_bot_turn(fd_out);
        break;
    }
    case PHASE_PLAYING:
        printf("[game_logic] Timeout turno — auto DRAW para pid=%d\n", gs.current_player);
        fflush(stdout);
        do_draw(fd_out, gs.current_player);
        break;

    default:
        break;
    }
}

/* ── Bucle principal ─────────────────────────────────────────── */

void game_logic_loop(void) {
    int fd_in  = open(FIFO_NET_TO_GAME, O_RDONLY);
    int fd_out = open(FIFO_GAME_TO_NET, O_WRONLY);
    if (fd_in < 0 || fd_out < 0) { perror("game_logic: open FIFOs"); _exit(1); }

    gs_init(&gs);

    bool restored = false;
    int snap_fd = open("/tmp/ek_snapshot.bin", O_RDONLY);
    if (snap_fd >= 0) {
        ssize_t n = read(snap_fd, &gs, sizeof(GameState));
        close(snap_fd);
        if (n == (ssize_t)sizeof(GameState)) {
            restored = true;
            unlink("/tmp/ek_snapshot.bin");
        } else {
            gs_init(&gs);
        }
    }

    printf("[game_logic pid=%d] Motor iniciado. %s\n", getpid(),
           restored ? "Partida restaurada desde snapshot." : "Esperando jugadores...");
    fflush(stdout);

    /*
     * deadline almacena el momento en que vence el timeout de la fase actual.
     * Se actualiza UNA SOLA VEZ al entrar a cada fase (cuando prev_phase cambia).
     * Los mensajes inválidos que llegan durante la fase NO resetean el timer —
     * esto evita que la NOPE_WINDOW quede bloqueada si el cliente envía acciones.
     */
    GamePhase      prev_phase = PHASE_LOBBY;
    struct timeval deadline   = {0, 0};

    while (1) {
        /* Si la fase cambió, calcular la nueva deadline */
        if (gs.phase != prev_phase) {
            prev_phase = gs.phase;
            gettimeofday(&deadline, NULL);
            if      (gs.phase == PHASE_NOPE_WINDOW)  deadline.tv_sec += 2;
            else if (gs.phase == PHASE_DEFUSE_PLACE) deadline.tv_sec += 30;
            else if (gs.phase == PHASE_PLAYING)      deadline.tv_sec += 60;
        }

        struct timeval tv, now, *tvp = NULL;
        if (gs.phase == PHASE_NOPE_WINDOW  ||
            gs.phase == PHASE_DEFUSE_PLACE ||
            gs.phase == PHASE_PLAYING) {
            gettimeofday(&now, NULL);
            /* ¿Ya pasó la deadline? */
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_usec >= deadline.tv_usec)) {
                handle_timeout(fd_out);
                prev_phase = PHASE_LOBBY; /* fuerza recálculo de deadline */
                continue;
            }
            /* Tiempo restante = deadline - now */
            tv.tv_sec  = deadline.tv_sec  - now.tv_sec;
            tv.tv_usec = deadline.tv_usec - now.tv_usec;
            if (tv.tv_usec < 0) { tv.tv_sec--; tv.tv_usec += 1000000; }
            tvp = &tv;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_in, &fds);

        int ready = select(fd_in + 1, &fds, NULL, NULL, tvp);

        if (ready == 0) {
            handle_timeout(fd_out);
            prev_phase = PHASE_LOBBY; /* fuerza recálculo de deadline */
            continue;
        }
        if (ready < 0) break; /* señal u error fatal */

        Action action;
        ssize_t n = read(fd_in, &action, sizeof(Action));
        if (n <= 0) break;

        printf("[game_logic] pid=%-2d type=%-12s payload=%.30s\n",
               action.player_id, action.type, action.payload);
        fflush(stdout);

        if      (strcmp(action.type, "JOIN")        == 0) handle_join(&action, fd_out);
        else if (strcmp(action.type, "START")       == 0) handle_start(&action, fd_out);
        else if (strcmp(action.type, "PLAY_CARD")   == 0) handle_play_card(&action, fd_out);
        else if (strcmp(action.type, "DRAW")        == 0) handle_draw(&action, fd_out);
        else if (strcmp(action.type, "NOPE")        == 0) handle_nope(&action, fd_out);
        else if (strcmp(action.type, "DEFUSE_POS")  == 0) handle_defuse_pos(&action, fd_out);
        else if (strcmp(action.type, "ALTER_ORDER") == 0) handle_alter_order(&action, fd_out);
        else if (strcmp(action.type, "PLAY_PAIR")   == 0) handle_play_pair(&action, fd_out);
        else send_error(fd_out, action.player_id, "Tipo de acción desconocido");
    }

    close(fd_in);
    close(fd_out);
}
