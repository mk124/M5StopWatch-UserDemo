# M5StopWatch-UserDemo
M5Stack StopWatch user demo for hardware evaluation.

## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Flash

```bash
idf.py flash
```

## BLE OTA

The first installation must be flashed over USB to install the bootloader, OTA
partition table, and BLE OTA App. Subsequent application updates can be sent
wirelessly by opening **BLE OTA** on the watch and running:

```bash
idf.py build
./tools/m5ota build/StopWatch-UserDemo.bin
```

On Windows PowerShell:

```powershell
idf.py build
.\tools\m5ota.ps1 build\StopWatch-UserDemo.bin
```

No pairing or Wi-Fi is required. Before connecting, press A+B to switch from the
default same-project Update mode to cross-project Install mode. VS Code provides
the equivalent **BLE Flash** task. Only the application `.bin` is supported;
bootloader, partition-table, merged, and full-flash images still require USB.

See the [`m5ota` documentation](https://github.com/mk124/m5ota#readme) for
platform requirements and optional CLI arguments.
