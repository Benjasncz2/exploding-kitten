/* ── Config ────────────────────────────────────────────────── */
// WS_PORT = HTTP_PORT + 1, leído desde URL param "ws" o inferido
const WS_PORT = parseInt(new URLSearchParams(window.location.search).get('ws') || (location.port ? +location.port + 1 : 8081));

const CARD_COLOR = {
    EXPLODING_KITTEN: '#e03030',
    DEFUSE:           '#43a047',
    ATTACK:           '#c62828',
    SKIP:             '#1e88e5',
    SEE_FUTURE:       '#7b1fa2',
    ALTER_FUTURE:     '#ab47bc',
    SHUFFLE:          '#f9a825',
    NOPE:             '#e64a19',
    RAINBOW_KITTEN:   '#e91e63',
    TACO_CAT:         '#ff7043',
    HAIRY_POTATO:     '#7cb342',
    BEARD_CAT:        '#0288d1',
    CATTERMELON:      '#00897b',
};

const CARD_IMG = {
    EXPLODING_KITTEN: 'explodin_Kitten.png',
    DEFUSE:           'defuse.png',
    ATTACK:           'attack.png',
    SKIP:             'skip.png',
    SEE_FUTURE:       'see_future.png',
    ALTER_FUTURE:     'alterTheFuture.png',
    SHUFFLE:          'shuffle.png',
    NOPE:             'nope.png',
    RAINBOW_KITTEN:   'gatoArcoiris.png',
    TACO_CAT:         'gatoTaco.png',
    HAIRY_POTATO:     'gatoPeludo.png',
    BEARD_CAT:        'gatoBarba.png',
    CATTERMELON:      'gatoMelon.png',
};

const CARD_NAME = {
    EXPLODING_KITTEN: 'Exploding Kitten',
    DEFUSE:           'Defuse',
    ATTACK:           'Attack',
    SKIP:             'Skip',
    SEE_FUTURE:       'See Future',
    ALTER_FUTURE:     'Alter Future',
    SHUFFLE:          'Shuffle',
    NOPE:             'Nope',
    RAINBOW_KITTEN:   'Rainbow Kitten',
    TACO_CAT:         'Taco Cat',
    HAIRY_POTATO:     'Hairy Potato',
    BEARD_CAT:        'Beard Cat',
    CATTERMELON:      'Cattermelon',
};

const PHASE_COLOR = {
    LOBBY:        '#9e9e9e',
    PLAYING:      '#43a047',
    NOPE_WINDOW:  '#e53935',
    DEFUSE_PLACE: '#fb8c00',
    GAME_OVER:    '#8e24aa',
};

/* ── State ─────────────────────────────────────────────────── */
let PLAYER_NAME = new URLSearchParams(window.location.search).get('name') || 'Player';

let state = {
    phase:          'LOBBY',
    current_player: -1,
    attack_count:   0,
    deck_size:      0,
    num_alive:      0,
    lamport_ts:     0,
    last_event:     '',
    players:        [],
    my_pid:         -1,
    hand:           [],
};

let ws           = null;
let selectedCard = null;
let eventLog     = [];
let musicOn      = false;

const music = document.getElementById('bg-music');
music.volume = 0.35;

/* ── WebSocket ─────────────────────────────────────────────── */
function connect() {
    ws = new WebSocket(`ws://localhost:${WS_PORT}`);

    ws.onopen = () => setStatus('Conectado. Esperando jugadores...');

    ws.onmessage = ({ data }) => {
        let msg;
        try { msg = JSON.parse(data); } catch { return; }
        dispatch(msg);
    };

    ws.onclose = () => {
        setStatus('Desconectado. Reconectando en 3 s...');
        setTimeout(connect, 3000);
    };

    ws.onerror = () => setStatus('Error de conexión.');
}

function dispatch(msg) {
    switch (msg.type) {
        case 'CONNECTED':
            PLAYER_NAME = msg.player_name;
            setStatus(`Conectado como "${PLAYER_NAME}". Esperando jugadores...`);
            startMusicOnInteraction();
            break;

        case 'GAME_STATE':
            applyState(msg);
            render();
            break;

        case 'ERROR':
            setStatus('⚠ ' + msg.msg);
            break;

        case 'PEEK':
            showPeek(msg.cards || []);
            addEvent('SEE FUTURE: ' + (msg.cards || []).join(' > '));
            break;
    }
}

/* ── State update ──────────────────────────────────────────── */
function applyState(msg) {
    if (msg.phase          !== undefined) state.phase          = msg.phase;
    if (msg.current_player !== undefined) state.current_player = msg.current_player;
    if (msg.attack_count   !== undefined) state.attack_count   = msg.attack_count;
    if (msg.deck_size      !== undefined) state.deck_size      = msg.deck_size;
    if (msg.num_alive      !== undefined) state.num_alive      = msg.num_alive;
    if (msg.ts             !== undefined) state.lamport_ts     = msg.ts;

    if (msg.event) {
        state.last_event = msg.event;
        addEvent(msg.event);
    }

    if (Array.isArray(msg.players)) {
        state.players = msg.players;
        state.hand = [];
        for (const p of msg.players) {
            if (p.name === PLAYER_NAME) {
                state.my_pid = p.id;
                if (Array.isArray(p.hand)) state.hand = p.hand;
            }
        }
    }

    if (state.phase === 'GAME_OVER') showGameOver();
}

/* ── Render ────────────────────────────────────────────────── */
function render() {
    renderHeader();
    renderPlayers();
    renderHand();
    renderControls();
}

function renderHeader() {
    const badge = document.getElementById('phase-badge');
    badge.textContent        = state.phase;
    badge.style.background   = PHASE_COLOR[state.phase] || '#9e9e9e';
    badge.style.color        = '#fff';

    document.getElementById('deck-info').textContent    = '🃏 ' + state.deck_size;
    document.getElementById('alive-info').textContent   = '❤️ ' + state.num_alive;
    document.getElementById('lamport-info').textContent = '⏱ ' + state.lamport_ts;
    document.getElementById('deck-count').textContent   = state.deck_size || '—';
    document.getElementById('last-event').textContent   = state.last_event || '—';

    document.body.classList.toggle('nope-active', state.phase === 'NOPE_WINDOW');
    document.getElementById('defuse-overlay').style.display =
        state.phase === 'DEFUSE_PLACE' ? 'flex' : 'none';

}

function renderPlayers() {
    const container = document.getElementById('players-circle');
    const n = state.players.length;
    if (n === 0) { container.innerHTML = ''; return; }

    const myIdx = state.players.findIndex(p => p.id === state.my_pid);
    const base  = myIdx >= 0 ? myIdx : 0;

    container.innerHTML = state.players.map((player, i) => {
        const offset   = (i - base + n) % n;
        const angleDeg = 90 + offset * (360 / n);
        const angleRad = angleDeg * Math.PI / 180;
        const r        = 38;
        const x        = 50 + r * Math.cos(angleRad);
        const y        = 50 + r * Math.sin(angleRad);

        const isMe     = player.id === state.my_pid;
        const isMyTurn = i === state.current_player;
        const isAlive  = player.alive !== false;
        const isBot    = !!player.bot;

        const avatarClass = [
            'player-avatar',
            isAlive  ? 'alive'   : 'dead',
            isMe     ? 'is-me'   : '',
            isMyTurn && isAlive ? 'my-turn' : '',
        ].filter(Boolean).join(' ');

        const avatarContent = !isAlive
            ? '<span class="avatar-dead">✕</span>'
            : `<img class="avatar-img" src="usuario.png" alt="${esc(player.name)}">`;

        return `
          <div class="player-token" style="left:${x.toFixed(1)}%; top:${y.toFixed(1)}%">
            <div class="${avatarClass}">
              ${isMe  ? '<span class="badge badge-me">YO</span>'   : ''}
              ${isBot ? '<span class="badge badge-bot">BOT</span>' : ''}
              ${avatarContent}
            </div>
            <div class="player-name">${esc(player.name)}</div>
            <div class="player-card-count">${player.hand_size ?? 0} cartas</div>
          </div>`;
    }).join('');
}

function cardSizeForCount(n) {
    if (n <= 6)  return { w: 160, h: 220 };
    if (n <= 9)  return { w: 130, h: 180 };
    if (n <= 12) return { w: 105, h: 148 };
    return            { w: 85,  h: 120 };
}

function renderHand() {
    const container = document.getElementById('hand-cards');
    document.getElementById('hand-count').textContent =
        state.hand.length ? `(${state.hand.length})` : '';

    const { w, h } = cardSizeForCount(state.hand.length);
    document.documentElement.style.setProperty('--card-w', w + 'px');
    document.documentElement.style.setProperty('--card-h', h + 'px');
    document.getElementById('hand-area').style.minHeight = (h + 45) + 'px';

    if (state.hand.length === 0) {
        container.innerHTML = '<span style="color:#ccc;font-size:0.85rem;align-self:center;padding:8px">Sin cartas</span>';
        return;
    }

    const isNopeWindow = state.phase === 'NOPE_WINDOW';

    container.innerHTML = state.hand.map((card, i) => {
        const color    = CARD_COLOR[card] || '#aaa';
        const img      = CARD_IMG[card];
        const name     = CARD_NAME[card]  || card;
        const isNope   = card === 'NOPE';
        const selClass = selectedCard === i ? 'selected'
                       : (isNopeWindow && isNope) ? 'nope-highlight' : '';

        return `
          <div class="card ${selClass}" onclick="handleCardClick(${i})" title="${name}">
            <div class="card-top-bar" style="background:${color}"></div>
            ${img ? `<img class="card-img" src="${img}" alt="${name}">` : `<div class="card-img-placeholder">${name}</div>`}
            <div class="card-name">${name}</div>
            <div class="card-number">${i + 1}</div>
          </div>`;
    }).join('');
}

function renderControls() {
    const isLobby    = state.phase === 'LOBBY';
    const isPlaying  = state.phase === 'PLAYING';
    const isNope     = state.phase === 'NOPE_WINDOW';
    const myTurnSlot = state.players[state.current_player];
    const isMyTurn   = isPlaying && myTurnSlot && myTurnSlot.id === state.my_pid;
    const hasSelected = selectedCard !== null && state.hand[selectedCard];
    const selectedCardType = hasSelected ? state.hand[selectedCard] : null;

    show('btn-start',     isLobby);
    show('draw-bar',      isMyTurn && !hasSelected);
    show('btn-nope',      isNope);
    show('btn-play',      isMyTurn && hasSelected);
    show('btn-play-pair', isMyTurn && hasSelected && canPlayAsPair(selectedCardType));
    show('play-hint',     isMyTurn && state.hand.length > 0 && !hasSelected);

    renderTurnIndicator(isPlaying, isMyTurn, myTurnSlot);
}

function renderTurnIndicator(isPlaying, isMyTurn, currentSlot) {
    const el = document.getElementById('turn-indicator');
    const activePhase = ['PLAYING', 'NOPE_WINDOW', 'DEFUSE_PLACE'].includes(state.phase);

    if (!activePhase || !currentSlot) {
        el.className = 'hidden';
        return;
    }

    // Nunca usar style.display — dejar que la clase CSS lo controle
    if (isMyTurn) {
        el.className = 'my-turn';
        el.innerHTML = '¡Es tu turno!';
    } else {
        el.className = 'other-turn';
        el.innerHTML = `Turno de <strong>${esc(currentSlot.name)}</strong>`;
    }
}

/* ── Card interaction ──────────────────────────────────────── */
const CAT_CARDS = new Set(['RAINBOW_KITTEN','TACO_CAT','HAIRY_POTATO','BEARD_CAT','CATTERMELON']);

function canPlayAsPair(card) {
    return CAT_CARDS.has(card) && state.hand.filter(c => c === card).length >= 2;
}

function handleCardClick(idx) {
    // Durante ventana NOPE cualquier jugador puede jugar su carta NOPE
    if (state.phase === 'NOPE_WINDOW') {
        if (state.hand[idx] === 'NOPE') {
            sendAction('NOPE');
            setStatus('Nope jugado.');
        } else {
            setStatus('Solo puedes jugar NOPE en este momento.');
        }
        return;
    }

    const myTurnSlot = state.players[state.current_player];
    const isMyTurn   = state.phase === 'PLAYING' &&
                       myTurnSlot && myTurnSlot.id === state.my_pid;

    if (!isMyTurn) {
        setStatus('No es tu turno.');
        return;
    }

    if (selectedCard === idx) {
        playSelected();
    } else {
        selectedCard = idx;
        const card = state.hand[idx];
        const name = CARD_NAME[card] || card;
        setStatus(`"${name}" seleccionada.`);
        renderHand();
        renderControls();
    }
}

function playSelected() {
    if (selectedCard === null) return;
    playCard(selectedCard);
}

function playCard(idx) {
    const card = state.hand[idx];
    if (!card) return;
    sendRaw({ type: 'PLAY_CARD', payload: card, ts: state.lamport_ts + 1 });
    selectedCard = null;
    setStatus('Jugando: ' + (CARD_NAME[card] || card) + '...');
    renderHand();
    renderControls();
}

/* ── Actions ───────────────────────────────────────────────── */
function sendAction(type) {
    sendRaw({ type, ts: state.lamport_ts + 1 });
}

function sendDefuse() {
    const pos = parseInt(document.getElementById('defuse-pos').value, 10) || 0;
    sendRaw({ type: 'DEFUSE_POS', payload: String(pos), ts: state.lamport_ts + 1 });
    document.getElementById('defuse-overlay').style.display = 'none';
}

function sendRaw(obj) {
    if (ws && ws.readyState === WebSocket.OPEN)
        ws.send(JSON.stringify(obj));
}

/* ── Game Over ─────────────────────────────────────────────── */
function showGameOver() {
    const overlay = document.getElementById('gameover-overlay');
    const myPlayer = state.players.find(p => p.id === state.my_pid);
    const iWon = myPlayer && myPlayer.alive;

    document.getElementById('gameover-emoji').textContent = iWon ? '🏆' : '💣';
    document.getElementById('gameover-text').textContent  = iWon ? '¡Ganaste!' : '¡Explotaste!';
    document.getElementById('gameover-sub').textContent   = state.last_event || '';
    overlay.style.display = 'flex';
}

/* ── See Future overlay ────────────────────────────────────── */
function showPeek(cards) {
    const overlay = document.getElementById('peek-overlay');
    const container = document.getElementById('peek-cards');

    const labels = ['1ra en robarse', '2da', '3ra'];
    const parts = [];
    cards.forEach((card, i) => {
        const color = CARD_COLOR[card] || '#aaa';
        const img   = CARD_IMG[card];
        const name  = CARD_NAME[card]  || card;
        parts.push(`
          <div class="peek-card" style="z-index:${3 - i}">
            <div class="card-top-bar" style="background:${color}"></div>
            ${img ? `<img class="card-img" src="${img}" alt="${name}">` : ''}
            <div class="card-name">${name}</div>
            <div class="peek-position">${labels[i]}</div>
          </div>`);
        if (i < cards.length - 1)
            parts.push(`<div class="peek-arrow">&#8594;</div>`);
    });
    container.innerHTML = parts.join('');

    overlay.style.display = 'flex';
}

function closePeek() {
    document.getElementById('peek-overlay').style.display = 'none';
}

/* ── Par overlay ───────────────────────────────────────────── */
let pendingPairCard = null;

function openPairSelector() {
    if (selectedCard === null) return;
    pendingPairCard = state.hand[selectedCard];

    const targets = document.getElementById('pair-targets');
    const opponents = state.players.filter(p => p.alive && p.id !== state.my_pid);

    if (opponents.length === 0) {
        setStatus('No hay oponentes disponibles.');
        return;
    }

    targets.innerHTML = opponents.map((p, i) => {
        const slot = state.players.findIndex(pl => pl.id === p.id);
        return `<button class="pair-target-btn" onclick="sendPair(${slot})">${esc(p.name)}</button>`;
    }).join('');

    document.getElementById('pair-overlay').style.display = 'flex';
}

function sendPair(targetSlot) {
    if (!pendingPairCard) return;
    sendRaw({ type: 'PLAY_PAIR', payload: `${pendingPairCard} ${targetSlot}`, ts: state.lamport_ts + 1 });
    setStatus('Par de ' + (CARD_NAME[pendingPairCard] || pendingPairCard) + ' jugado.');
    pendingPairCard = null;
    selectedCard = null;
    closePair();
    renderHand();
    renderControls();
}

function closePair() {
    document.getElementById('pair-overlay').style.display = 'none';
    pendingPairCard = null;
}

/* ── Music ─────────────────────────────────────────────────── */
function toggleMusic() {
    musicOn ? pauseMusic() : playMusic();
}

function playMusic() {
    music.play().then(() => {
        musicOn = true;
        document.getElementById('music-btn').textContent = '🔊';
    }).catch(() => {});
}

function pauseMusic() {
    music.pause();
    musicOn = false;
    document.getElementById('music-btn').textContent = '🔇';
}

function startMusicOnInteraction() {
    document.addEventListener('click', () => {
        if (!musicOn) playMusic();
    }, { once: true });
}

/* ── Helpers ───────────────────────────────────────────────── */
function setStatus(msg) {
    document.getElementById('status-bar').textContent = msg;
}

function addEvent(line) {
    eventLog.unshift(line);
    if (eventLog.length > 30) eventLog.pop();
    document.getElementById('event-log').innerHTML =
        eventLog.map(e => `<div class="event-line">${esc(e)}</div>`).join('');
}

function show(id, visible) {
    document.getElementById(id).style.display = visible ? '' : 'none';
}

function esc(s) {
    return String(s)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;');
}

/* ── Init ──────────────────────────────────────────────────── */
connect();
