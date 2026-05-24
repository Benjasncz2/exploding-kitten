#!/bin/bash
# deploy.sh — Compila y despliega el servidor en las dos instancias EC2.
# Uso: ./scripts/deploy.sh [pem_key]
#
# Variables de entorno esperadas (o editar abajo):
#   EK_KEY      Ruta al archivo .pem de la keypair de AWS (default: ek-keypair.pem)
#   EK_PRIMARY  IP pública o DNS de la instancia Primary (default: 10.0.1.5)
#   EK_BACKUP   IP pública o DNS de la instancia Backup  (default: 10.0.1.6)

set -euo pipefail

PRIMARY="${EK_PRIMARY:-10.0.1.5}"
BACKUP="${EK_BACKUP:-10.0.1.6}"
KEY="${EK_KEY:-ek-keypair.pem}"
REMOTE_USER="ubuntu"
REMOTE_DIR="~/ek"

if [[ ! -f "$KEY" ]]; then
    echo "ERROR: No se encontró la keypair '$KEY'."
    echo "Usa: EK_KEY=/ruta/a/key.pem ./scripts/deploy.sh"
    exit 1
fi

# ── 1. Compilar localmente ────────────────────────────────────────────
echo "=== [1/4] Compilando ==="
make clean && make
echo "Binarios: server  client"

# ── 2. Instalar dependencias en ambas instancias (idempotente) ────────
echo "=== [2/4] Instalando dependencias ==="
for HOST in "$PRIMARY" "$BACKUP"; do
    echo "  → $HOST"
    ssh -i "$KEY" -o StrictHostKeyChecking=no "$REMOTE_USER@$HOST" \
        "sudo apt-get update -qq && \
         sudo apt-get install -y -qq libcjson-dev libncurses-dev gcc make && \
         mkdir -p $REMOTE_DIR /var/lib/ek" &
done
wait
echo "Dependencias instaladas."

# ── 3. Subir binario y scripts ────────────────────────────────────────
echo "=== [3/4] Desplegando PRIMARY ($PRIMARY) ==="
scp -i "$KEY" -o StrictHostKeyChecking=no \
    server scripts/reassign_eip.sh \
    "$REMOTE_USER@$PRIMARY:$REMOTE_DIR/"

ssh -i "$KEY" -o StrictHostKeyChecking=no "$REMOTE_USER@$PRIMARY" "
    chmod +x $REMOTE_DIR/reassign_eip.sh
    pkill server 2>/dev/null; sleep 1
    OMP_NUM_THREADS=2 \
    ROLE=primary \
    PEER_HOST=$BACKUP \
    nohup $REMOTE_DIR/server > /var/log/ek-primary.log 2>&1 &
    echo \"PRIMARY iniciado. PID=\$!\"
"

echo "=== [3/4] Desplegando BACKUP ($BACKUP) ==="
scp -i "$KEY" -o StrictHostKeyChecking=no \
    server scripts/reassign_eip.sh \
    "$REMOTE_USER@$BACKUP:$REMOTE_DIR/"

ssh -i "$KEY" -o StrictHostKeyChecking=no "$REMOTE_USER@$BACKUP" "
    chmod +x $REMOTE_DIR/reassign_eip.sh
    pkill server 2>/dev/null; sleep 1
    OMP_NUM_THREADS=2 \
    ROLE=backup \
    PEER_HOST=$PRIMARY \
    nohup $REMOTE_DIR/server > /var/log/ek-backup.log 2>&1 &
    echo \"BACKUP iniciado. PID=\$!\"
"

# ── 4. Verificar que están escuchando ─────────────────────────────────
echo "=== [4/4] Verificando cluster ==="
sleep 3
for HOST in "$PRIMARY" "$BACKUP"; do
    STATUS=$(ssh -i "$KEY" -o StrictHostKeyChecking=no \
                "$REMOTE_USER@$HOST" "pgrep -c server 2>/dev/null || echo 0")
    echo "  $HOST: $STATUS proceso(s) 'server' activo(s)"
done

echo ""
echo "=== Cluster listo ==="
echo "Conectar: ./client $PRIMARY 9090 <nombre>"
