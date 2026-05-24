# Plan de Desarrollo: Exploding Kittens Distribuido
## Sistema Multijugador en C/UNIX con Primary-Backup en AWS EC2

**Destinatario:** Claude Opus (generación de código)  
**Stack:** C · Sockets BSD · Pthreads · Named Pipes · OpenMP · AWS EC2  
**Patrón de HA:** Primary-Backup con Heartbeats + Log de Eventos (Lamport)  
**Modelo de Consistencia:** Strong Consistency (servidor Primary como única fuente de verdad)

---

## Decisión de Arquitectura: Por Qué Primary-Backup y No Raft

El propio material del curso lo establece: Raft completo requiere ~2000-5000 líneas de C bien diseñadas y es equivalente a un proyecto de tesis. Para este alcance, se aplican las **ideas centrales de Raft** sin implementar el protocolo completo:

| Concepto de Raft | Lo que implementamos |
|---|---|
| Leader Election (compleja, con votación) | ❌ No implementado — el Primary está fijo en la configuración |
| Log Replication con `AppendEntries` | ✅ Simplificado: Primary envía cada entrada de log al Backup vía TCP |
| Heartbeats para detección de fallos | ✅ Implementado completo |
| Servidor como autoridad única | ✅ El Primary es la única fuente de verdad |
| Timestamps ordenados (Lamport) | ✅ Implementado para ordenar el log de eventos |

**Trade-off aceptado:** Si el Primary cae, el Backup toma el control pero el failover es **manual o semi-automático** (un script o señal), no automático por votación. El downtime esperado es de 5-15 segundos. Para un juego de carta universitario con 2-5 jugadores, esto es completamente aceptable.

---

## Mapa de Tecnologías por Componente

| Tecnología | Componente | Justificación |
|---|---|---|
| **TCP Sockets** | Clientes ↔ Primary; Primary ↔ Backup (replication channel) | Comunicación fiable; el canal de replicación necesita garantía de entrega para mantener el log en sync |
| **Pthreads + Mutex/Condvar** | Servidor: hilo por cliente, hilo heartbeat, hilo de replicación | Concurrencia interna sin bloquear la lógica del juego; sincronización del `GameState` compartido |
| **Named Pipes (FIFOs)** | Servidor: proceso `net_handler` ↔ proceso `game_logic` | Desacopla red y lógica en procesos separados; el FIFO actúa como cola de acciones validadas |
| **OpenMP** | Servidor: broadcast paralelo a clientes, IA de bots, barajado masivo | Paraleliza trabajo CPU-intensivo aprovechando los vCPU de EC2 |

---

## Arquitectura en AWS EC2

```
AWS EC2 — VPC privada (10.0.1.0/24)
──────────────────────────────────────────────────────────────────
  ec2-primary (10.0.1.5)   ← PRIMARY: acepta clientes, procesa acciones
  ec2-backup  (10.0.1.6)   ← BACKUP:  replica en caliente, listo para failover

Puerto de juego  (clientes):  9090/TCP  → Solo el PRIMARY
Puerto replicación (P → B):   9191/TCP  → Solo entre las 2 instancias EC2
Puerto heartbeat:             9192/UDP  → Bidireccional (detección de fallos)

Security Groups:
  - 9090/TCP: abierto al mundo (jugadores)
  - 9191/TCP, 9192/UDP: solo entre 10.0.1.5 y 10.0.1.6
  - 22/TCP: solo IP del desarrollador

Elastic IP apunta siempre al PRIMARY activo.
Si hay failover, el script de promoción reasigna la Elastic IP al Backup.
```

---

## Estructuras de Datos Globales (C)

```c
/* === PROTOCOLO DE MENSAJES CLIENTE-SERVIDOR === */
#define MSG_JOIN        0x01
#define MSG_ACTION      0x02
#define MSG_GAME_STATE  0x03
#define MSG_HEARTBEAT   0x04
#define MSG_ACK         0x05
#define MSG_ERROR       0x06

/* === LOG DE EVENTOS CON TIMESTAMP LAMPORT === */
typedef struct {
    uint32_t lamport_ts;     /* Reloj lógico de Lamport en el momento del evento */
    uint32_t index;          /* Índice secuencial del evento (1, 2, 3...) */
    char     command[256];   /* JSON: {"action":"PLAY_CARD","pid":0,"card":"ATTACK"} */
} LogEntry;

/* === ESTADO DEL JUEGO === */
typedef enum {
    RAINBOW_KITTEN, TACO_CAT, HAIRY_POTATO, BEARD_CAT, CATTERMELON,
    ATTACK, SKIP, SEE_FUTURE, ALTER_FUTURE, SHUFFLE, NOPE,
    DEFUSE, EXPLODING_KITTEN
} CardType;

typedef struct {
    int      player_id;
    CardType hand[20];
    int      hand_size;
    bool     is_alive;
    char     name[32];
} Player;

typedef struct {
    CardType  deck[56];
    int       deck_top;
    Player    players[5];
    int       num_players;
    int       num_alive;
    int       current_player;
    int       attack_count;
    CardType  future_peek[3];
    int       future_count;
    uint32_t  lamport_clock;   /* Reloj lógico global del servidor */
    uint32_t  last_log_index;  /* Último índice de log enviado al Backup */
} GameState;

/* === NODO PRIMARY-BACKUP === */
typedef enum { ROLE_PRIMARY, ROLE_BACKUP } NodeRole;

typedef struct {
    NodeRole  role;
    int       backup_fd;        /* Socket TCP hacia el Backup (si soy Primary) */
    int       primary_fd;       /* Socket TCP desde el Primary (si soy Backup) */
    time_t    last_heartbeat;   /* Último heartbeat recibido del Primary */
    bool      backup_alive;     /* ¿Está el Backup respondiendo? */
    LogEntry  log[4096];        /* Log persistido en ambos nodos */
    int       log_size;
    uint32_t  lamport_clock;
} PBNode;
```

---

# FASE 1: Arquitectura de Red (Semanas 1–2)

## Objetivos
- Comunicación TCP funcional entre clientes y servidor Primary.
- Protocolo de mensajes JSON sobre TCP con serialización cJSON.
- Servidor multi-hilo: un hilo dedicado por cliente conectado.
- Named Pipes para desacoplar red y lógica de juego en procesos separados.

## Componentes y Funciones UNIX

### 1.1 Servidor Primary Multi-Hilo

```
server_main.c
├── main()
│   ├── signal(SIGPIPE, SIG_IGN)            /* evitar crash al escribir a cliente caído */
│   ├── socket() + bind(9090) + listen()
│   ├── mkfifo("/tmp/ek_net_to_game")        /* acción cliente → lógica */
│   ├── mkfifo("/tmp/ek_game_to_net")        /* estado → red */
│   ├── fork() → proceso game_logic          /* motor de juego separado */
│   └── Loop: accept() → pthread_create(&client_thread)
│
├── client_thread(void *arg)
│   ├── setsockopt(SO_RCVTIMEO, 30s)         /* timeout de turno */
│   ├── recv_message()                        /* lee Message del socket */
│   ├── actualiza lamport_clock: L = max(L, msg.ts) + 1
│   ├── escribe acción al FIFO net_to_game
│   └── lee respuesta del FIFO game_to_net y hace send() al cliente
│
└── broadcast_thread()
    ├── recibe estado desde game_to_net
    └── #pragma omp parallel for → send_message(client_fds[i], &state_msg)
```

### 1.2 Proceso de Lógica del Juego (vía FIFOs)

```c
/* game_logic.c — proceso separado */
void game_logic_loop() {
    int fd_in  = open("/tmp/ek_net_to_game", O_RDONLY);
    int fd_out = open("/tmp/ek_game_to_net", O_WRONLY);

    while (1) {
        Action action;
        read(fd_in, &action, sizeof(Action));  /* bloquea hasta nueva acción */

        pthread_mutex_lock(&state_mutex);
        if (!is_valid_action(&action, &game_state)) {
            send_error(action.player_id, "Invalid action");
        } else {
            apply_action(&action, &game_state);
            game_state.lamport_clock++;        /* Regla Lamport: evento local */
            append_log_entry(&pb_node, &action);
        }
        pthread_mutex_unlock(&state_mutex);

        /* Serializar y enviar al hilo de broadcast */
        char *json = serialize_state(&game_state);
        write(fd_out, json, strlen(json) + 1);
        free(json);
    }
}
```

### 1.3 Protocolo de Mensajes

```
/* Todos los mensajes incluyen el timestamp Lamport del emisor */

CLIENT → PRIMARY:
  {"type":"JOIN","player_name":"Alice","ts":0}
  {"type":"ACTION","action":"PLAY_CARD","card":"ATTACK","ts":5}
  {"type":"DRAW","ts":8}
  {"type":"HB","ts":10}         ← heartbeat del cliente (grace period)

PRIMARY → CLIENT:
  {"type":"GAME_STATE","state":{...},"ts":9}
  {"type":"ERROR","msg":"Not your turn","ts":9}
  {"type":"ACK","seq":5,"ts":9}
  {"type":"FAILOVER","new_host":"10.0.1.6","port":9090}
```

### 1.4 OpenMP en Broadcast

```c
void broadcast_state(GameState *gs, int *client_fds, int n) {
    char *json = serialize_state(gs);       /* serialización única, fuera del parallel */
    int   ts   = gs->lamport_clock;

    #pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < n; i++) {
        if (client_fds[i] > 0)
            send_message(client_fds[i], MSG_GAME_STATE, json, ts);
    }
    free(json);
}
```

## Cuellos de Botella y Mitigaciones

| Riesgo | Mitigación |
|---|---|
| **Race condition en `game_state`** | FIFO serializa acciones naturalmente (PIPE_BUF atómico ≤ 4096B); `state_mutex` protege las modificaciones |
| **Hilo bloqueado en `recv()`** | `SO_RCVTIMEO = 30s` + `signal(SIGPIPE, SIG_IGN)` en `main()` |
| **False sharing en broadcast OpenMP** | El `json` es puntero de solo lectura compartido; los `client_fds[i]` son independientes por índice |
| **FIFO saturado** | `fcntl(F_SETPIPE_SZ, 65536)` al crear el FIFO; backpressure bloqueando `accept()` si FIFO supera 80% |

---

# FASE 2: Primary-Backup con Heartbeats y Log Lamport (Semanas 3–4)

## Objetivos
- Implementar canal de replicación TCP entre Primary y Backup.
- Replicar cada entrada de log (con timestamp Lamport) al Backup en tiempo real.
- Detectar caída del Primary vía heartbeats con timeout adaptativo.
- Ejecutar failover semi-automático: Backup se promueve a Primary.
- Implementar reconexión de clientes ante failover.

## 2.1 Canal de Replicación Primary → Backup

El Primary envía cada `LogEntry` al Backup **inmediatamente después de aplicarla** al estado local. El Backup la escribe a su log y responde ACK.

```c
/* === PRIMARY: replicar entrada al Backup === */
void replicate_to_backup(PBNode *node, LogEntry *entry) {
    if (!node->backup_alive) return;   /* Backup caído: continuar sin replicar */

    /* Regla Lamport al enviar: L++ antes de enviar */
    node->lamport_clock++;
    entry->lamport_ts = node->lamport_clock;

    ssize_t n = send(node->backup_fd, entry, sizeof(LogEntry), MSG_NOSIGNAL);
    if (n <= 0) {
        node->backup_alive = false;
        log_warn("Backup unreachable, continuing as standalone Primary");
        return;
    }

    /* Esperar ACK del Backup (con timeout de 500ms) */
    LogEntry ack;
    setsockopt(node->backup_fd, SOL_SOCKET, SO_RCVTIMEO, &(struct timeval){0, 500000}, sizeof(struct timeval));
    recv(node->backup_fd, &ack, sizeof(LogEntry), 0);
}

/* === BACKUP: recibir y persistir entradas === */
void *backup_replication_thread(void *arg) {
    PBNode *node = (PBNode *)arg;
    while (1) {
        LogEntry entry;
        ssize_t n = recv(node->primary_fd, &entry, sizeof(LogEntry), 0);
        if (n <= 0) {
            /* Primary desapareció — iniciar failover */
            handle_primary_failure(node);
            break;
        }
        /* Regla Lamport al recibir: L = max(L, T) + 1 */
        node->lamport_clock = MAX(node->lamport_clock, entry.lamport_ts) + 1;

        /* Persistir en log local */
        node->log[node->log_size++] = entry;
        persist_log_to_disk(&entry);    /* fsync() para durabilidad */

        /* Responder ACK */
        send(node->primary_fd, &entry, sizeof(LogEntry), 0);
    }
    return NULL;
}
```

## 2.2 Log de Eventos con Timestamps Lamport

Cada acción del juego genera una entrada en el log. El log es la fuente de verdad para reconstruir el estado:

```
Log compartido (Primary y Backup lo tienen idéntico):
┌────────┬────────────┬──────────────────────────────────────────────────┐
│ index  │ lamport_ts │ command (JSON)                                   │
├────────┼────────────┼──────────────────────────────────────────────────┤
│   1    │     1      │ {"action":"START","seed":42,"players":["A","B"]} │
│   2    │     3      │ {"action":"PLAY_CARD","pid":0,"card":"ATTACK"}   │
│   3    │     5      │ {"action":"DRAW","pid":1}                        │
│   4    │     7      │ {"action":"EXPLODE","pid":1}                     │
│   5    │     9      │ {"action":"DEFUSE","pid":1,"pos":3}              │
│   6    │    11      │ {"action":"SKIP","pid":0}                        │
└────────┴────────────┴──────────────────────────────────────────────────┘

Datos críticos contenidos en el log (todo reconstructible):
  ✅ Semilla del barajado inicial (entrada #1 → determina el mazo completo)
  ✅ Cada carta jugada y por quién
  ✅ Cada carta robada (con su posición en el mazo)
  ✅ Eliminaciones de jugadores
  ✅ Posiciones de Exploding Kittens re-insertadas tras Defuse
  ✅ Órdenes de Alter the Future
```

```c
void append_log_entry(PBNode *node, Action *action) {
    node->lamport_clock++;   /* Regla Lamport: evento local */

    LogEntry entry = {
        .lamport_ts = node->lamport_clock,
        .index      = node->log_size + 1,
    };
    snprintf(entry.command, sizeof(entry.command), "%s", serialize_action(action));

    node->log[node->log_size++] = entry;
    persist_log_to_disk(&entry);   /* fsync() antes de continuar */

    replicate_to_backup(node, &entry);
}
```

## 2.3 Heartbeats — Detección de Fallos

```c
/* === PRIMARY: enviar heartbeat al Backup cada 1 segundo === */
void *heartbeat_sender(void *arg) {
    PBNode *node = (PBNode *)arg;
    while (node->role == ROLE_PRIMARY) {
        node->lamport_clock++;
        HeartbeatMsg hb = {
            .node_id      = 0,          /* ID del Primary */
            .lamport_ts   = node->lamport_clock,
            .log_index    = node->log_size,
        };
        if (send(node->backup_fd, &hb, sizeof(hb), MSG_NOSIGNAL) <= 0) {
            node->backup_alive = false;
            log_warn("Backup DOWN (heartbeat send failed)");
        }
        sleep(1);
    }
    return NULL;
}

/* === BACKUP: monitorear heartbeats del Primary === */
void *heartbeat_monitor(void *arg) {
    PBNode *node = (PBNode *)arg;
    while (node->role == ROLE_BACKUP) {
        time_t elapsed = time(NULL) - node->last_heartbeat;

        if (elapsed > 3) {          /* 3 segundos sin heartbeat → Primary caído */
            log_warn("Primary DOWN (timeout: %lds)", elapsed);
            handle_primary_failure(node);
            break;
        }
        sleep(1);
    }
    return NULL;
}

/* === BACKUP: recibir heartbeats del Primary === */
void on_heartbeat_received(PBNode *node, HeartbeatMsg *hb) {
    /* Regla Lamport al recibir: L = max(L, T) + 1 */
    node->lamport_clock = MAX(node->lamport_clock, hb->lamport_ts) + 1;
    node->last_heartbeat = time(NULL);
    log_info("Heartbeat OK — Primary log_index: %d, my log_index: %d",
             hb->log_index, node->log_size);
}
```

### Timeout Adaptativo

```c
#define ALPHA 0.1

typedef struct {
    double avg_latency;
    double variance;
} NetworkStats;

void update_timeout(NetworkStats *stats, double measured_ms) {
    stats->avg_latency = ALPHA * measured_ms + (1 - ALPHA) * stats->avg_latency;
    double dev = fabs(measured_ms - stats->avg_latency);
    stats->variance = ALPHA * dev + (1 - ALPHA) * stats->variance;
    /* Timeout = media + 4σ — cubre 99.99% de las variaciones normales */
    double timeout = stats->avg_latency + 4 * stats->variance;
    /* Aplicar como SO_RCVTIMEO en el socket de heartbeat */
    set_heartbeat_timeout((int)timeout);
}
```

## 2.4 Failover: Backup se Promueve a Primary

```c
void handle_primary_failure(PBNode *node) {
    log_info("=== FAILOVER INICIADO ===");
    log_info("Último Lamport conocido: %u", node->lamport_clock);
    log_info("Entradas de log disponibles: %d", node->log_size);

    /* 1. Reconstruir GameState desde el log (log replay) */
    reset_game_state(&game_state);
    for (int i = 0; i < node->log_size; i++) {
        apply_log_entry(&game_state, &node->log[i]);
    }
    log_info("GameState reconstruido desde %d entradas de log", node->log_size);

    /* 2. Promover a Primary */
    node->role = ROLE_PRIMARY;

    /* 3. Abrir socket de clientes en puerto 9090 */
    start_client_listener(9090);

    /* 4. Notificar a los clientes conectados (vía broadcast en la nueva IP) */
    /* Los clientes deben intentar reconectarse a la nueva IP del Backup promovido */
    log_info("Backup promovido a Primary. Escuchando en puerto 9090.");

    /* 5. Reasignar Elastic IP (ejecutar script AWS CLI) */
    system("./scripts/reassign_eip.sh 10.0.1.6");
}
```

### Script de Reasignación de Elastic IP

```bash
#!/bin/bash
# reassign_eip.sh — ejecutado por el Backup al promover
NEW_IP=$1
EIP_ALLOC="eipalloc-xxxxxxxxx"   # ID de la Elastic IP del proyecto

aws ec2 associate-address \
    --instance-id $(curl -s http://169.254.169.254/latest/meta-data/instance-id) \
    --allocation-id $EIP_ALLOC \
    --allow-reassociation

echo "Elastic IP reasignada a $NEW_IP"
```

## 2.5 Reconexión de Clientes

```c
/* Cliente: detecta desconexión y entra en bucle de reconexión */
void reconnect_loop(const char *hosts[], int n_hosts, int port, uint32_t last_ts) {
    int delay_ms = 500;
    for (int attempt = 0; attempt < 20; attempt++) {
        for (int i = 0; i < n_hosts; i++) {
            int fd = try_connect(hosts[i], port);
            if (fd > 0) {
                /* Enviar último timestamp Lamport conocido para que el servidor
                   sepa desde qué punto el cliente necesita ser actualizado */
                send_reconnect(fd, last_ts);
                log_info("Reconectado a %s tras %d intento(s)", hosts[i], attempt + 1);
                return;
            }
        }
        log_info("Reintento %d en %dms...", attempt + 1, delay_ms);
        usleep(delay_ms * 1000);
        delay_ms = MIN(delay_ms * 2, 8000);   /* backoff exponencial, máx 8s */
    }
    fprintf(stderr, "No se pudo reconectar. La partida terminó.\n");
    exit(1);
}

/* Servidor (nuevo Primary): manejar reconexión de cliente */
void handle_client_reconnect(int client_fd, uint32_t client_last_ts) {
    /* Enviar todas las entradas de log posteriores al último timestamp del cliente */
    for (int i = 0; i < pb_node.log_size; i++) {
        if (pb_node.log[i].lamport_ts > client_last_ts) {
            send_log_entry(client_fd, &pb_node.log[i]);
        }
    }
    /* Luego enviar el GameState completo actual */
    send_full_state(client_fd, &game_state);
}
```

## Cuellos de Botella y Mitigaciones

| Riesgo | Descripción | Mitigación |
|---|---|---|
| **Split-Brain** | Ambos nodos creen ser Primary simultáneamente | Con solo 2 nodos es imposible usar quórum. Mitigación práctica: el Backup espera 5s antes de promover, dando tiempo al Primary real a recuperarse. En producción usaría un tercer nodo como árbitro (o el servicio AWS Route53 Health Check como tiebreaker) |
| **Log desincronizado** | El Backup tiene menos entradas que el Primary en el momento del fallo | El Primary incluye `log_index` en cada heartbeat; el Backup detecta el gap y solicita las entradas faltantes antes de promover |
| **Race en `state_mutex` vs replicación** | El hilo de replicación lee el log mientras el hilo de juego escribe | El hilo de replicación sólo lee `log[]` por índice; el hilo de juego sólo agrega al final. El índice `log_size` se actualiza con `__atomic_fetch_add` para evitar el mutex en la mayoría de los casos |
| **Timeout mal calibrado** | Heartbeat muy corto → falsos positivos en EC2 bajo carga | Timeout inicial = 3s; refinado con media móvil exponencial (α=0.1). Heartbeat cada 1s deja 3 pérdidas antes de declarar fallo |

---

# FASE 3: Lógica del Juego y Concurrencia (Semanas 5–6)

## Objetivos
- Motor completo de Exploding Kittens: todas las cartas requeridas.
- Toda modificación de `GameState` atómica y seguida de entrada al log Lamport.
- Timeouts de turno y bots con OpenMP.

## Flujo Principal del Juego

```
INICIO:
  init_deck()                   → mazo base sin Exploding Kittens
  seed = generate_seed()        → semilla aleatoria
  shuffle_deck(seed)            → Fisher-Yates con la semilla
  append_log("START", seed)     → PRIMERA entrada del log (todo se reconstruye desde aquí)
  deal_cards()                  → 7 cartas + 1 Defuse por jugador
  insert_exploding_kittens()    → (N_jugadores - 1) cartas insertadas aleatoriamente

TURNO (loop):
  notify_current_player()
  wait_for_action(60s)          → pthread_cond_timedwait; si expira → SKIP automático

  if PLAY_CARD:
      validate → check_nope_window(2s) → apply → log → replicate → broadcast

  if DRAW:
      card = deck[deck_top--]
      if EXPLODING_KITTEN:
          if has_defuse: prompt_placement(30s) → insert_at_position → log
          else:          eliminate_player → log → check_winner
      else:
          add_to_hand → log → advance_turn
```

## Lógica de Cartas

### Exploding Kitten + Defuse
```c
void handle_exploding_kitten(GameState *gs, int pid) {
    pthread_mutex_lock(&state_mutex);
    if (player_has_card(gs, pid, DEFUSE)) {
        remove_card(gs, pid, DEFUSE);
        gs->phase = PHASE_DEFUSE_PLACEMENT;
        pthread_mutex_unlock(&state_mutex);
        /* Esperar posición del jugador (timeout 30s) */
        int pos = wait_for_defuse_position(pid, 30);
        if (pos < 0) pos = rand() % (gs->deck_top + 1);  /* timeout → pos aleatoria */
        insert_card_at(gs->deck, &gs->deck_top, EXPLODING_KITTEN, pos);
        append_log_entry(&pb_node, make_action("DEFUSE", pid, pos));
    } else {
        gs->players[pid].is_alive = false;
        gs->num_alive--;
        append_log_entry(&pb_node, make_action("ELIMINATED", pid, -1));
        if (gs->num_alive == 1) end_game(gs);
        pthread_mutex_unlock(&state_mutex);
    }
}
```

### Attack
```c
void handle_attack(GameState *gs, int pid) {
    /* El jugador actual NO roba. El siguiente debe tomar (attack_count) turnos */
    gs->attack_count = (gs->attack_count == 0) ? 2 : gs->attack_count + 2;
    advance_turn(gs);
    append_log_entry(&pb_node, make_action("ATTACK", pid, gs->attack_count));
}
```

### Skip
```c
void handle_skip(GameState *gs, int pid) {
    /* Cancela UN turno pendiente de Attack, o el turno actual */
    if (gs->attack_count > 0) gs->attack_count--;
    advance_turn(gs);
    append_log_entry(&pb_node, make_action("SKIP", pid, -1));
}
```

### See the Future
```c
void handle_see_future(GameState *gs, int pid) {
    int count = MIN(3, gs->deck_top);
    for (int i = 0; i < count; i++)
        gs->future_peek[i] = gs->deck[gs->deck_top - 1 - i];
    gs->future_count = count;
    /* Enviar SOLO al jugador que jugó la carta (no broadcast) */
    send_private_peek(pid, gs->future_peek, count);
    append_log_entry(&pb_node, make_action("SEE_FUTURE", pid, -1));
}
```

### Alter the Future
```c
void handle_alter_future(GameState *gs, int pid, CardType new_order[3]) {
    int count = MIN(3, gs->deck_top);
    for (int i = 0; i < count; i++)
        gs->deck[gs->deck_top - 1 - i] = new_order[i];
    /* El nuevo orden queda en el log → el Backup también sabe el nuevo tope */
    append_log_entry(&pb_node, make_action_with_order("ALTER_FUTURE", pid, new_order, count));
}
```

### Ventana NOPE
```c
void check_nope_window(GameState *gs, int acting_pid, CardType played) {
    if (!is_nopeable(played)) return;
    gs->phase = PHASE_NOPE_WINDOW;
    gs->nope_deadline = time(NULL) + 2;   /* 2 segundos */
    broadcast_nope_opportunity(gs, played);
    pthread_cond_timedwait(&nope_cond, &state_mutex,
                           &(struct timespec){gs->nope_deadline, 0});
    gs->phase = PHASE_PLAYING;
}
```

## Sincronización con Pthreads

```c
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  turn_cond   = PTHREAD_COND_INITIALIZER;
pthread_cond_t  nope_cond   = PTHREAD_COND_INITIALIZER;

/* Invariante: siempre que se modifique game_state, debe:
   1. Tomar state_mutex
   2. Aplicar cambio
   3. Incrementar lamport_clock
   4. Llamar append_log_entry() (que replica al Backup)
   5. Liberar state_mutex
   6. Llamar broadcast_state() FUERA del mutex */
```

## OpenMP para IA de Bots

```c
CardType bot_choose_card(GameState *gs, int bot_id) {
    int n = gs->players[bot_id].hand_size;
    int scores[20] = {0};

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < n; i++)
        scores[i] = evaluate_heuristic(gs, bot_id, i);

    int best = 0;
    for (int i = 1; i < n; i++)
        if (scores[i] > scores[best]) best = i;

    return gs->players[bot_id].hand[best];
}
```

## Cuellos de Botella y Mitigaciones

| Riesgo | Mitigación |
|---|---|
| **Deadlock `state_mutex`** | Orden fijo de adquisición: `state_mutex` es el único mutex de lógica; el mutex de replicación nunca se toma mientras `state_mutex` está tomado |
| **Turno sin fin** | `pthread_cond_timedwait(60s)` → si expira → SKIP automático + log de timeout |
| **Race en ventana NOPE** | El primer NOPE recibido gana; los subsiguientes se descartan con flag `nope_resolved` protegido por `state_mutex` |

---

# FASE 4: Despliegue en AWS EC2 (Semana 7)

## Objetivos
- 2 instancias EC2 con Primary y Backup funcionando.
- Script de deploy automatizado.
- Compilar con `-fopenmp` y optimizaciones.
- Cliente CLI con ncurses.

## Makefile

```makefile
CC     = gcc
CFLAGS = -Wall -Wextra -O2 -fopenmp -pthread
LIBS   = -lcjson -lncurses -lm

all: server client

server: server_main.c game_logic.c net_handler.c pb_replication.c heartbeat.c
	$(CC) $(CFLAGS) -o server $^ $(LIBS)

client: client_main.c ui_cli.c
	$(CC) $(CFLAGS) -o client $^ $(LIBS)

install-deps:
	apt-get install -y libcjson-dev libncurses-dev gcc make

clean:
	rm -f server client *.o /tmp/ek_* /var/lib/ek/raft.log
```

## Script de Deploy

```bash
#!/bin/bash
# deploy.sh
PRIMARY="10.0.1.5"
BACKUP="10.0.1.6"
KEY="ek-keypair.pem"

echo "=== Compilando ==="
make clean && make

echo "=== Desplegando PRIMARY ($PRIMARY) ==="
scp -i $KEY server ubuntu@$PRIMARY:~/
ssh -i $KEY ubuntu@$PRIMARY "
    pkill server 2>/dev/null; sleep 1
    mkdir -p /var/lib/ek
    ROLE=primary PEER_HOST=$BACKUP PEER_PORT=9191 \
    nohup ./server --port 9090 --hb-port 9192 > /var/log/ek.log 2>&1 &
    echo 'PRIMARY iniciado. PID='$!
"

echo "=== Desplegando BACKUP ($BACKUP) ==="
scp -i $KEY server ubuntu@$BACKUP:~/
ssh -i $KEY ubuntu@$BACKUP "
    pkill server 2>/dev/null; sleep 1
    mkdir -p /var/lib/ek
    ROLE=backup PEER_HOST=$PRIMARY PEER_PORT=9191 \
    nohup ./server --port 9090 --hb-port 9192 > /var/log/ek.log 2>&1 &
    echo 'BACKUP iniciado. PID='$!
"
echo "=== Cluster listo. Conectar a $PRIMARY:9090 ==="
```

## Interfaz CLI (ncurses)

```
┌─────────────────────────────────────────────────────────┐
│        EXPLODING KITTENS  —  Servidor: PRIMARY          │
├─────────────────────────────────────────────────────────┤
│  Turno: Alice (tú) [Lamport: 42]     Mazo: 18 cartas   │
│  Vivos: Alice ✅  Bob ✅  Charlie ❌                    │
├─────────────────────────────────────────────────────────┤
│  Tu mano:                                               │
│  [1] ATTACK    [2] SKIP    [3] SEE_FUTURE               │
│  [4] RAINBOW_KITTEN        [5] DEFUSE                   │
├─────────────────────────────────────────────────────────┤
│  > Elige [1-5] para jugar carta, [D] para robar: _     │
├─────────────────────────────────────────────────────────┤
│  LOG (últimas acciones):                                │
│  [ts=40] Bob jugó ATTACK                               │
│  [ts=41] Alice jugó SKIP                               │
│  [ts=42] Bob robó carta → EXPLODING KITTEN             │
│  [ts=42] Bob usó DEFUSE, colocó en posición 7          │
└─────────────────────────────────────────────────────────┘
```

## Cuellos de Botella y Mitigaciones

| Riesgo | Mitigación |
|---|---|
| **Split-Brain con 2 nodos** | Backup espera 5s antes de promover; script de healthcheck externo (AWS Route53) puede actuar como árbitro adicional |
| **Log crece indefinidamente** | Checkpointing periódico: cada 100 entradas se guarda snapshot del `GameState` a disco + se trunca el log hasta ese punto |
| **Costos de EC2** | Usar `t3.micro` (capa gratuita); `aws ec2 stop-instances` al terminar el día de desarrollo |
| **OpenMP limitado en t3.micro** | Configurar `OMP_NUM_THREADS=2`; el beneficio principal es en broadcast con múltiples clientes |

---

# Resumen de Entregables por Fase

| Fase | Entregable | Tecnologías principales |
|---|---|---|
| **Fase 1** | Server + Client TCP funcional, protocolo JSON, FIFOs | Sockets, Pthreads, Named Pipes |
| **Fase 2** | Replicación Primary-Backup, heartbeats, log Lamport, failover | Sockets (canal P-B), Pthreads (hilos HB), Lamport |
| **Fase 3** | Motor de juego completo (todas las cartas), bots, NOPE | Pthreads (mutex/condvar), OpenMP |
| **Fase 4** | Cluster en 2 EC2, CLI ncurses, scripts de deploy | AWS EC2, ncurses, Makefile |

---

# Invariantes para Claude Opus

Al generar código, respetar siempre:

1. **Toda modificación a `GameState` ocurre dentro de `state_mutex`.**
2. **Después de cada modificación, incrementar `lamport_clock` y llamar `append_log_entry()`.**
3. **El broadcast de estado ocurre FUERA del mutex** para evitar deadlock con hilos de cliente.
4. **`append_log_entry()` llama `replicate_to_backup()` antes de retornar.** Si el Backup no responde, loguear y continuar (el Primary no se bloquea esperando al Backup).
5. **Toda entrada de log se persiste con `fsync()` antes de enviar ACK al cliente.**
6. **La semilla del barajado es siempre la primera entrada del log** (índice 1). Todo el estado es reconstructible desde ella más las acciones posteriores.
7. **Los sockets de clientes usan `SO_RCVTIMEO = 30s`.** Si el recv expira, el turno se pasa automáticamente (SKIP).
8. **`signal(SIGPIPE, SIG_IGN)` al inicio de `main()`** en el servidor.
9. **El Backup nunca acepta acciones de clientes mientras el Primary está vivo.** Solo escucha el canal de replicación y los heartbeats.

---

*Documento generado para el proyecto de Sistemas Distribuidos — Exploding Kittens en C/UNIX*
*Basado en el material de las Unidades 1 y 2 del curso (Semanas 1–8)*
*Patrón elegido: Primary-Backup (ideas de Raft sin implementación completa del protocolo)*
