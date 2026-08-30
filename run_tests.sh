#!/bin/bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${REPO_ROOT}/.venv"
DEVICE="${C985_DEVICE:-/dev/video0}"
BIND_SCRIPT="${REPO_ROOT}/tools/c985-bind.sh"

if [[ ! -d "${VENV_DIR}" ]]; then
    python3 -m venv "${VENV_DIR}"
    "${VENV_DIR}/bin/pip" install --upgrade pip
    "${VENV_DIR}/bin/pip" install -r "${REPO_ROOT}/requirements.txt"
fi

echo "Unbinding c985 module (requires sudo)..."
sudo "${BIND_SCRIPT}" unbind || true

echo "Binding c985 module (requires sudo)..."
sudo "${BIND_SCRIPT}" bind

sleep 2

export C985_DEVICE="${DEVICE}"
"${VENV_DIR}/bin/pytest" "${REPO_ROOT}/tests/" -v "$@"