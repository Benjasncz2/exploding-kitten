#ifndef GAME_STATE_H
#define GAME_STATE_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_HAND     20
#define MAX_DECK     64   /* base deck + EKs */

typedef enum {
    RAINBOW_KITTEN = 0, TACO_CAT, HAIRY_POTATO, BEARD_CAT, CATTERMELON,
    ATTACK, SKIP, SEE_FUTURE, ALTER_FUTURE, SHUFFLE, NOPE,
    DEFUSE, EXPLODING_KITTEN,
    CARD_NONE = -1
} CardType;

typedef enum {
    PHASE_LOBBY,         /* esperando jugadores / START */
    PHASE_PLAYING,       /* turno normal */
    PHASE_NOPE_WINDOW,   /* 2s para que alguien juegue NOPE */
    PHASE_DEFUSE_PLACE,  /* 30s para colocar el EK */
    PHASE_GAME_OVER
} GamePhase;

typedef struct {
    int      player_id;
    CardType hand[MAX_HAND];
    int      hand_size;
    bool     is_alive;
    bool     is_bot;
    char     name[32];
} Player;

typedef struct {
    CardType  deck[MAX_DECK];
    int       deck_top;          /* índice de la cima (-1 = vacío) */
    Player    players[5];
    int       num_players;
    int       num_alive;
    int       current_player;
    int       attack_count;      /* turnos pendientes bajo ATTACK */
    CardType  future_peek[3];    /* cima visible después de SEE_FUTURE */
    int       future_count;
    GamePhase phase;
    uint32_t  lamport_clock;
    uint32_t  game_seed;

    /* Acción pendiente durante NOPE_WINDOW */
    char     pending_card[32];
    int      pending_player;

    /* Último evento para mostrar en el cliente */
    char     last_event[160];
} GameState;

/* ── Búsqueda por player_id ──────────────────────────────────── */
int      gs_find_slot(const GameState *gs, int player_id); /* slot o -1 */

/* ── Inicialización ──────────────────────────────────────────── */
void     gs_init(GameState *gs);
void     gs_init_deck(GameState *gs);
void     gs_shuffle(GameState *gs, uint32_t seed);
void     gs_deal(GameState *gs);
void     gs_insert_exploding_kittens(GameState *gs);
int      gs_add_player(GameState *gs, int player_id, const char *name, bool is_bot);
void     gs_start(GameState *gs, uint32_t seed);

/* ── Helpers de mano y mazo ──────────────────────────────────── */
bool     gs_has_card(const GameState *gs, int pid, CardType c);
void     gs_remove_card(GameState *gs, int pid, CardType c);
void     gs_add_card(GameState *gs, int pid, CardType c);
CardType gs_draw_top(GameState *gs);
void     gs_insert_at(GameState *gs, CardType c, int pos);
void     gs_advance_turn(GameState *gs);
bool     gs_is_nopeable(CardType c);

/* ── Cartas ──────────────────────────────────────────────────── */
void     gs_apply_attack(GameState *gs, int pid);
void     gs_apply_skip(GameState *gs, int pid);
void     gs_apply_see_future(GameState *gs, int pid);
void     gs_apply_alter_future(GameState *gs, int pid, CardType new_top[3]);
void     gs_apply_shuffle(GameState *gs, int pid);

/* ── Dibujar carta (devuelve la carta robada) ────────────────── */
CardType gs_handle_draw(GameState *gs, int pid);

/* ── IA de bots ──────────────────────────────────────────────── */
CardType gs_bot_choose(GameState *gs, int bot_pid);  /* OpenMP interno */

/* ── Serialización ───────────────────────────────────────────── */
char       *gs_serialize(const GameState *gs, uint32_t ts); /* caller frees */
const char *gs_card_name(CardType c);
CardType    gs_card_from_str(const char *s);
const char *gs_phase_name(GamePhase p);

#endif /* GAME_STATE_H */
