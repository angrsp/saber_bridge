# saber_bridge

Firmware del bridge BLE ↔ USB CDC/ACM per spade OpenCore (Anima NXT), su Seeed XIAO ESP32-S3.

Piano di sviluppo e contratto BLE: `piano-FW-bridge.md` (cartella del progetto).

## Compilare

Toolchain: ESP-IDF 5.5.6. Senza installarla, con Docker:

```
tools/idf-docker.sh build
```

Con ESP-IDF installato (installer Windows): `idf.py build`.

## Flash

Il container non vede le porte COM: si flasha dall'host con esptool (`pip install esptool`),
con l'adattatore OTG staccato e il XIAO collegato al PC (se non compare, tenere premuto BOOT mentre si collega):

```
cd build
python -m esptool --chip esp32s3 -p COMx -b 460800 write_flash "@flash_args"
```
