#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if ! command -v idf.py >/dev/null 2>&1; then
    echo "Activate ESP-IDF first: source ~/esp/esp-idf/export.sh" >&2
    exit 1
fi
idf.py -B build-esp32 "$@" build
