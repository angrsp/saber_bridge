#!/usr/bin/env python3
"""F5: prova il bridge via BLE come lo userà l'app (piano-FW-bridge.md, fase F5 e sezione 3).

Trova il bridge (UUID del NUS o nome), legge DIS e stato, poi:
  1. manda una volta ciascuno i comandi di sola lettura e registra le risposte;
  2. chiede una risposta lunga (c?, f? e w? in una sola scrittura) che deve arrivare su più notifiche;
  3. manda N comandi di fila (default 500), uno alla volta come l'app, e misura la latenza;
  4. rilegge lo stato: il flag "byte persi" deve essere a zero.
Scrive un report bridge-test-<data>.md. Manda solo comandi di lettura: i colori della spada non cambiano.

La latenza è quella che vede l'app: dalla scrittura su RX (con risposta) all'ultima riga della risposta.

Uso:
    pip install bleak
    python tools/bridge_test.py                      # primo bridge trovato
    python tools/bridge_test.py --address AA:BB:...  # un bridge preciso
    python tools/bridge_test.py --count 100
    python tools/bridge_test.py --serial COM5        # senza BLE: la spada direttamente sulla seriale,
                                                     # per confrontare le latenze (serve pyserial)
"""

import argparse
import asyncio
import datetime as dt
import importlib.util
import re
import statistics
import struct
import sys
import threading
import time
from pathlib import Path

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
STATUS_CHAR = "7a6f0002-3c1d-4b8e-9f2a-6d5e4c3b2a10"
DIS_MODEL = "00002a24-0000-1000-8000-00805f9b34fb"
DIS_FIRMWARE = "00002a26-0000-1000-8000-00805f9b34fb"

ATT_ERRORS = {0x80: "spada non collegata", 0x81: "buffer verso la spada pieno"}

REPLY_TIMEOUT = 3.0   # come il timeout di lettura di py2saber e dell'app
SCAN_TIMEOUT = 10.0


class Cmd:
    def __init__(self, text, line_re, lines=1):
        self.text, self.line_re, self.lines = text, line_re, lines


def bank_lines(kind):
    return Cmd(kind + "?", kind + r"[0-7]=\d+,\d+,\d+,\d+", 8)


# Comandi di sola lettura, con la forma delle righe di risposta (sezione 6 del piano)
READ_ONLY = [
    Cmd("WR?", r"OK, Write Ready"),
    Cmd("V?", r"V=\S+"),
    Cmd("S?", r"S=[0-9a-f]+"),
    Cmd("B?", r"B=[0-7]"),
    Cmd("c0?", r"c0=\d+,\d+,\d+,\d+"),
    bank_lines("c"),
    bank_lines("f"),
    bank_lines("w"),
]
LONG = [bank_lines("c"), bank_lines("f"), bank_lines("w")]   # in una sola scrittura: circa 400 byte


def decode_status(data):
    if data is None or len(data) < 8:
        return None
    version, flags, vid, pid, mv = struct.unpack_from("<BBHHH", data)
    return {"version": version, "saber": bool(flags & 1), "lost": bool(flags & 2),
            "battery_low": bool(flags & 4), "vid": vid, "pid": pid, "mv": mv}


def status_text(st):
    if st is None:
        return "—"
    saber = f"spada {st['vid']:04X}:{st['pid']:04X}" if st["saber"] else "spada assente"
    battery = f"{st['mv']} mV" if st["mv"] else "batteria non misurata"
    flags = [f for f, on in (("byte persi", st["lost"]), ("batteria bassa", st["battery_low"])) if on]
    return f"formato {st['version']}, {saber}, {battery}" + (", " + ", ".join(flags) if flags else "")


def att_error(exc):
    """Codice ATT applicativo (0x80, 0x81) dell'errore di bleak, se c'è."""
    code = getattr(exc, "code", None)   # BleakGATTProtocolError, da bleak 3.0
    if isinstance(code, int):
        return int(code)
    m = re.search(r"0x(8[0-9a-f])\b", str(exc), re.I)   # bleak più vecchi: solo nel messaggio
    return int(m.group(1), 16) if m else None


class Replies:
    """Rimette insieme le notifiche in righe e scarta le righe "#" (log della spada, o della variante fake)."""

    def __init__(self):
        self.buf = bytearray()
        self.lines = asyncio.Queue()
        self.notifications = 0
        self.received = 0
        self.debug_lines = []

    def feed(self, data):
        self.notifications += 1
        self.received += len(data)
        self.buf += data
        while True:
            i = self.buf.find(b"\n")
            if i < 0:
                return
            line = bytes(self.buf[:i]).decode("latin-1")
            del self.buf[:i + 1]
            if line.startswith("#"):
                self.debug_lines.append(line)
            else:
                self.lines.put_nowait(line)

    async def drain(self, quiet=0.3):
        """Aspetta il silenzio e butta le righe arrivate (risposte in ritardo dopo un timeout)."""
        while True:
            seen = self.received
            await asyncio.sleep(quiet)
            if self.received == seen:
                break
        while not self.lines.empty():
            self.lines.get_nowait()
        self.buf.clear()


class BleLink:
    def __init__(self, args):
        self.args = args
        self.client = None
        self.info = {}
        self.status_events = []
        self.disconnected = False

    async def open(self, on_data):
        from bleak import BleakClient, BleakScanner

        if self.args.address:
            device = await BleakScanner.find_device_by_address(self.args.address, timeout=SCAN_TIMEOUT)
        else:
            def match(d, adv):
                uuids = [u.lower() for u in adv.service_uuids]
                return NUS_SERVICE in uuids or (adv.local_name or d.name) == self.args.name
            device = await BleakScanner.find_device_by_filter(match, timeout=SCAN_TIMEOUT)
        if device is None:
            sys.exit("Nessun bridge trovato: è acceso e in advertising (LED a lampeggio lento)?")
        # trovato dal primo pacchetto di advertising: il nome sta nella scan response e può mancare
        name = device.name or "nome non ricevuto"
        print(f"Bridge: {name} {device.address}. Connessione...")

        def gone(_):
            self.disconnected = True
            print("\nBridge disconnesso.")

        self.client = BleakClient(device, disconnected_callback=gone)
        t0 = time.perf_counter()
        await self.client.connect()
        rx = self.client.services.get_characteristic(NUS_RX)
        self.max_write = rx.max_write_without_response_size
        self.max_notify = self.client.mtu_size - 3
        self.info = {
            "Bridge": f"{name} ({device.address})",
            "Connessione": f"{(time.perf_counter() - t0) * 1000:.0f} ms",
            "MTU": f"{self.client.mtu_size} (scrittura massima {self.max_write} byte)",
            "Model Number": await self.read_text(DIS_MODEL),
            "Firmware Revision": await self.read_text(DIS_FIRMWARE),
        }
        await self.client.start_notify(NUS_TX, lambda _, data: on_data(bytes(data)))
        await self.client.start_notify(STATUS_CHAR, self.on_status)

    async def read_text(self, uuid):
        try:
            return (await self.client.read_gatt_char(uuid)).decode("utf-8", "replace")
        except Exception as e:  # noqa: BLE001 (bleak alza eccezioni diverse a seconda del sistema)
            return f"non letto ({e})"

    def on_status(self, _, data):
        st = decode_status(bytes(data))
        self.status_events.append((time.monotonic(), st))
        print(f"  [stato] {status_text(st)}")

    async def status(self):
        return decode_status(bytes(await self.client.read_gatt_char(STATUS_CHAR)))

    async def write(self, data):
        # write con risposta, come l'app (3.2): ordine e controllo di flusso garantiti
        for i in range(0, len(data), self.max_write):
            await self.client.write_gatt_char(NUS_RX, data[i:i + self.max_write], response=True)

    async def close(self):
        if self.client and self.client.is_connected:
            await self.client.disconnect()


class SerialLink:
    """Al posto del BLE, la spada (o l'emulatore su una pty) sulla seriale. I byte ricevuti vengono
    spezzati in "notifiche" da --chunk byte, come farebbe il bridge con l'MTU."""

    def __init__(self, args):
        self.args = args
        self.max_write = self.max_notify = args.chunk
        self.status_events = []
        self.disconnected = False
        self.stop = threading.Event()

    async def open(self, on_data):
        try:
            import serial
        except ImportError:
            sys.exit("Serve pyserial: pip install pyserial")
        # come pyserial in py2saber: DTR e RTS asseriti all'apertura
        self.ser = serial.Serial(self.args.serial, 1152000, timeout=0.05)
        self.info = {"Seriale": f"{self.args.serial} (senza bridge, notifiche da {self.args.chunk} byte)"}
        loop = asyncio.get_running_loop()

        def reader():
            while not self.stop.is_set():
                data = self.ser.read(1)
                if data:
                    data += self.ser.read(self.ser.in_waiting)
                for i in range(0, len(data), self.max_notify):
                    loop.call_soon_threadsafe(on_data, data[i:i + self.max_notify])

        self.thread = threading.Thread(target=reader, daemon=True)
        self.thread.start()

    async def status(self):
        return None

    async def write(self, data):
        self.ser.write(data)
        self.ser.flush()

    async def close(self):
        self.stop.set()
        self.thread.join()
        self.ser.close()


async def transact(link, replies, cmds, timeout):
    """Manda i comandi in una sola scrittura e aspetta tutte le righe di risposta.
    Ritorna un dict con righe, latenza, notifiche, esito ed eventuale errore."""
    expected = [c.line_re for c in cmds for _ in range(c.lines)]
    payload = "".join(c.text + "\n" for c in cmds).encode()
    n0, b0 = replies.notifications, replies.received
    t0 = time.perf_counter()
    result = {"cmd": " ".join(c.text for c in cmds), "lines": [], "error": None}
    try:
        await link.write(payload)
        deadline = t0 + timeout
        while len(result["lines"]) < len(expected):
            left = deadline - time.perf_counter()
            if left <= 0:
                raise asyncio.TimeoutError
            result["lines"].append(await asyncio.wait_for(replies.lines.get(), left))
    except asyncio.TimeoutError:
        result["error"] = f"timeout: {len(result['lines'])} righe su {len(expected)}"
    except Exception as e:  # noqa: BLE001 (errori di scrittura di bleak o della seriale)
        code = att_error(e)
        result["error"] = f"errore ATT 0x{code:02X} ({ATT_ERRORS.get(code, '?')})" if code else f"scrittura: {e}"
        result["att"] = code
    result["ms"] = (time.perf_counter() - t0) * 1000
    result["notifications"] = replies.notifications - n0
    result["bytes"] = replies.received - b0
    if result["error"] is None:
        bad = [l for l, r in zip(result["lines"], expected) if not re.fullmatch(r, l)]
        if bad:
            result["error"] = f"risposta inattesa: {bad[0]!r}"
    if result["error"]:
        await replies.drain()
    return result


async def run_sequence(link, replies, count, timeout):
    results = []
    for i in range(count):
        if link.disconnected:
            break
        r = await transact(link, replies, [READ_ONLY[i % len(READ_ONLY)]], timeout)
        results.append(r)
        if r["error"]:
            print(f"  {i + 1}: {r['cmd']} -> {r['error']}")
        if (i + 1) % 50 == 0:
            done = [x["ms"] for x in results if not x["error"]]
            mean = statistics.mean(done) if done else 0
            print(f"  {i + 1}/{count}, errori {sum(1 for x in results if x['error'])}, media {mean:.1f} ms")
    return results


def latency_stats(values):
    if not values:
        return None
    q = statistics.quantiles(values, n=20) if len(values) >= 2 else values
    return {"n": len(values), "mean": statistics.mean(values), "median": statistics.median(values),
            "p95": q[-1], "max": max(values), "min": min(values)}


def md(text):
    return "`" + repr(text)[1:-1].replace("|", "\\|").replace("`", "'") + "`" if text else "*(nessuna)*"


def write_report(path, link, start_status, end_status, singles, long_reply, seq, count, debug_lines, verdict):
    out = ["# F5: prova del bridge", "",
           f"Data: {dt.datetime.now():%Y-%m-%d %H:%M}.", ""]
    out += [f"- {k}: {v}" for k, v in link.info.items()]
    out += [f"- Stato all'inizio: {status_text(start_status)}",
            f"- Stato alla fine: {status_text(end_status)}", ""]

    out += ["## Esito", "", verdict, ""]

    out += ["## Comandi singoli", "", "| Comando | Risposta | Latenza | Notifiche | Esito |", "|---|---|---|---|---|"]
    for r in singles:
        out.append(f"| `{r['cmd']}` | {md(chr(10).join(r['lines']))} | {r['ms']:.1f} ms | {r['notifications']} "
                   f"| {r['error'] or 'ok'} |")

    out += ["", "## Risposta lunga", ""]
    if long_reply:
        need = -(-long_reply["bytes"] // link.max_notify) if long_reply["bytes"] else 0
        out += [f"`{long_reply['cmd']}` in una sola scrittura: {len(long_reply['lines'])} righe, "
                f"{long_reply['bytes']} byte in {long_reply['notifications']} notifiche "
                f"(almeno {need} da {link.max_notify} byte), {long_reply['ms']:.1f} ms. "
                f"Esito: {long_reply['error'] or 'ok'}."]
    else:
        out.append("Non eseguita.")

    out += ["", "## Sequenza", ""]
    errors = [r for r in seq if r["error"]]
    stats = latency_stats([r["ms"] for r in seq if not r["error"]])
    out.append(f"{len(seq)} comandi su {count}, {len(errors)} errori.")
    if stats:
        out += ["", "| | Latenza |", "|---|---|"]
        out += [f"| {label} | {stats[key]:.1f} ms |" for label, key in
                (("minima", "min"), ("media", "mean"), ("mediana", "median"), ("95%", "p95"), ("massima", "max"))]
        out += ["", "Per comando:", "", "| Comando | N | Media | Massima |", "|---|---|---|---|"]
        for c in READ_ONLY:
            ms = [r["ms"] for r in seq if not r["error"] and r["cmd"] == c.text]
            if ms:
                out.append(f"| `{c.text}` | {len(ms)} | {statistics.mean(ms):.1f} ms | {max(ms):.1f} ms |")
    if errors:
        out += ["", "Primi errori:", ""]
        out += [f"- `{r['cmd']}`: {r['error']}" for r in errors[:20]]

    if link.status_events:
        out += ["", "## Notifiche di stato", ""]
        t0 = link.status_events[0][0]
        out += [f"- +{t - t0:.1f} s: {status_text(st)}" for t, st in link.status_events]
    if debug_lines:
        out += ["", "## Righe `#` scartate", "", f"{len(debug_lines)}, per esempio: {md(debug_lines[0])}"]
    path.write_text("\n".join(out) + "\n", encoding="utf-8")


async def main_async(args):
    replies = Replies()
    link = SerialLink(args) if args.serial else BleLink(args)
    await link.open(replies.feed)
    singles, long_reply, seq, start_status, end_status = [], None, [], None, None
    verdict = "**Non superata**: prova interrotta."
    try:
        for k, v in link.info.items():
            print(f"{k}: {v}")
        start_status = await link.status()
        if start_status is not None:
            print(f"Stato: {status_text(start_status)}")
        await replies.drain()

        if start_status is not None and not start_status["saber"]:
            # senza spada il bridge deve rifiutare la scrittura con 0x80 (3.2)
            r = await transact(link, replies, [READ_ONLY[0]], args.timeout)
            singles.append(r)
            verdict = ("Spada assente: la scrittura è stata rifiutata con 0x80, come da contratto. "
                       "Collega la spada (o `plug` nella variante fake) e rilancia."
                       if r.get("att") == 0x80 else
                       f"**Spada assente, ma la scrittura non è stata rifiutata con 0x80**: {r['error'] or 'risposta ricevuta'}.")
            print(verdict)
        else:
            print("\nComandi singoli:")
            for c in READ_ONLY:
                r = await transact(link, replies, [c], args.timeout)
                singles.append(r)
                print(f"  {c.text}: {r['ms']:.1f} ms, {r['notifications']} notifiche, {r['error'] or 'ok'}")

            print("\nRisposta lunga:")
            long_reply = await transact(link, replies, LONG, args.timeout)
            print(f"  {long_reply['bytes']} byte in {long_reply['notifications']} notifiche, "
                  f"{long_reply['ms']:.1f} ms, {long_reply['error'] or 'ok'}")

            print(f"\nSequenza di {args.count} comandi:")
            seq = await run_sequence(link, replies, args.count, args.timeout)

            end_status = None if link.disconnected else await link.status()
            failed = [r for r in singles + [long_reply] + seq if r["error"]]
            long_split = long_reply["bytes"] <= link.max_notify or long_reply["notifications"] >= 2
            ok = (not failed and len(seq) == args.count and long_split
                  and (end_status is None or not end_status["lost"]))
            stats = latency_stats([r["ms"] for r in seq if not r["error"]])
            verdict = ("**Superata**" if ok else "**Non superata**") + (
                f": {len(seq)} comandi, {len(failed)} errori in tutto"
                + (f", latenza media {stats['mean']:.1f} ms, massima {stats['max']:.1f} ms" if stats else "")
                + ("" if end_status is None else f", byte persi: {'sì' if end_status['lost'] else 'no'}")
                + ("" if long_split else ", la risposta lunga non è arrivata su più notifiche")
                + ".")
            print("\n" + verdict.replace("**", ""))
    finally:
        await link.close()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"bridge-test-{dt.datetime.now():%Y%m%d-%H%M%S}.md"
    write_report(path, link, start_status, end_status, singles, long_reply, seq, args.count,
                 replies.debug_lines, verdict)
    print(f"Report: {path}")
    return verdict.startswith("**Superata")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--address", help="indirizzo del bridge (default: il primo che pubblica il NUS)")
    ap.add_argument("--name", default="SaberBridge", help="nome del bridge, se non pubblica il NUS")
    ap.add_argument("--count", type=int, default=500, help="comandi della sequenza (default 500)")
    ap.add_argument("--timeout", type=float, default=REPLY_TIMEOUT, help="attesa massima di una risposta, in s")
    ap.add_argument("--out", default=".", help="cartella del report (default: quella corrente)")
    ap.add_argument("--serial", metavar="PORTA", help="niente BLE: la spada (o l'emulatore) su questa seriale")
    ap.add_argument("--chunk", type=int, default=244, help="con --serial: byte per notifica (default 244, MTU 247)")
    args = ap.parse_args()
    if not args.serial and importlib.util.find_spec("bleak") is None:
        sys.exit("Serve bleak: pip install bleak")
    sys.exit(0 if asyncio.run(main_async(args)) else 1)


if __name__ == "__main__":
    main()
