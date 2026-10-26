# Piano di sviluppo: firmware del bridge BLE ↔ USB (XIAO ESP32-S3)

Ultimo aggiornamento: 2026-10-26.
File collegati, nella cartella del progetto (`..` rispetto a questo repo): `ESP32-bridge-note.md` (architettura e scheletro del firmware), `saber_manager-note-app-BLE.md` (note sull'app), `piano-app-BLE.md` (piano dell'app).

Questo piano contiene il **contratto BLE condiviso** con l'app (sezione 3): è la fonte unica, il piano dell'app lo richiama e non lo ripete.

## Stato delle fasi

| Fase | Stato | Chiusa | Merge in `develop` | Note |
|---|---|---|---|---|
| F0 – Verifiche su PC | ✅ conclusa | 2026-10-25 | `0230d0d` | risultati nella sezione 6; fatta con `tools/f0_probe.py` invece di py2saber |
| F1 – Toolchain e build | ✅ conclusa | 2026-10-25 | `911f564` | ESP-IDF v5.5.6 via Docker (`tools/idf-docker.sh`), non con l'installer Windows |
| F2 – Robustezza | ✅ conclusa | 2026-10-25 | `d82cbc0` | |
| F3 – Stato e DIS | ✅ conclusa | 2026-10-25 | `975004c` | UUID fissati (3.3) |
| F4 – Fake saber | ✅ conclusa | 2026-10-25 | `659b842` | emulatore allineato alle risposte reali di F0 |
| F5 – Bring-up hardware | 🔧 in corso | | | `tools/bridge_test.py` pronto (provato solo con `--serial` contro l'emulatore); per chiudere serve la XIAO |
| F6 – Bridge con la NXT | ⏳ in attesa | | | servono XIAO, LiPo, OTG |
| F7–F10 | ⏳ in attesa | | | |

Nessuna fase è ancora provata su hardware reale: F1–F4 sono verificate con la build (0 warning) e con le prove sul PC (`tools/test-host.sh`).

## 1. Punto di partenza

- Spada: **Anima NXT** (STM32, VID 0483 / PID 5740; `V?` risponde `V=NXT_...`).
- Hardware: **ancora da ordinare** (XIAO ESP32-S3, LiPo con PCM, adattatore OTG, adattatore USB-UART). Le fasi F0–F4 si fanno senza.
- `saber_bridge/` esisteva vuota e ESP-IDF non era nel PATH (all'inizio; ora vedi "Stato delle fasi").
- La caratteristica di stato del bridge è **nella v1**.

### 1.1 Fatti verificati che correggono le note

Fonti: `LamaDiLuce/polaris-opencore/Documentation/COMMS-PROTOCOL.md` (protocollo ufficiale) e `angrsp/py2saber/py2saber/py2saber.py`.

| Argomento | Le note dicevano | Fatto |
|---|---|---|
| Handshake | `WR?` oppure `\`, da chiarire | `WR?` → `OK, Write Ready\n` |
| Salvataggio | `S` | `SAVE` → `OK SAVE\n`. `S` non esiste |
| Comandi sconosciuti | — | **nessuna risposta**: per l'app un timeout vuol dire "comando non supportato" |
| Indice dei bank | 0 o 1? | 0–7 per `C/F/W`, `B=`, `B?` |
| Lettura colori | forse non esiste | `c?`, `f?`, `w?` (8 righe `cN=r,g,b,w`), `B?` → `B=x`; `w?` richiede la build ≥ 1.9.15 |
| Righe `#` | da scartare | confermato: log del firmware in modalità debug |
| Scrittura NXT | — | al massimo 128 byte per scrittura (`_CHUNK_SIZE` di py2saber) |
| DTR/RTS | da asserire | pyserial apre con DTR e RTS asseriti: il bridge fa lo stesso (opzione Kconfig per provarli separati) |

**Rischio VBUS.** Il protocollo dice che la spada entra in "configuration mode" alla prima comunicazione e ne esce, salvando, quando rileva la fine della carica. Con il rosso tagliato la carica non c'è mai. Va verificato in F0 che `SAVE` renda persistenti i colori e che l'anteprima `P=` funzioni. **Verificato in F0:** senza VBUS `SAVE` rende persistenti i colori e `P=` funziona (sezione 6).

## 2. Flusso di lavoro: gitflow

Il firmware ha un repository proprio, `saber_bridge/`, separato dall'app.

### 2.1 Rami

| Ramo | Da dove nasce | Dove confluisce | Contenuto |
|---|---|---|---|
| `main` | — | — | solo versioni rilasciate, ognuna con un tag `vX.Y.Z` |
| `develop` | `main` | — | integrazione: stato più recente che compila |
| `feature/<nome>` | `develop` | `develop` (`--no-ff`) | una fase o parte di fase (sezione 4) |
| `release/X.Y.Z` | `develop` | `main` + tag, poi `develop` | solo numero di versione, correzioni e documentazione |
| `hotfix/X.Y.Z` | `main` | `main` + tag, poi `develop` | correzione urgente su una versione rilasciata |

Regole:
- Nessun commit diretto su `main` né su `develop`: si passa sempre da un ramo `feature/`, `release/` o `hotfix/`.
- Un ramo `feature/` si chiude solo con `idf.py build` pulito (entrambe le varianti, reale e fake, quando la variante fake esiste) e con il criterio di uscita della fase raggiunto.
- Versione con SemVer, in `PROJECT_VER` del `CMakeLists.txt` radice (letta da `esp_app_get_description()` e pubblicata nel DIS, vedi 3.4). Si cambia solo nei rami `release/` e `hotfix/`.
- Le modifiche al contratto (sezione 3) si fanno in un ramo `feature/` dedicato, aggiornano questo file e incrementano il byte "versione formato" del payload di stato.

### 2.2 Inizializzazione

Con git-flow (AVH 1.12.3, già installato):

```
cd saber_bridge
git init
# primo commit su main: README minimo e .gitignore (build/, managed_components/, sdkconfig, sdkconfig.old, dependencies.lock solo se non lo si vuole versionare)
git flow init -d          # main, develop, feature/, release/, hotfix/, tag con prefisso "v"
```

Equivalente senza git-flow:

```
git switch -c develop
git switch -c feature/skeleton-build develop
# ...lavoro...
git switch develop && git merge --no-ff feature/skeleton-build && git branch -d feature/skeleton-build
```

Remoto: da creare (per esempio `angrsp/saber_bridge` su GitHub) e collegare prima del primo push; finché non c'è, si lavora in locale.

### 2.3 Rilasci previsti

| Versione | Quando | Contenuto |
|---|---|---|
| `v0.1.0` | dopo F7 | primo bridge utilizzabile con la NXT e con l'app |
| `v0.2.0` | dopo F8 | power management |
| `v1.0.0` | dopo F9 e F10 | pairing con bonding, documentazione completa |

Ciclo di rilascio: `git flow release start 0.1.0` → aggiornare `PROJECT_VER`, README, tabella del contratto → build e prova completa (F7) → `git flow release finish 0.1.0` (merge in `main`, tag `v0.1.0`, merge in `develop`).

## 3. Contratto BLE condiviso (v1)

### 3.1 Advertising e connessione
- Nome `SaberBridge` nella scan response; UUID 128 bit del NUS nel pacchetto di advertising (l'app filtra per UUID).
- Una sola connessione alla volta; alla disconnessione si riparte con l'advertising.
- MTU preferita 247. Dopo la connessione il bridge chiede un intervallo di connessione di 15–30 ms (latenza delle richieste/risposte).

### 3.2 Nordic UART Service
- Servizio `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`.
- RX `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`: write e write-without-response; al massimo (MTU − 3) byte per scrittura; niente long write. **L'app usa la write con risposta**, che garantisce ordine e controllo di flusso.
- TX `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`: notify; frammenti fino a (MTU − 3) byte; byte grezzi della spada, senza framing.
- Errori ATT applicativi restituiti dalla write su RX:

| Codice | Significato |
|---|---|
| `0x80` | spada non collegata: i byte non vengono inoltrati |
| `0x81` | buffer verso la spada pieno: riprovare più tardi |

### 3.3 Servizio di stato del bridge
UUID confermati a fine F3 (2026-10-25), ora fissi:
- Servizio `7A6F0001-3C1D-4B8E-9F2A-6D5E4C3B2A10`
- Caratteristica di stato `7A6F0002-3C1D-4B8E-9F2A-6D5E4C3B2A10`: read + notify.

Payload, 8 byte little-endian:

| Offset | Tipo | Campo |
|---|---|---|
| 0 | `u8` | versione del formato (= 1) |
| 1 | `u8` | flag: bit0 spada collegata, bit1 byte persi dall'ultima lettura, bit2 batteria bassa |
| 2 | `u16` | VID della spada (0 se non collegata) |
| 4 | `u16` | PID della spada (0 se non collegata) |
| 6 | `u16` | tensione batteria in mV (0 = non misurata) |

Notifica a ogni cambio di stato della spada o dei flag, e ogni 30 s per la batteria (solo se la lettura della batteria è attiva). Una notifica parte anche quando il client si iscrive, con lo stato corrente. L'app deve ignorare i byte oltre l'ottavo (estensioni future compatibili).

Il bit1 si azzera quando lo stato che lo riporta arriva al client, con una lettura o con una notifica. Il bit2 ha un'isteresi di 100 mV sopra la soglia.

### 3.4 Device Information Service (`0x180A`)
- Firmware Revision String `0x2A26`: `PROJECT_VER` (per esempio `0.1.0`).
- Model Number String `0x2A24`: `SaberBridge-S3`.

### 3.5 Comportamento
- A ogni cambio di stato USB (spada collegata/scollegata) o BLE (client connesso/disconnesso) i buffer vengono svuotati: nessun dato vecchio viene inoltrato.
- Il bridge non interpreta i byte: tutto il protocollo della spada vive nell'app.

## 4. Fasi

Per ogni fase: ramo, lavoro, criterio di uscita. Le fasi F0–F4 non richiedono hardware nuovo.

### F0 – Verifiche su PC con la NXT
Ramo: nessuno (il risultato va in questo file, sezione 6, con un `feature/f0-protocol-notes` se il repo esiste già). **Bloccante per la fase A3 dell'app.**

- py2saber da `angrsp/py2saber`, con il cavo col rosso tagliato: `py2saber -D -t "<comando>"` per `WR?`, `V?`, `S?`, `B?`, `c?`, `f?`, `w?`, `C0=255,0,0,0`, `P=0,0,255,0`, `P=0`, `SAVE`, e un comando inesistente (deve tacere).
- Persistenza: impostare un colore + `SAVE`, spegnere e riaccendere la spada, rileggere con `c?`. Ripetere **senza** `SAVE` per capire se la configuration mode salva da sola.
- Descrittori con USBTreeView: VID/PID, indice dell'interfaccia CDC (comunicazione e dati), dimensione degli endpoint, full-speed.
- Uscita: tabella "risposte reali NXT" compilata (sezione 6).

### F1 – Toolchain e build (`feature/skeleton-build`)
- ESP-IDF 5.x, ultima stabile, con l'installer Windows; `idf.py --version`.
- Inizializzazione gitflow (2.2); file della sezione 6 di `ESP32-bridge-note.md`; `PROJECT_VER` nel `CMakeLists.txt`.
- Allineare le firme agli header installati: `data_cb` (tipo di ritorno), `event->data.error`, `cdc_acm_host_open`, chi libera l'mbuf in `ble_gatts_notify_custom`, `ble_store_config_init`, `ble_hs_cfg.reset_cb`, nomi delle opzioni `CONFIG_BT_NIMBLE_ROLE_*`.
- In `sdkconfig.defaults` aggiungere `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`: la console secondaria USB-Serial-JTAG, attiva di default, confligge con l'host USB.
- Uscita: `idf.py build` senza errori né warning.

### F2 – Robustezza (`feature/robustness`)
- Svuotamento dei buffer senza violare "un lettore e un scrittore per stream buffer": contatore di generazione incrementato a ogni cambio di stato; ogni lettore scarta da sé i dati della generazione precedente.
- Write su RX: errore `0x80` se la spada non è collegata, `0x81` se il buffer è pieno, invece di perdere byte in silenzio. Contatore dei byte persi verso il BLE (flag bit1 dello stato).
- Apertura limitata ai VID/PID noti (0483:5740 NXT, 16C0:0483 EVO); opzione Kconfig per accettare qualsiasi dispositivo CDC.
- Riapertura automatica della spada con backoff (500 ms → 2 s).
- Menu Kconfig "Saber Bridge": nome BLE, DTR e RTS asseriti sì/no, VID/PID qualsiasi, variante fake (F4), livello di log.
- LED utente GPIO21 (attivo basso): lampeggio lento in advertising, veloce con client connesso e spada assente, fisso con tutto pronto.
- Uscita: build pulita; revisione del codice dei casi di disconnessione (USB e BLE) con i log.

### F3 – Servizio di stato e DIS (`feature/status-service`)
- Servizio di stato e DIS secondo 3.3 e 3.4; notifica sui cambi di stato.
- Batteria opzionale (Kconfig: partitore presente, GPIO di ADC1, rapporto del partitore): `adc_oneshot` + calibrazione, media su più campioni; condensatore da 100 nF sul partitore da 220 kΩ (alta impedenza). Soglia "batteria bassa" configurabile (default 3,4 V).
- Uscita: build pulita; UUID confermati e scritti nella sezione 3.3 (da qui l'app può implementare la lettura dello stato).

### F4 – Variante "fake saber" (`feature/fake-saber`)
- `CONFIG_BRIDGE_FAKE_SABER`: niente host USB; la console resta sulla USB della XIAO (log senza adattatore USB-UART).
- Piccolo emulatore della NXT dietro lo stesso stream buffer: `WR?`, `V?` (`V=NXT_fake`), `S?`, `B?`/`B=`, `c?`/`f?`/`w?`, `C/F/W<n>=`, `P=`, `SAVE`; silenzio sui comandi sconosciuti; ogni tanto una riga `#`. Risposte uguali a quelle reali registrate in F0.
- Lo stato riporta "spada collegata" con VID/PID della NXT, e un comando di debug simula lo scollegamento.
- Uscita: le due varianti compilano; con la variante fake l'app si può provare via BLE con la sola scheda (fase A6 dell'app).

### F5 – Bring-up dell'hardware (`feature/bridge-test-tool`)
Richiede: XIAO.
- Controllare la polarità dei pad batteria sul silkscreen e la presenza dell'antenna.
- Flash della variante fake; prova con nRF Connect: notifiche su TX, scrittura su RX di `57 52 3F 0A` (`WR?\n`) → `OK, Write Ready`; lettura dello stato e del DIS.
- Script `tools/bridge_test.py` (Python + `bleak`): scansione, richiesta/risposta, latenza media e massima, 500 comandi di fila, risposte su più notifiche.
- Uscita: 500 comandi senza perdite; latenza misurata e annotata.

### F6 – Bridge reale con la NXT (`feature/nxt-bringup`)
Richiede: XIAO, LiPo, adattatore OTG, cavo col rosso tagliato; meglio con l'adattatore USB-UART.
- Variante reale: enumerazione, VID/PID e interfaccia come in F0; `WR?`, `V?`, `c?` da nRF Connect e da `bridge_test.py`; scrittura di un colore + `SAVE` e verifica dopo lo spegnimento.
- Prove con DTR/RTS asseriti, solo DTR, nessuno (Kconfig): annotare cosa cambia.
- Log dall'adattatore USB-UART su D6/D7; se manca, LED e caratteristica di stato.
- Uscita: le risposte via bridge coincidono con la tabella di F0.

### F7 – Robustezza sul campo (`feature/field-robustness`, se servono correzioni)
- Spada spenta e riaccesa con il telefono connesso; telefono disconnesso a metà risposta; adattatore staccato e ricollegato; un'ora di comandi continui; 4 KB scritti di fila (deve rispondere con un errore, non bloccarsi).
- Obiettivi: riapertura della spada entro 2 s dall'accensione; notifica di stato entro 1 s da ogni cambio.
- Uscita: tutti i casi superati → **`release/0.1.0`**.

### F8 – Consumi e power management (`feature/power-management`)
- Misura della corrente dalla LiPo: in advertising, connesso, connesso con host USB attivo.
- Advertising lento a riposo; deep sleep dopo N minuti senza client (Kconfig); risveglio da pulsante; deep sleep sotto 3,3 V.
- Uscita: autonomia stimata con la LiPo scelta → **`release/0.2.0`**.

### F9 – Sicurezza (`feature/ble-pairing`)
- LE Secure Connections con bonding; nuovi bond accettati solo in una finestra di pairing (60 s dall'accensione o dopo la pressione di un pulsante); caratteristiche RX e stato con permesso cifrato.
- Da coordinare con la fase A7 dell'app (dialog di pairing Android, bond perso).
- Uscita: un telefono non associato non può scrivere su RX.

### F10 – Rilascio 1.0 (`release/1.0.0`)
- README: cablaggio (solo D+, D−, GND verso la spada), flash, recupero con il tasto BOOT, significato del LED, contratto BLE.
- OTA fuori dalla v1.

## 5. Sincronizzazione con l'app

| Quando | Firmware | App (`piano-app-BLE.md`) |
|---|---|---|
| Subito, senza hardware | F0, F1–F4 | A1–A5 (A3 conferma i fatti con F0) |
| Contratto fissato (fine F3) | UUID di stato definitivi | A4 legge il servizio di stato |
| Arriva la XIAO | F5 (variante fake) | A6, prima parte |
| Arrivano OTG e LiPo | F6–F7, `v0.1.0` | A6 con la NXT, poi la release `0.1.0` dell'app |
| Prima della versione stabile | F8–F9 | A7 |

## 6. Risposte reali NXT (F0)

Registrate il 2026-10-25 con `tools/f0_probe.py` (due giri identici): Anima NXT 0483:5740, cavo col rosso tagliato, DTR e RTS asseriti, terminatore LF. Tutte le risposte coincidono con l'emulatore della variante fake (`main/nxt_emu.c`). Tempo al primo byte tra 0 e 16 ms, tranne `SAVE` (313 ms).

| Comando | Risposta attesa (protocollo) | Risposta reale | Note |
|---|---|---|---|
| `WR?` | `OK, Write Ready` | `OK, Write Ready\n` | |
| `V?` | `V=NXT_...` | `V=NXT_1.1.0\n` | |
| `S?` | `S=...` | `S=5f00303931501920383758\n` | ID unico dell'STM32: tre parole da 32 bit in esadecimale minuscolo, senza zeri iniziali né separatori. Il seriale USB (`209737883931`) deriva dalle stesse parole |
| `B?` | `B=x` | `B=7\n` | |
| `c?` / `f?` / `w?` | 8 righe `cN=r,g,b,w` | 8 righe `c0=…`, `f0=…`, `w0=…` | **minuscole**, come la lettera del comando (uguale al sorgente EVO). `w?` risponde anche sulla NXT 1.1.0: il limite "build ≥ 1.9.15" vale solo per la EVO |
| `C0=255,0,0,0` | `OK C0=rrggbbww` | `OK C0=255,0,0,0\n` | eco del comando, uguale al sorgente EVO; `c0?` rilegge `c0=255,0,0,0` |
| `P=0,0,255,0` / `P=0` | `OK P=...` | `OK, P=0,0,255,0\n` / `OK, P=0\n` | con la virgola. La lama si accende di blu e si spegne **anche senza VBUS** |
| `SAVE` | `OK SAVE` | `OK SAVE\n` | primo byte dopo **313 ms** (scrittura in flash, presumibilmente): l'app non deve usare timeout più corti |
| comando inesistente | nessuna risposta | nessuna risposta | |
| `V?` con `\r\n` | — | nessuna risposta | il `\r` resta nel comando: l'app deve terminare con il solo LF |
| riga che inizia con `#` | — | `#`, senza LF | |
| persistenza dopo `SAVE` | colore mantenuto | mantenuto, senza VBUS | |
| persistenza senza `SAVE` | ? | mantenuto | ripetuto due volte; nella seconda la porta COM è sparita allo spegnimento (USB davvero scollegata). Non si sa se la spada salvi da sola allo spegnimento o se in "spento" resti in sleep con la RAM conservata: `C0=` risponde subito mentre `SAVE` impiega 313 ms, quindi `C0=` non scrive in flash. Per distinguere servirebbe staccare la batteria. **L'app manda comunque `SAVE`** dopo le modifiche da conservare |
| descrittori USB | 0483:5740, CDC su interfacce 0/1, full-speed | 0483:5740, CDC su interfacce 0/1, full-speed | vedi sotto |

Descrittori USB (USBTreeView):
- Prodotto `GENERIC_L433CCTX CDC in FS Mode` di `STMicroelectronics` (STM32L433CC), seriale `209737883931`, `bcdDevice` 0x0200.
- Classe del dispositivo 0x02/0x02 (CDC classico, senza IAD), una configurazione, self-powered, MaxPower 100 mA.
- Interfaccia 0: CDC ACM (02/02/00), endpoint interrupt IN `0x83` da 8 byte, `bInterval` 16 ms. Capacità ACM 0x02: `SET_LINE_CODING`, `SET_CONTROL_LINE_STATE`, `GET_LINE_CODING`, notifica `SERIAL_STATE`; niente `SEND_BREAK`. Union: controllo 0, dati 1.
- Interfaccia 1: CDC-Data (0A), bulk OUT `0x01` e bulk IN `0x82`, 64 byte ciascuno.
- Full-speed (12 Mbit/s), `bMaxPacketSize0` 64.

Conseguenze per il firmware: `BRIDGE_CDC_INTERFACE` = 0 (default) è giusto; i buffer del driver (IN alla dimensione del pacchetto, 64 byte; OUT 512 byte) e i blocchi da 128 byte verso la spada vanno bene; line coding e DTR/RTS sono richieste supportate.
