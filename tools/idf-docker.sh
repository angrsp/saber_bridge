#!/bin/sh
# Esegue idf.py nel container ufficiale di ESP-IDF, senza installare la toolchain.
# Uso: tools/idf-docker.sh build   |   tools/idf-docker.sh menuconfig
# Il container non vede le porte COM: il flash si fa dall'host (vedi README).
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v5.5.6}"
ROOT="$(cd "$(dirname "$0")/.." && { pwd -W 2>/dev/null || pwd; })"
TTY=""
[ -t 0 ] && TTY="-it"
MSYS_NO_PATHCONV=1 exec docker run --rm $TTY -v "$ROOT:/project" -w /project "$IDF_IMAGE" idf.py "$@"
