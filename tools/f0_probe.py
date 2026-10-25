#!/usr/bin/env python3
"""F0: registra le risposte reali della spada sulla seriale USB (piano-FW-bridge.md, fase F0 e sezione 6).

Manda i comandi della fase F0 uno alla volta, salva i byte grezzi di ogni risposta e li confronta con
quello che risponde l'emulatore della variante fake (main/nxt_emu.c, che ricalca CoreComms.cpp di
polaris-opencore). Poi, se non lo si salta, prova la persistenza dei colori dopo uno spegnimento.

Prima di modificare qualcosa salva i colori principali in un file di backup e alla fine li rimette
(con SAVE). Se lo script si interrompe a metà: python tools/f0_probe.py --restore <file di backup>.

Uso (spada accesa, collegata col cavo col rosso tagliato):
    pip install pyserial
    python tools/f0_probe.py                    # porta trovata da sola (VID:PID 0483:5740 o 16C0:0483)
    python tools/f0_probe.py --port COM5
    python tools/f0_probe.py --skip-persistence # senza la prova di spegnimento
    python tools/f0_probe.py --no-rts           # porta aperta con DTR ma senza RTS
"""

import argparse
import datetime as dt
import json
import re
import sys
import time
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("Serve pyserial: pip install pyserial")

KNOWN_IDS = {(0x0483, 0x5740): "Anima NXT", (0x16C0, 0x0483): "Anima EVO"}

REPLY_TIMEOUT = 3.0   # come il timeout di lettura di py2saber e dell'app
IDLE_GAP = 0.3        # una risposta è finita dopo questo silenzio
REOPEN_TIMEOUT = 30.0

COLOR_LINE = re.compile(r"^([cC])([0-7])=(\d+),(\d+),(\d+),(\d+)$")


def eight_lines(kind):
    """Le 8 righe di c?, f? o w?, una per bank."""
    return r"(?:" + kind + r"[0-7]=\d+,\d+,\d+,\d+\n){8}"


# Comandi della fase F0, in ordine. "expect" è la risposta dell'emulatore (regex sull'intera risposta):
# se la NXT risponde diversamente, l'emulatore va corretto. "observe" chiede di guardare la spada.
PROBE = [
    {"cmd": b"WR?\n", "expect": r"OK, Write Ready\n"},
    {"cmd": b"V?\n", "expect": r"V=[^\n]+\n"},
    {"cmd": b"S?\n", "expect": r"S=[^\n]+\n"},
    {"cmd": b"B?\n", "expect": r"B=[0-7]\n"},
    {"cmd": b"c?\n", "expect": eight_lines("c")},
    {"cmd": b"f?\n", "expect": eight_lines("f")},
    {"cmd": b"w?\n", "expect": eight_lines("w"), "note": "richiede la build >= 1.9.15"},
    {"cmd": b"C0=255,0,0,0\n", "expect": r"OK C0=255,0,0,0\n",
     "note": "eco del comando o colore esadecimale?"},
    {"cmd": b"c0?\n", "expect": r"c0=255,0,0,0\n"},
    {"cmd": b"P=0,0,255,0\n", "expect": r"OK, P=0,0,255,0\n",
     "observe": "La lama si è accesa di blu?"},
    {"cmd": b"P=0\n", "expect": r"OK, P=0\n", "observe": "La lama si è spenta?"},
    {"cmd": b"SAVE\n", "expect": r"OK SAVE\n"},
    {"cmd": b"XYZ\n", "expect": r"", "note": "comando inesistente: deve tacere"},
    {"cmd": b"V?\r\n", "expect": r"",
     "note": "terminatore CRLF: l'emulatore tace perché il \\r resta nel comando"},
    {"cmd": b"#f0\n", "expect": r"#", "note": "riga legacy: un solo '#', senza LF"},
]

SAVE_COLOR = (1, 2, 3, 4)     # bank 0, con SAVE
NOSAVE_COLOR = (5, 6, 7, 8)   # bank 1, senza SAVE


def find_port():
    found = [p for p in list_ports.comports() if (p.vid, p.pid) in KNOWN_IDS]
    if not found:
        ports = ", ".join(f"{p.device} ({p.hwid})" for p in list_ports.comports()) or "nessuna"
        sys.exit(f"Nessuna spada trovata. Porte seriali: {ports}. Usa --port.")
    if len(found) > 1:
        sys.exit("Più spade collegate: " + ", ".join(p.device for p in found) + ". Usa --port.")
    return found[0].device


def port_info(device):
    for p in list_ports.comports():
        if p.device == device:
            name = KNOWN_IDS.get((p.vid, p.pid), "sconosciuto")
            return {
                "device": p.device,
                "hwid": p.hwid,
                "vid_pid": f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else "?",
                "model": name,
                "description": p.description,
                "serial_number": p.serial_number,
                "location": p.location,
                "interface": p.interface,
            }
    return {"device": device}


class Saber:
    def __init__(self, device, dtr, rts):
        self.device, self.dtr, self.rts = device, dtr, rts
        self.ser = None

    def open(self):
        # come pyserial in py2saber: DTR e RTS asseriti all'apertura (salvo --no-dtr / --no-rts)
        ser = serial.Serial()
        ser.port = self.device
        ser.baudrate = 1152000
        ser.timeout = 0.05
        ser.dtr = self.dtr
        ser.rts = self.rts
        ser.open()
        time.sleep(0.2)
        ser.reset_input_buffer()
        self.ser = ser

    def close(self):
        if self.ser:
            self.ser.close()
            self.ser = None

    def reopen(self, timeout=REOPEN_TIMEOUT):
        self.close()
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.open()
                return
            except serial.SerialException:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.5)

    def send(self, cmd, timeout=REPLY_TIMEOUT):
        """Manda cmd e ritorna (byte ricevuti, secondi al primo byte o None)."""
        self.ser.reset_input_buffer()
        start = time.monotonic()
        self.ser.write(cmd)
        self.ser.flush()
        data = bytearray()
        first = None
        last = start
        while True:
            # read(1) ritorna appena arriva un byte: il tempo al primo byte non dipende dal timeout
            chunk = self.ser.read(1)
            if chunk:
                chunk += self.ser.read(self.ser.in_waiting)
            now = time.monotonic()
            if chunk:
                if first is None:
                    first = now - start
                data += chunk
                last = now
            elif data and now - last >= IDLE_GAP:
                break
            elif not data and now - start >= timeout:
                break
        return bytes(data), first

    def read_main_colors(self):
        data, _ = self.send(b"c?\n")
        colors = {}
        for line in data.decode("ascii", "replace").splitlines():
            m = COLOR_LINE.match(line.strip())
            if m:
                colors[int(m.group(2))] = tuple(int(m.group(i)) for i in range(3, 7))
        return colors

    def set_main_color(self, bank, color):
        cmd = "C{}={},{},{},{}\n".format(bank, *color).encode()
        return self.send(cmd)


def ask_yes_no(question):
    while True:
        answer = input(f"{question} [s/n] ").strip().lower()
        if answer in ("s", "si", "sì", "y"):
            return "sì"
        if answer == "n":
            return "no"


def md(raw):
    """Byte grezzi come in Python, pronti per una cella di tabella Markdown."""
    return "`" + repr(raw)[2:-1].replace("|", "\\|").replace("`", "'") + "`" if raw else "*(nessuna)*"


def restore(saber, backup, log):
    current = saber.read_main_colors()
    changed = [b for b, color in backup.items() if current.get(b) != color]
    for bank in changed:
        saber.set_main_color(bank, backup[bank])
    if changed:
        saber.send(b"SAVE\n")
    after = saber.read_main_colors()
    ok = all(after.get(b) == c for b, c in backup.items())
    log(f"Ripristino dei bank {changed or 'nessuno'}: {'riuscito' if ok else 'NON riuscito, controlla con c?'}")
    return ok


def run_probe(saber, rows):
    for item in PROBE:
        data, first = saber.send(item["cmd"])
        text_ok = re.fullmatch(item["expect"], data.decode("latin-1"), re.S) is not None
        row = {
            "cmd": item["cmd"],
            "reply": data,
            "first_byte_ms": None if first is None else round(first * 1000),
            "as_emulator": text_ok,
            "note": item.get("note", ""),
        }
        print(f"\n> {item['cmd']!r}\n< {data!r}")
        print("  come l'emulatore" if text_ok else "  DIVERSO dall'emulatore")
        if "observe" in item:
            row["observed"] = ask_yes_no("  " + item["observe"])
        rows.append(row)


def run_persistence(saber, log):
    result = {}
    log("\nProva di persistenza: bank 0 con SAVE, bank 1 senza SAVE.")
    saber.set_main_color(0, SAVE_COLOR)
    saber.send(b"SAVE\n")
    saber.set_main_color(1, NOSAVE_COLOR)
    before = saber.read_main_colors()
    result["before"] = before
    if before.get(0) != SAVE_COLOR or before.get(1) != NOSAVE_COLOR:
        log(f"I colori di prova non risultano impostati (c? = {before}): prova annullata.")
        return result

    saber.close()
    input("\nSpegni la spada con il suo interruttore, aspetta qualche secondo, riaccendila "
          "(cavo sempre collegato), poi premi Invio... ")
    print("Riapro la porta...")
    saber.reopen()
    after = saber.read_main_colors()
    result["after"] = after
    result["with_save"] = after.get(0) == SAVE_COLOR
    result["without_save"] = after.get(1) == NOSAVE_COLOR
    log(f"Dopo lo spegnimento: bank 0 (con SAVE) {after.get(0)} -> "
        f"{'mantenuto' if result['with_save'] else 'PERSO'}; "
        f"bank 1 (senza SAVE) {after.get(1)} -> {'mantenuto' if result['without_save'] else 'perso'}")
    return result


def write_report(path, info, args, rows, persistence, restored, notes):
    stamp = dt.datetime.now().strftime("%Y-%m-%d %H:%M")
    out = [
        "# F0: risposte reali della spada",
        "",
        f"Data: {stamp}. Porta: `{info.get('device')}`, {info.get('model', '?')} {info.get('vid_pid', '')}.",
        f"DTR {'asserito' if args.dtr else 'non asserito'}, RTS {'asserito' if args.rts else 'non asserito'}.",
        "",
        "## Comandi",
        "",
        "Risposte come byte grezzi. \"Emulatore\" dice se la risposta coincide con quella della variante fake.",
        "",
        "| Comando | Risposta | Primo byte | Emulatore | Osservato | Note |",
        "|---|---|---|---|---|---|",
    ]
    for r in rows:
        ms = "—" if r["first_byte_ms"] is None else f"{r['first_byte_ms']} ms"
        out.append(f"| {md(r['cmd'])} | {md(r['reply'])} | {ms} | {'sì' if r['as_emulator'] else '**no**'} "
                   f"| {r.get('observed', '')} | {r['note']} |")
    out += ["", "## Persistenza", ""]
    if not persistence:
        out.append("Prova saltata.")
    elif "after" not in persistence:
        out.append(f"Prova annullata: colori di prova non impostati ({persistence.get('before')}).")
    else:
        out.append(f"- Bank 0 impostato a {SAVE_COLOR} + `SAVE`, dopo lo spegnimento: "
                   f"{persistence['after'].get(0)} ({'mantenuto' if persistence['with_save'] else '**perso**'})")
        out.append(f"- Bank 1 impostato a {NOSAVE_COLOR} senza `SAVE`, dopo lo spegnimento: "
                   f"{persistence['after'].get(1)} ({'mantenuto' if persistence['without_save'] else 'perso'})")
    out += ["", f"Ripristino dei colori originali: {'riuscito' if restored else '**non riuscito**'}.", ""]
    out += ["## Porta USB (pyserial)", "", "```", json.dumps(info, indent=2, ensure_ascii=False), "```", ""]
    out += ["## Da completare con USBTreeView", "",
            "- Interfaccia CDC di comunicazione (classe 02): numero",
            "- Interfaccia CDC dati (classe 0A): numero",
            "- Endpoint bulk IN/OUT: indirizzo e dimensione massima del pacchetto",
            "- Velocità (full-speed?)", ""]
    if notes:
        out += ["## Log", "", "```", *notes, "```", ""]
    path.write_text("\n".join(out), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="porta seriale della spada (default: trovata da VID:PID)")
    ap.add_argument("--out", default=".", help="cartella per il report e il backup (default: quella corrente)")
    ap.add_argument("--skip-persistence", action="store_true", help="salta la prova di spegnimento")
    ap.add_argument("--no-dtr", dest="dtr", action="store_false", help="apri la porta senza DTR")
    ap.add_argument("--no-rts", dest="rts", action="store_false", help="apri la porta senza RTS")
    ap.add_argument("--restore", metavar="BACKUP", help="rimetti i colori di un file di backup ed esci")
    args = ap.parse_args()

    device = args.port or find_port()
    saber = Saber(device, args.dtr, args.rts)
    saber.open()
    notes = []

    def log(msg):
        print(msg)
        notes.append(msg.strip())

    if args.restore:
        backup = {int(k): tuple(v) for k, v in json.loads(Path(args.restore).read_text()).items()}
        ok = restore(saber, backup, log)
        saber.close()
        sys.exit(0 if ok else 1)

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup_path = out_dir / f"f0-backup-{stamp}.json"
    report_path = out_dir / f"f0-report-{stamp}.md"
    info = port_info(device)
    print(f"Spada su {device}: {info.get('model', '?')} {info.get('vid_pid', '')}")

    backup = saber.read_main_colors()
    if len(backup) != 8:
        saber.close()
        sys.exit(f"c? non ha restituito 8 colori ({backup}): la spada risponde? Nessuna modifica fatta.")
    backup_path.write_text(json.dumps(backup, indent=2), encoding="utf-8")
    print(f"Colori principali salvati in {backup_path}")

    rows, persistence, restored = [], {}, False
    try:
        run_probe(saber, rows)
        if not args.skip_persistence:
            persistence = run_persistence(saber, log)
    except KeyboardInterrupt:
        log("Interrotto.")
    except serial.SerialException as e:
        log(f"Errore sulla seriale: {e}")
        try:
            saber.reopen(timeout=10)
        except serial.SerialException:
            pass
    finally:
        if saber.ser:
            restored = restore(saber, backup, log)
            saber.close()
        else:
            log(f"Porta non disponibile: ripristina con  python tools/f0_probe.py --restore {backup_path}")
        write_report(report_path, info, args, rows, persistence, restored, notes)
        print(f"\nReport: {report_path}")


if __name__ == "__main__":
    main()
