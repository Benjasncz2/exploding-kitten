#!/bin/bash
# reassign_eip.sh — Reasigna la Elastic IP al nodo Backup promovido a Primary.
# Ejecutado automáticamente por el servidor al hacer failover.
#
# Requiere:
#   - AWS CLI instalado y configurado en la instancia (rol IAM con ec2:AssociateAddress)
#   - EK_EIP_ALLOC_ID: ID de asignación de la Elastic IP (ej. eipalloc-0abc1234)
#
# Uso: EK_EIP_ALLOC_ID=eipalloc-xxxx ./scripts/reassign_eip.sh

set -euo pipefail

EIP_ALLOC="${EK_EIP_ALLOC_ID:-}"

if [[ -z "$EIP_ALLOC" ]]; then
    echo "[reassign_eip] ERROR: EK_EIP_ALLOC_ID no está definida." >&2
    echo "[reassign_eip] Configura la variable con el ID de tu Elastic IP." >&2
    exit 1
fi

# Obtener el Instance ID via IMDSv2
TOKEN=$(curl -sf -X PUT "http://169.254.169.254/latest/api/token" \
    -H "X-aws-ec2-metadata-token-ttl-seconds: 21600")
INSTANCE_ID=$(curl -sf -H "X-aws-ec2-metadata-token: $TOKEN" \
    http://169.254.169.254/latest/meta-data/instance-id)
if [[ -z "$INSTANCE_ID" ]]; then
    echo "[reassign_eip] ERROR: No se pudo obtener el Instance ID." >&2
    exit 1
fi

echo "[reassign_eip] Reasignando Elastic IP $EIP_ALLOC a $INSTANCE_ID ..."

aws ec2 associate-address \
    --instance-id "$INSTANCE_ID" \
    --allocation-id "$EIP_ALLOC" \
    --allow-reassociation

echo "[reassign_eip] Elastic IP reasignada exitosamente a $INSTANCE_ID."
