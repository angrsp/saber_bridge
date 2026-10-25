#!/bin/sh
# Compila ed esegue le prove sul PC. Da lanciare dalla radice del progetto (tools/test-host.sh lo fa nel container).
set -e
mkdir -p build-host
CFLAGS="-std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Imain -Itests/host -Itests/host/stubs"
gcc $CFLAGS tests/host/test_nxt_emu.c main/nxt_emu.c -o build-host/test_nxt_emu
gcc $CFLAGS tests/host/test_pipe.c main/pipe.c -o build-host/test_pipe
# i pipe vivono per tutto il programma, come nel firmware: niente rilevamento dei leak
export ASAN_OPTIONS=detect_leaks=0
build-host/test_nxt_emu
build-host/test_pipe
