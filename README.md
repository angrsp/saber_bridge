# saber_bridge

Firmware del bridge BLE ↔ USB CDC/ACM per spade OpenCore (Anima NXT), su Seeed XIAO ESP32-S3.

Piano di sviluppo e contratto BLE: `piano-FW-bridge.md` (cartella del progetto).

## Compilare

Toolchain: ESP-IDF 5.5.6. Senza installarla, con Docker:

```
tools/build-all.sh        # variante reale in build/, variante fake in build-fake/
tools/idf-docker.sh build # solo la variante reale
tools/test-host.sh        # prove sul PC: emulatore della NXT e pipe
```

Con ESP-IDF installato (installer Windows): `idf.py build`; per la variante fake
`idf.py -B build-fake -D SDKCONFIG=build-fake/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.fake" build`.

Le opzioni stanno nel menu "Saber Bridge" (`tools/idf-docker.sh menuconfig`).

## Varianti

- **Reale**: host USB CDC/ACM verso la spada (solo 0483:5740 NXT e 16C0:0483 EVO, salvo opzione).
  La console è su UART0 (D6 = TX, D7 = RX): per i log serve un adattatore USB-UART a 3,3 V.
- **Fake** (`sdkconfig.fake`): niente host USB, un emulatore della NXT risponde ai comandi dei colori.
  La console resta sulla USB della XIAO; dal monitor seriale `unplug` e `plug` simulano lo scollegamento
  della spada. Serve per provare l'app via BLE con la sola scheda.

## LED (GPIO21)

| LED | Stato |
|---|---|
| lampeggio lento | advertising, nessun client |
| lampeggio veloce | client connesso, spada assente |
| fisso | client connesso e spada collegata |

## Flash

Il container non vede le porte COM: si flasha dall'host con esptool (`pip install esptool`),
con l'adattatore OTG staccato e il XIAO collegato al PC (se non compare, tenere premuto BOOT mentre si collega):

```
cd build
python -m esptool --chip esp32s3 -p COMx -b 460800 write_flash "@flash_args"
```

Per la variante fake, lo stesso da `build-fake/`.

## F0: risposte reali della spada

Con la spada accesa e collegata al PC col cavo col rosso tagliato:

```
pip install pyserial
python tools/f0_probe.py
```

Lo script manda i comandi della fase F0, confronta le risposte con l'emulatore, prova la persistenza
dei colori con uno spegnimento (si salta con `--skip-persistence`) e scrive un report `f0-report-*.md`.
Prima di modificare qualcosa salva i colori principali in `f0-backup-*.json` e alla fine li rimette;
se si interrompe a metà: `python tools/f0_probe.py --restore f0-backup-<data>.json`.
