#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <omp.h>
#include <cjson/cJSON.h>
#include "game_state.h"

/* ── Tablas de strings ───────────────────────────────────────── */

static const char *CARD_NAMES[] = {
    "RAINBOW_KITTEN", "TACO_CAT", "HAIRY_POTATO", "BEARD_CAT", "CATTERMELON",
    "ATTACK", "SKIP", "SEE_FUTURE", "ALTER_FUTURE", "SHUFFLE", "NOPE",
    "DEFUSE", "EXPLODING_KITTEN"
};

const char *gs_card_name(CardType c) {
    if (c < 0 || c >= EXPLODING_KITTEN + 1) return "UNKNOWN";
    return CARD_NAMES[(int)c];
}

CardType gs_card_from_str(const char *s) {
    if (!s) return CARD_NONE;
    for (int i = 0; i <= (int)EXPLODING_KITTEN; i++)
        if (strcasecmp(s, CARD_NAMES[i]) == 0) return (CardType)i;
    return CARD_NONE;
}

const char *gs_phase_name(GamePhase p) {
    switch (p) {
    case PHASE_LOBBY:        return "LOBBY";
    case PHASE_PLAYING:      return "PLAYING";
    case PHASE_NOPE_WINDOW:  return "NOPE_WINDOW";
    case PHASE_DEFUSE_PLACE: return "DEFUSE_PLACE";
    case PHASE_GAME_OVER:    return "GAME_OVER";
    default:                 return "UNKNOWN";
    }
}

/* ── Inicialización ──────────────────────────────────────────── */

void gs_init(GameState *gs) {
    memset(gs, 0, sizeof(GameState));
    gs->deck_top       = -1;
    gs->phase          = PHASE_LOBBY;
    gs->current_player = 0;
}

/*
 * Mazo base sin Exploding Kittens ni DEFUSEs (50 cartas).
 * Los DEFUSEs se reparten directamente en deal(); los EK se insertan después.
 */
void gs_init_deck(GameState *gs) {
    int top = 0;
    /* Cat cards: 4 de cada tipo */
    for (int c = RAINBOW_KITTEN; c <= CATTERMELON; c++)
        for (int i = 0; i < 4; i++) gs->deck[top++] = (CardType)c;
    /* Cartas de acción */
    for (int i = 0; i < 4; i++) gs->deck[top++] = ATTACK;
    for (int i = 0; i < 4; i++) gs->deck[top++] = SKIP;
    for (int i = 0; i < 5; i++) gs->deck[top++] = SEE_FUTURE;
    for (int i = 0; i < 4; i++) gs->deck[top++] = ALTER_FUTURE;
    for (int i = 0; i < 2; i++) gs->deck[top++] = SHUFFLE;
    for (int i = 0; i < 5; i++) gs->deck[top++] = NOPE;
    /* 6 DEFUSEs para repartir (1 por jugador + spare) */
    for (int i = 0; i < 6; i++) gs->deck[top++] = DEFUSE;
    gs->deck_top = top - 1;
}

void gs_shuffle(GameState *gs, uint32_t seed) {
    srand(seed);
    for (int i = gs->deck_top; i > 0; i--) {
        int j = rand() % (i + 1);
        CardType tmp  = gs->deck[i];
        gs->deck[i]   = gs->deck[j];
        gs->deck[j]   = tmp;
    }
}

/* Reparte: 1 DEFUSE + 7 cartas aleatorias por jugador */
void gs_deal(GameState *gs) {
    /* Primero dar 1 DEFUSE a cada jugador sacándolo del mazo */
    for (int p = 0; p < gs->num_players; p++) {
        for (int i = gs->deck_top; i >= 0; i--) {
            if (gs->deck[i] == DEFUSE) {
                gs->players[p].hand[gs->players[p].hand_size++] = DEFUSE;
                gs->deck[i] = gs->deck[gs->deck_top--]; /* swap con tope */
                break;
            }
        }
    }
    /* Luego 7 cartas del tope del mazo por jugador */
    for (int p = 0; p < gs->num_players; p++) {
        for (int c = 0; c < 7 && gs->deck_top >= 0; c++)
            gs->players[p].hand[gs->players[p].hand_size++] = gs->deck[gs->deck_top--];
    }
}

/* Inserta (num_players - 1) EK en posiciones aleatorias del mazo */
void gs_insert_exploding_kittens(GameState *gs) {
    int n_ek = gs->num_players - 1;
    for (int k = 0; k < n_ek; k++) {
        int pos = (gs->deck_top >= 0) ? rand() % (gs->deck_top + 2) : 0;
        gs->deck_top++;
        for (int i = gs->deck_top; i > pos; i--)
            gs->deck[i] = gs->deck[i - 1];
        gs->deck[pos] = EXPLODING_KITTEN;
    }
}

int gs_find_slot(const GameState *gs, int player_id) {
    for (int i = 0; i < gs->num_players; i++)
        if (gs->players[i].player_id == player_id) return i;
    return -1;
}

int gs_add_player(GameState *gs, int player_id, const char *name, bool is_bot) {
    if (gs->num_players >= 5) return -1;
    int slot = gs->num_players++;
    gs->players[slot].player_id = player_id;
    gs->players[slot].is_alive  = true;
    gs->players[slot].is_bot    = is_bot;
    gs->players[slot].hand_size = 0;
    gs->num_alive++;
    strncpy(gs->players[slot].name, name, sizeof(gs->players[slot].name) - 1);
    return slot;
}

void gs_start(GameState *gs, uint32_t seed) {
    gs->game_seed = seed;
    gs_init_deck(gs);
    gs_shuffle(gs, seed);
    gs_deal(gs);
    gs_insert_exploding_kittens(gs);
    gs->phase          = PHASE_PLAYING;
    gs->current_player = 0;
}

/* ── Helpers de mano y mazo ──────────────────────────────────── */

bool gs_has_card(const GameState *gs, int pid, CardType c) {
    for (int i = 0; i < gs->players[pid].hand_size; i++)
        if (gs->players[pid].hand[i] == c) return true;
    return false;
}

void gs_remove_card(GameState *gs, int pid, CardType c) {
    Player *p = &gs->players[pid];
    for (int i = 0; i < p->hand_size; i++) {
        if (p->hand[i] == c) {
            p->hand[i] = p->hand[--p->hand_size];
            return;
        }
    }
}

void gs_add_card(GameState *gs, int pid, CardType c) {
    Player *p = &gs->players[pid];
    if (p->hand_size < MAX_HAND) p->hand[p->hand_size++] = c;
}

CardType gs_draw_top(GameState *gs) {
    if (gs->deck_top < 0) return CARD_NONE;
    return gs->deck[gs->deck_top--];
}

void gs_insert_at(GameState *gs, CardType c, int pos) {
    gs->deck_top++;
    for (int i = gs->deck_top; i > pos; i--)
        gs->deck[i] = gs->deck[i - 1];
    gs->deck[pos] = c;
}

/* Avanza al siguiente jugador vivo */
void gs_advance_turn(GameState *gs) {
    if (gs->num_alive <= 1) return;
    do {
        gs->current_player = (gs->current_player + 1) % gs->num_players;
    } while (!gs->players[gs->current_player].is_alive);
    gs->future_count = 0; /* el peek de SEE_FUTURE no persiste entre turnos */
}

bool gs_is_nopeable(CardType c) {
    return c == ATTACK || c == SKIP || c == SEE_FUTURE ||
           c == ALTER_FUTURE || c == SHUFFLE;
}

/* ── Efectos de cartas ───────────────────────────────────────── */

void gs_apply_attack(GameState *gs, int pid) {
    gs->attack_count = (gs->attack_count == 0) ? 2 : gs->attack_count + 2;
    gs_advance_turn(gs);
    snprintf(gs->last_event, sizeof(gs->last_event),
             "%s jugó ATTACK — el siguiente debe robar %d veces",
             gs->players[pid].name, gs->attack_count);
}

void gs_apply_skip(GameState *gs, int pid) {
    if (gs->attack_count > 0) gs->attack_count--;
    if (gs->attack_count == 0) gs_advance_turn(gs);
    snprintf(gs->last_event, sizeof(gs->last_event),
             "%s jugó SKIP", gs->players[pid].name);
}

void gs_apply_see_future(GameState *gs, int pid) {
    gs->future_count = 0;
    for (int i = 0; i < 3 && (gs->deck_top - i) >= 0; i++)
        gs->future_peek[gs->future_count++] = gs->deck[gs->deck_top - i];
    snprintf(gs->last_event, sizeof(gs->last_event),
             "%s jugó SEE_FUTURE", gs->players[pid].name);
}

void gs_apply_alter_future(GameState *gs, int pid, CardType new_top[3]) {
    /* Auto-peek si no había un SEE_FUTURE previo */
    if (gs->future_count == 0) {
        for (int i = 0; i < 3 && (gs->deck_top - i) >= 0; i++)
            gs->future_peek[gs->future_count++] = gs->deck[gs->deck_top - i];
    }
    int count = gs->future_count;

    /* Si new_top está vacío (todos CARD_NONE), revertir el orden del peek */
    bool has_order = false;
    for (int i = 0; i < count; i++)
        if (new_top[i] != CARD_NONE) { has_order = true; break; }
    if (!has_order) {
        for (int i = 0; i < count; i++)
            new_top[i] = gs->future_peek[count - 1 - i];
    }

    for (int i = 0; i < count && (gs->deck_top - i) >= 0; i++)
        gs->deck[gs->deck_top - i] = new_top[i];
    gs->future_count = 0;
    snprintf(gs->last_event, sizeof(gs->last_event),
             "%s jugó ALTER_FUTURE", gs->players[pid].name);
}

void gs_apply_shuffle(GameState *gs, int pid) {
    uint32_t seed2 = (uint32_t)(time(NULL) ^ (gs->lamport_clock * 31337u));
    srand(seed2);
    for (int i = gs->deck_top; i > 0; i--) {
        int j = rand() % (i + 1);
        CardType tmp = gs->deck[i];
        gs->deck[i]  = gs->deck[j];
        gs->deck[j]  = tmp;
    }
    snprintf(gs->last_event, sizeof(gs->last_event),
             "%s jugó SHUFFLE", gs->players[pid].name);
}

/*
 * Roba una carta y aplica consecuencias (EK, normal).
 * Devuelve la carta robada. Si fue EK y hay DEFUSE, la reinserción la hace el
 * llamador (o game_logic.c para esperar DEFUSE_POS del humano).
 * Si fue EK sin DEFUSE, el jugador queda eliminado aquí.
 */
CardType gs_handle_draw(GameState *gs, int pid) {
    CardType drawn = gs_draw_top(gs);
    if (drawn == CARD_NONE) {
        /* Mazo vacío — no debería pasar en partida normal */
        snprintf(gs->last_event, sizeof(gs->last_event), "Mazo vacío!");
        return CARD_NONE;
    }

    if (drawn != EXPLODING_KITTEN) {
        gs_add_card(gs, pid, drawn);
        if (gs->attack_count > 0) {
            gs->attack_count--;
        }
        if (gs->attack_count == 0) {
            gs_advance_turn(gs);
        }
        snprintf(gs->last_event, sizeof(gs->last_event),
                 "%s robó una carta", gs->players[pid].name);
    }
    /* Si es EK, el llamador decide qué hacer (DEFUSE o eliminar) */
    return drawn;
}

/* ── IA de bots (OpenMP) ─────────────────────────────────────── */

static int evaluate_heuristic(const GameState *gs, int bot_pid, int card_idx) {
    CardType card     = gs->players[bot_pid].hand[card_idx];
    int      ek_ahead = 0;
    for (int i = 0; i < gs->future_count; i++)
        if (gs->future_peek[i] == EXPLODING_KITTEN) ek_ahead++;

    switch (card) {
    case ATTACK:      return ek_ahead > 0 ? 90 : 40;
    case SKIP:        return ek_ahead > 0 ? 85 : 30;
    case SHUFFLE:     return ek_ahead > 0 ? 88 : 20;
    case SEE_FUTURE:  return gs->future_count == 0 ? 70 : 8;
    case ALTER_FUTURE:return gs->future_count > 0  ? 60 : 5;
    case NOPE:        return 0;   /* bots no usan NOPE reactivamente */
    case DEFUSE:      return 0;   /* nunca jugar DEFUSE voluntariamente */
    default:          return 5;   /* cat cards: baja prioridad */
    }
}

CardType gs_bot_choose(GameState *gs, int bot_pid) {
    int n = gs->players[bot_pid].hand_size;
    if (n == 0) return CARD_NONE;

    int scores[MAX_HAND] = {0};

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; i++)
        scores[i] = evaluate_heuristic(gs, bot_pid, i);

    int best = 0;
    for (int i = 1; i < n; i++)
        if (scores[i] > scores[best]) best = i;

    /* Si el mejor puntaje es 0, mejor robar */
    if (scores[best] == 0) return CARD_NONE;
    return gs->players[bot_pid].hand[best];
}

/* ── Serialización JSON ──────────────────────────────────────── */

char *gs_serialize(const GameState *gs, uint32_t ts) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type",           "GAME_STATE");
    cJSON_AddStringToObject(root, "phase",          gs_phase_name(gs->phase));
    cJSON_AddNumberToObject(root, "current_player", gs->current_player);
    cJSON_AddNumberToObject(root, "attack_count",   gs->attack_count);
    cJSON_AddNumberToObject(root, "deck_size",      gs->deck_top + 1);
    cJSON_AddNumberToObject(root, "num_alive",      gs->num_alive);
    cJSON_AddNumberToObject(root, "ts",             (double)ts);
    cJSON_AddStringToObject(root, "event",          gs->last_event);

    cJSON *players = cJSON_CreateArray();
    for (int i = 0; i < gs->num_players; i++) {
        const Player *p = &gs->players[i];
        cJSON *pj = cJSON_CreateObject();
        cJSON_AddNumberToObject(pj, "id",        p->player_id);
        cJSON_AddStringToObject(pj, "name",      p->name);
        cJSON_AddBoolToObject(pj,   "alive",     p->is_alive);
        cJSON_AddBoolToObject(pj,   "bot",       p->is_bot);
        cJSON_AddNumberToObject(pj, "hand_size", p->hand_size);

        cJSON *hand = cJSON_CreateArray();
        for (int c = 0; c < p->hand_size; c++)
            cJSON_AddItemToArray(hand, cJSON_CreateString(gs_card_name(p->hand[c])));
        cJSON_AddItemToObject(pj, "hand", hand);
        cJSON_AddItemToArray(players, pj);
    }
    cJSON_AddItemToObject(root, "players", players);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}
