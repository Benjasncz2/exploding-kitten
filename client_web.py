#!/usr/bin/env python3
"""
client_web.py — Cliente web para Exploding Kittens
Sirve el frontend HTML y hace de proxy WebSocket → TCP al servidor C.

Uso: python3 client_web.py <server_ip> <player_name> [server_port]
Dep: pip install websockets
"""

import asyncio, websockets, socket, struct, sys, os, json, threading
import http.server, webbrowser

SERVER_IP   = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PLAYER_NAME = sys.argv[2] if len(sys.argv) > 2 else "Player"
SERVER_PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 9090
HTTP_PORT   = int(sys.argv[4]) if len(sys.argv) > 4 else 8080
WS_PORT     = HTTP_PORT + 1
ROOT_DIR    = os.path.dirname(os.path.abspath(__file__))


def tcp_send(sock, msg: str):
    data = msg.encode()
    sock.sendall(struct.pack(">I", len(data)) + data)


def tcp_recv(sock) -> str | None:
    buf = b""
    while len(buf) < 4:
        chunk = sock.recv(4 - len(buf))
        if not chunk:
            return None
        buf += chunk
    n = struct.unpack(">I", buf)[0]
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf.decode()


async def ws_handler(websocket):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.connect((SERVER_IP, SERVER_PORT))
    except Exception as e:
        await websocket.send(json.dumps({"type": "ERROR", "msg": f"No se pudo conectar: {e}"}))
        return

    # Informar al browser quién es el jugador local
    await websocket.send(json.dumps({"type": "CONNECTED", "player_name": PLAYER_NAME}))

    # Enviar JOIN al servidor C
    tcp_send(sock, json.dumps({"type": "JOIN", "player_name": PLAYER_NAME, "ts": 0}))

    loop = asyncio.get_event_loop()

    async def server_to_browser():
        while True:
            msg = await loop.run_in_executor(None, tcp_recv, sock)
            if msg is None:
                break
            try:
                await websocket.send(msg)
            except Exception:
                break

    async def browser_to_server():
        async for msg in websocket:
            try:
                tcp_send(sock, msg)
            except Exception:
                break

    try:
        await asyncio.gather(server_to_browser(), browser_to_server())
    finally:
        sock.close()


def serve_http():
    os.chdir(ROOT_DIR)

    class QuietHandler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *args):
            pass

    with http.server.HTTPServer(("", HTTP_PORT), QuietHandler) as httpd:
        httpd.serve_forever()


async def main():
    threading.Thread(target=serve_http, daemon=True).start()

    url = f"http://localhost:{HTTP_PORT}/web/?name={PLAYER_NAME}"
    print(f"\n{'='*52}")
    print(f"  Exploding Kittens — Cliente Web")
    print(f"{'='*52}")
    print(f"  Jugador   : {PLAYER_NAME}")
    print(f"  Servidor  : {SERVER_IP}:{SERVER_PORT}")
    print(f"  Navegador : {url}")
    print(f"{'='*52}\n")
    print("  Abre la URL de arriba en tu navegador.")
    print("  Ctrl+C para salir.\n")

    async with websockets.serve(ws_handler, "localhost", WS_PORT):
        await asyncio.Future()


asyncio.run(main())
