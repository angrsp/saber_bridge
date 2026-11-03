# Riepilogo degli sviluppi: firmware saber_bridge

Aggiornato al 2026-11-03. Dettagli e contratto BLE: [`piano-FW-bridge.md`](piano-FW-bridge.md).

Il bridge collega la Anima NXT (USB CDC/ACM) al telefono via BLE, su una Seeed XIAO ESP32-S3. Inoltra i byte senza interpretarli: il protocollo della spada vive tutto nell'app.

## In breve

| Fase | Stato | Note |
|---|---|---|
| F0 – Verifiche su PC con la NXT | ✅ conclusa | protocollo reale registrato, coincide con l'emulatore |
| F1 – Toolchain e build | ✅ conclusa | ESP-IDF v5.5.6 in Docker, 0 warning |
| F2 – Robustezza | ✅ conclusa | |
| F3 – Servizio di stato e DIS | ✅ conclusa | UUID fissati |
| F4 – Variante fake saber | ✅ conclusa | |
| F5 – Bring-up hardware | ✅ conclusa | variante fake sulla XIAO: 500 comandi senza perdite, latenza media 68 ms |
| F6–F10 | ⏳ in attesa | servono LiPo e adattatore OTG |

La variante fake è provata sulla XIAO (F5) e il protocollo sulla NXT collegata al PC (F0). **La variante reale, con la NXT collegata alla XIAO, non è ancora stata provata** (F6).

## Cosa è stato fatto

### F1 – Toolchain e build
- ESP-IDF v5.5.6 dall'immagine Docker `espressif/idf:v5.5.6`, senza installarlo: `tools/idf-docker.sh` (qualsiasi comando `idf.py`) e `tools/build-all.sh` (variante reale in `build/`, fake in `build-fake/`).
- Il flash si fa dall'host con esptool, perché il container non vede le porte COM (README).
- Gitflow con `main` e `develop`; un ramo `feature/` per fase, chiuso con merge `--no-ff`.
- Firme allineate al componente `usb_host_cdc_acm` 2.4.1 e a NimBLE di IDF 5.5.

### F2 – Robustezza
- Buffer tra USB e BLE (`pipe.c`) svuotabili a ogni cambio di stato senza rompere la regola "un lettore e uno scrittore": ogni lettore scarta da sé i dati vecchi.
- La scrittura su RX restituisce un errore invece di perdere byte: `0x80` se la spada non è collegata, `0x81` se il buffer è pieno. I byte persi verso il telefono vengono contati.
- Apre solo le spade note (NXT 0483:5740, EVO 16C0:0483) e le riapre da sola con backoff da 500 ms a 2 s.
- Corretti due casi di scollegamento: una spada ricollegata con lo stesso VID/PID poteva essere ignorata, e un evento di scollegamento arrivato troppo presto poteva bloccare il task.
- Menu Kconfig "Saber Bridge": nome BLE, DTR/RTS, VID/PID qualsiasi, LED, batteria, livello di log.
- LED su GPIO21: lampeggio lento senza client, veloce con client ma senza spada, fisso con tutto pronto.

### F3 – Servizio di stato e DIS
- Caratteristica di stato `7A6F0002-…` (read + notify), payload di 8 byte: versione, flag (spada, byte persi, batteria bassa), VID, PID, mV.
- Notifica a ogni cambio, quando il client si iscrive e, se la misura della batteria è attiva, ogni 30 s.
- Device Information Service: Firmware Revision = `PROJECT_VER`, Model Number = `SaberBridge-S3`.
- Misura della batteria opzionale (ADC1 con calibrazione, media su 16 campioni, isteresi di 100 mV).

### F4 – Variante fake saber
- `sdkconfig.fake`: al posto dell'host USB c'è un emulatore della NXT in C puro (`nxt_emu.c`) che risponde ai comandi dei colori.
- La console resta sulla USB della XIAO; `unplug` e `plug` dal monitor seriale simulano lo scollegamento della spada.
- Serve a provare l'app via BLE con la sola scheda.

### F0 – Verifiche con la NXT
- `tools/f0_probe.py`: manda i comandi, registra i byte grezzi e il tempo di risposta, confronta con l'emulatore, prova la persistenza dei colori (con backup e ripristino automatici) e scrive un report.
- Due giri sulla NXT reale col cavo col rosso tagliato, più i descrittori USB con USBTreeView. Tutto è nel §6 del piano.

### F5 – Bring-up della XIAO
- `tools/bridge_test.py` (bleak): DIS e stato, comandi di sola lettura, una risposta lunga che arriva su più notifiche, 500 comandi di fila con la latenza, controllo dei byte persi, report. Con `--serial` fa le stesse prove senza BLE (sulla spada collegata al PC, o sull'emulatore nelle prove sul PC).
- Variante fake sulla XIAO, con la sua antenna, provata dal PC Windows: 500 comandi senza errori né byte persi, latenza mediana 60 ms, media 68 ms, massima 151 ms. Risposta lunga di 376 byte ricomposta da 3 notifiche. MTU 247, DIS e stato corretti, `0x80` con la spada scollegata. Da Android (nRF Connect) si connette al primo tentativo.
- **Corretto un blocco della connessione con Windows.** Il Bluetooth Intel risponde con un errore alla lettura delle funzioni del client, e il NimBLE di ESP-IDF scambiava la connessione per fallita: scartava le prime richieste dei servizi (scoperta appesa per 30 s) e lasciava la connessione aperta senza advertising. Correzioni in `sdkconfig.defaults` e in `ble_uart.c`; l'advertising ora riparte da solo se non parte.
- Log di NimBLE a livello warning: a livello info scriveva una riga per ogni notifica, troppo lento sulla UART della variante reale.

### Prove sul PC (`tools/test-host.sh`)
- Emulatore: 48 controlli; buffer: 14 controlli.
- `f0_probe.py` e `bridge_test.py` eseguiti per intero contro l'emulatore su una pseudo-seriale.

## Cosa ha mostrato la NXT (F0)

Da sapere per l'app:
- Tutte le risposte coincidono con l'emulatore della variante fake.
- I comandi vanno terminati col solo LF: con `\r\n` la spada non risponde.
- `c?`, `f?` e `w?` rispondono con la lettera minuscola (`c0=…`). `C0=` risponde con l'eco del comando, `P=` con `OK, ` (con la virgola).
- `w?` funziona sulla NXT 1.1.0: l'app non deve decidere se usarlo in base al numero di versione (il limite 1.9.15 vale solo per la EVO).
- `P=` accende la lama anche senza VBUS.
- `SAVE` risponde dopo circa 313 ms, gli altri comandi entro 16 ms.
- Senza VBUS, `SAVE` rende i colori persistenti. Anche i colori impostati senza `SAVE` sono rimasti dopo lo spegnimento, ma non si sa se la spada salvi da sola o se in "spento" resti in sleep. Per questo l'app manda comunque `SAVE`.
- USB: STM32L433, CDC classico con controllo sull'interfaccia 0 e dati sull'interfaccia 1, endpoint bulk da 64 byte, full-speed. Coincide con la configurazione del firmware.

## Stato del repository

- Remoto: `github.com/angrsp/saber_bridge`.
- `main` contiene solo il commit iniziale: nessuna versione è ancora rilasciata.
- `develop` contiene F0–F5, il piano e questo riepilogo.
- Nessun ramo `feature/` aperto: quelli già uniti sono stati cancellati, in locale e su `origin`.

## Prossimi passi

### Subito, senza hardware
1. **Ordinare l'hardware che manca:** LiPo con circuito di protezione (PCM), adattatore OTG corto e di buona qualità, adattatore USB-UART a 3,3 V.
2. **App:** la fase A3 non è più bloccata da F0 e può partire usando il §6 del piano. Anche la prima parte di A6 (app via BLE con la variante fake) si può fare già ora con la XIAO. In `piano-app-BLE.md` va aggiornato il rimando al piano, che ora si trova in `saber_bridge/piano-FW-bridge.md`.

### Con OTG e LiPo: F6 e F7
1. **F6:** prima di collegare la LiPo, controllare la polarità dei pad batteria sul silkscreen. Poi variante reale con la NXT (cavo col rosso tagliato), con le stesse risposte del §6; colore + `SAVE` e verifica dopo lo spegnimento; prove con DTR/RTS asseriti, solo DTR, nessuno. I log dall'adattatore USB-UART su D6/D7.
2. **F7:** spada spenta e riaccesa col telefono connesso, telefono disconnesso a metà risposta, adattatore staccato e ricollegato, un'ora di comandi, 4 KB scritti di fila. Obiettivi: spada riaperta entro 2 s, notifica di stato entro 1 s.
3. Rilascio **`v0.1.0`**: primo bridge utilizzabile con la NXT e con l'app.

### Dopo
- **F8:** consumi e power management (advertising lento, deep sleep senza client e sotto 3,3 V, risveglio da pulsante) → `v0.2.0`.
- **F9:** pairing con LE Secure Connections e bonding, da coordinare con la fase A7 dell'app.
- **F10:** documentazione completa (cablaggio, flash, recupero, LED, contratto BLE) → `v1.0.0`.
