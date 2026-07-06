# Security and Build Setup

## Local Secrets

1. Copy `include/secrets.example.h` to `include/secrets.h`.
2. Fill WiFi, AP, web auth, printer IPs and OTA values.
3. Keep `include/secrets.h` out of Git. It is ignored in `.gitignore`.
4. Rotate any password that was previously stored in source code before sharing or committing this project.

Recommended split:

- `GUARDIAN_WEB_AUTH_PASSWORD`: panel and API access.
- `GUARDIAN_ARDUINO_OTA_PASSWORD`: PlatformIO/ArduinoOTA upload password.
- `GUARDIAN_AP_PASSWORD`: fallback AP password, long and not reused.

## Protected HTTP Surface

`/`, `/api/status`, `/api/config`, diagnostics, update actions (including `/api/update/upload`), STOP, reboot and reconnect are protected when `GUARDIAN_WEB_AUTH_USERNAME` and `GUARDIAN_WEB_AUTH_PASSWORD` are not empty.

## Firmware Binary Confidentiality

`firmware.bin` embeds every value from `include/secrets.h` (WiFi password, panel credentials, OTA password) as plain strings. Anyone holding the binary can extract them with `strings`. Therefore:

- Never publish the binary on a public URL or public repository.
- Prefer the panel upload flow (`Firmware -> Subir firmware (.bin)`), which keeps the binary inside the LAN.
- If a manifest OTA server is used, it must require the Bearer token (`GUARDIAN_OTA_AUTH_TOKEN`) or be private to the team.

For lab-only work, auth can be disabled by setting both web auth values to empty strings in `include/secrets.h`.

## Build Target

The production target is the local board:

```sh
~/.platformio/penv/bin/pio run -e guardian-n16r8
```

This uses:

- ESP32-S3 N16R8 local board definition.
- 16 MB flash.
- OPI PSRAM.
- `partitions_16mb_ota.csv`.
- Pinned PlatformIO platform and library versions.

## Serial Upload

Let PlatformIO auto-detect the port, or pass it explicitly:

```sh
~/.platformio/penv/bin/pio run -e guardian-n16r8 -t upload --upload-port /dev/cu.usbserial-0001
```

## Authenticated ArduinoOTA Upload

`ArduinoOTA.setPassword()` uses `GUARDIAN_ARDUINO_OTA_PASSWORD` at runtime. The upload command must provide the same password with `--auth`.

Build first:

```sh
~/.platformio/penv/bin/pio run -e guardian-n16r8
```

Upload the generated binary:

```sh
~/.platformio/penv/bin/python ~/.platformio/packages/framework-arduinoespressif32/tools/espota.py \
  -i impresoras3d.local \
  -p 3232 \
  -a '<GUARDIAN_ARDUINO_OTA_PASSWORD>' \
  -f .pio/build/guardian-n16r8/firmware.bin
```

Do not paste the real password into committed scripts.

## Manifest OTA

El panel web usa `GUARDIAN_OTA_METADATA_URL` para consultar un manifest HTTPS. El firmware exige `size` y `sha256` antes de instalar.

Ver `docs/ota-release.md` para el flujo de publicacion del binario, calculo de hash y actualizacion del manifest.
