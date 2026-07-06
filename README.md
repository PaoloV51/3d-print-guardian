# 3D Print Guardian

Firmware PlatformIO para un ESP32-S3 N16R8 que vigila una UPS FORZA FX-1500, mantiene un panel web local y envia STOP por WebSocket a dos impresoras 3D cuando un corte electrico supera el temporizador configurado.

## Estado Del Proyecto

Base profesional aplicada:

- Secretos fuera del firmware versionable.
- API y panel protegidos por Basic Auth configurable.
- ArduinoOTA con password runtime.
- OTA por manifest HTTPS con validacion de tamaño y SHA-256.
- Build reproducible para ESP32-S3 N16R8.
- Tabla de particiones OTA de 16 MB incluida en el repo.
- Dependencias PlatformIO fijadas.
- Panel de soporte con diagnostico exportable.
- Descubrimiento WebSocket RX/TX y telemetria pasiva para estudiar el protocolo real de las impresoras.

Pendiente recomendado:

- Modularizar `src/main.cpp`.
- Agregar firma criptografica de manifest/firmware si se requiere cadena de suministro estricta.
- Publicar servidor OTA real y certificado TLS definitivo.

## Hardware Objetivo

- ESP32-S3 N16R8.
- 16 MB flash.
- 8 MB OPI PSRAM.
- UPS FORZA FX-1500 conectada por USB host.
- Dos impresoras compatibles con STOP por WebSocket.

## Estructura

- `src/main.cpp`: firmware principal.
- `include/secrets.example.h`: plantilla de secretos versionable.
- `include/secrets.h`: secretos locales, ignorado por Git.
- `boards/esp32-s3-devkitc-1-n16r8.json`: placa local N16R8.
- `partitions_16mb_ota.csv`: particiones OTA para 16 MB.
- `lib/EspUsbHostFork`: fork local de USB host usado por la UPS.
- `docs/security-build.md`: flujo de secretos, build y OTA autenticado.
- `docs/ota-release.md`: flujo de release OTA con tamaño y SHA-256.
- `docs/critical-stop-flow.md`: supervisor UPS -> STOP, reintentos y checklist fisico.
- `docs/websocket-discovery.md`: probes seguros y registro RX/TX de impresoras.
- `ota_manifest_example.json`: ejemplo de manifest OTA.

## Configuracion Inicial

Crear el archivo local de secretos:

```sh
cp include/secrets.example.h include/secrets.h
```

Editar `include/secrets.h` con:

- WiFi principal.
- Password del AP de respaldo.
- Usuario/password del panel.
- Password ArduinoOTA.
- IPs de impresoras.
- URL/token/certificado OTA si aplica.

No subir `include/secrets.h` a Git.

## Build

```sh
~/.platformio/penv/bin/pio run -e guardian-n16r8
```

El build debe reportar:

- Board: `esp32-s3-devkitc-1-n16r8`.
- Hardware: `16MB Flash`.
- Particiones: `partitions_16mb_ota.csv`.

## Upload Serial

```sh
~/.platformio/penv/bin/pio run -e guardian-n16r8 -t upload --upload-port /dev/cu.usbserial-0001
```

## OTA Arduino

Ver [docs/security-build.md](docs/security-build.md) para el comando OTA autenticado con `--auth`.

## OTA Por Manifest

Ver [docs/ota-release.md](docs/ota-release.md) para generar `firmware.bin`, calcular `size`/`sha256` y publicar el manifest HTTPS que usa el panel.

## Panel Web

- STA/mDNS: `http://impresoras3d.local/`
- AP de respaldo: `http://192.168.4.1/`

El panel, `/api/status`, `/api/config`, diagnostico, acciones de STOP, reboot, reconnect y OTA quedan protegidos cuando `GUARDIAN_WEB_AUTH_USERNAME` y `GUARDIAN_WEB_AUTH_PASSWORD` tienen valor.

La pestaña `Soporte` muestra salud operativa, particiones OTA, memoria, PSRAM, flash, version instalada y acciones de mantenimiento. Desde ahi se puede abrir o descargar el diagnostico protegido.

El flujo critico UPS -> STOP esta documentado en [docs/critical-stop-flow.md](docs/critical-stop-flow.md). El firmware mantiene un supervisor independiente para activar STOP aunque la UPS deje de entregar lecturas despues de reportar bateria.

El descubrimiento WebSocket esta documentado en [docs/websocket-discovery.md](docs/websocket-discovery.md). Permite enviar probes `get`, guardar mensajes RX/TX e interpretar telemetria pasiva antes de agregar controles mas avanzados.

Endpoints de soporte:

- `/api/diagnostics`: diagnostico JSON protegido.
- `/api/diagnostics/export`: mismo diagnostico como descarga.
- `/api/ws/probe`: probe WebSocket protegido para una impresora.
- `/api/ws/discovery/clear`: limpia el registro RX/TX.
- `/api/update/upload`: sube un `firmware.bin` directamente desde el panel (protegido).

## Actualizacion Por Panel (Sin Servidor)

La pestaña `Firmware` incluye `Subir firmware (.bin)`: sube el binario compilado directamente al ESP32 desde el navegador, sin servidor OTA externo. Es la via recomendada para soporte en sitio o LAN:

1. Compilar: `~/.platformio/penv/bin/pio run -e guardian-n16r8`
2. Tomar `.pio/build/guardian-n16r8/firmware.bin`
3. Panel -> Firmware -> `Subir firmware (.bin)` -> confirmar

El firmware valida la imagen antes de aplicarla y reinicia al terminar. El binario embebe los secretos compilados, por eso este flujo local (que nunca publica el `.bin` en internet) es preferible a hospedarlo en un servidor publico. El OTA por manifest HTTPS sigue disponible para flotas con servidor propio autenticado.

## Primer Commit Limpio

Antes del primer commit:

```sh
git status --short --ignored
git check-ignore -v include/secrets.h .pio/build/guardian-n16r8/firmware.bin .DS_Store
~/.platformio/penv/bin/pio run -e guardian-n16r8 -e ota
```

Debe quedar ignorado:

- `.pio/`
- `.DS_Store`
- `.claude/`
- `include/secrets.h`
- artefactos `*.bin`, `*.elf`, `*.map`

Archivos que si deben versionarse:

- `README.md`
- `.gitignore`
- `platformio.ini`
- `boards/`
- `docs/`
- `include/secrets.example.h`
- `lib/EspUsbHostFork/`
- `partitions_16mb_ota.csv`
- `src/`
- `test/`

## Notas De Operacion

- Si se cambian credenciales ya compartidas, rotarlas tambien en el router/AP y en cualquier estacion de soporte.
- Si se cambia la tabla de particiones, hacer upload serial completo antes de confiar en OTA.
- Si se cambia el password de ArduinoOTA, actualizar tambien el comando de carga OTA.
