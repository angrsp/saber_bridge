#!/bin/sh
# Compila le due varianti, reale (build/) e fake (build-fake/), nel container di ESP-IDF.
# Un ramo feature/ si chiude solo se entrambe compilano senza errori né warning (piano, 2.1).
set -e
cd "$(dirname "$0")/.."
tools/idf-docker.sh build
tools/idf-docker.sh -B build-fake -D SDKCONFIG=build-fake/sdkconfig \
    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.fake" build
