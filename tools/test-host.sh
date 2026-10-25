#!/bin/sh
# Prove sul PC con il gcc del container di ESP-IDF: emulatore della NXT e pipe.
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v5.5.6}"
ROOT="$(cd "$(dirname "$0")/.." && { pwd -W 2>/dev/null || pwd; })"
MSYS_NO_PATHCONV=1 exec docker run --rm -v "$ROOT:/project" -w /project --entrypoint sh "$IDF_IMAGE" tests/host/run.sh
