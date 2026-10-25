#!/bin/sh
# Prova tools/f0_probe.py contro l'emulatore su una pty, con la prova di persistenza e il ripristino.
# Da lanciare dalla radice del progetto dentro il container (tests/host/run.sh lo fa).
set -e
PY=$(ls /opt/esp/python_env/*/bin/python | head -1)
mkdir -p build-host/f0
gcc -std=gnu11 -Wall -Wextra -Werror -Imain tests/host/emu_pty.c main/nxt_emu.c -o build-host/emu_pty

build-host/emu_pty > build-host/f0/pty.txt &
EMU=$!
trap 'kill $EMU 2>/dev/null' EXIT
sleep 0.3
PORT=$(head -1 build-host/f0/pty.txt)

# risposte alle domande: lama blu sì, lama spenta sì, Invio dopo lo "spegnimento"
rm -f build-host/f0/f0-*
printf 's\ns\n\n' | "$PY" tools/f0_probe.py --port "$PORT" --out build-host/f0 > build-host/f0/out.txt
REPORT=$(ls build-host/f0/f0-report-*.md)

fail=0
check() {
    if grep -q -- "$1" "$REPORT"; then echo "ok: $2"; else echo "FALLITO: $2"; fail=1; fi
}
check '| `WR?\\n` | `OK, Write Ready\\n` |' "risposta grezza di WR?"
check '| `XYZ\\n` | \*(nessuna)\* | — | sì |' "silenzio sul comando inesistente"
check 'Emulatore | Osservato' "intestazione della tabella"
check '| `P=0,0,255,0\\n` | `OK, P=0,0,255,0\\n` | [0-9]* ms | sì | sì |' "osservazione registrata"
check 'senza `SAVE`, dopo lo spegnimento: (5, 6, 7, 8) (mantenuto)' "persistenza senza SAVE (l'emulatore tiene tutto in RAM)"
check 'Porta sparita durante lo spegnimento: non verificabile' "porta non verificabile su una pty"
check 'Ripristino dei colori originali: riuscito' "ripristino"
if grep -q '\*\*no\*\*' "$REPORT"; then echo "FALLITO: l'emulatore non coincide con se stesso"; fail=1; fi
# l'emulatore risponde in meno di 50 ms (il timeout di lettura): il tempo al primo byte deve essere sotto
if grep -q '| `WR?\\n` | `OK, Write Ready\\n` | [0-9] ms |' "$REPORT"; then
    echo "ok: tempo al primo byte misurato"
else
    echo "FALLITO: il tempo al primo byte sembra quello del timeout di lettura"; fail=1
fi

# il ripristino ha rimesso davvero i colori di default dell'emulatore
"$PY" -c "
import serial, sys
s = serial.Serial(sys.argv[1], timeout=1)
s.write(b'c?\n'); s.flush()
got = s.read(400).decode()
want = 'c0=255,0,0,0\nc1=0,255,0,0\n'
print('ok: colori di default rimessi' if got.startswith(want) else 'FALLITO: colori dopo il ripristino: %r' % got)
sys.exit(0 if got.startswith(want) else 1)
" "$PORT" || fail=1

exit $fail
