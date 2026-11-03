#!/bin/sh
# Prova tools/bridge_test.py contro l'emulatore su una pty, con --serial al posto del BLE.
# Da lanciare dalla radice del progetto dentro il container (tests/host/run.sh lo fa).
set -e
PY=$(ls /opt/esp/python_env/*/bin/python | head -1)
mkdir -p build-host/bt
gcc -std=gnu11 -Wall -Wextra -Werror -Imain tests/host/emu_pty.c main/nxt_emu.c -o build-host/emu_pty

build-host/emu_pty > build-host/bt/pty.txt &
EMU=$!
trap 'kill $EMU 2>/dev/null' EXIT
sleep 0.3
PORT=$(head -1 build-host/bt/pty.txt)

rm -f build-host/bt/bridge-test-*
# notifiche piccole (20 byte, MTU minima): ogni risposta di c?/f?/w? arriva in più pezzi
"$PY" tools/bridge_test.py --serial "$PORT" --chunk 20 --count 120 --out build-host/bt > build-host/bt/out.txt
REPORT=$(ls build-host/bt/bridge-test-*.md)

fail=0
check() {
    if grep -q -- "$1" "$REPORT"; then echo "ok: $2"; else echo "FALLITO: $2"; fail=1; fi
}
check '^\*\*Superata\*\*: 120 comandi, 0 errori' "esito superato"
check '| `WR?` | `OK, Write Ready` | ' "risposta di WR?"
check '| `V?` | `V=NXT_fake` | ' "risposta di V?"
check 'in una sola scrittura: 24 righe' "risposta lunga ricomposta"
check '| `c?` | 15 | ' "sequenza: i comandi girano"
# la risposta lunga deve essere arrivata spezzata in almeno 300/20 notifiche
n=$(sed -n 's/.* byte in \([0-9]*\) notifiche.*/\1/p' "$REPORT")
if [ "${n:-0}" -ge 15 ]; then echo "ok: risposta lunga su $n notifiche"; else echo "FALLITO: notifiche della risposta lunga: $n"; fail=1; fi
exit $fail
