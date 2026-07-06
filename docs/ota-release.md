# OTA Release

Este firmware acepta OTA por manifest HTTPS y exige integridad antes de instalar:

- `bin_url` debe ser HTTPS.
- `size` debe coincidir con el tamaño real del `.bin`.
- `sha256` debe ser el SHA-256 hexadecimal del `.bin` completo.
- `project`, si existe, debe ser `3D Print Guardian`.

Deteccion de update: se ofrece si la `version` remota es mayor que la local, o si es la misma `version` con un `build` mas nuevo (rebuild/hotfix sin bump de version, ej. `20260706.2` -> `20260706.4`). Un manifest con version o build menor nunca se ofrece (anti-downgrade).

## 1. Build De Produccion

```sh
~/.platformio/penv/bin/pio run -e guardian-n16r8
```

El binario queda en:

```text
.pio/build/guardian-n16r8/firmware.bin
```

## 2. Obtener Size Y SHA-256

En macOS:

```sh
stat -f%z .pio/build/guardian-n16r8/firmware.bin
shasum -a 256 .pio/build/guardian-n16r8/firmware.bin | awk '{print $1}'
```

Cada rebuild cambia `size` y `sha256`; recalcularlos siempre antes de publicar el manifest.

## 3. Publicar Archivos

Subir el binario a una URL HTTPS estable:

```text
https://tu-servidor.com/ota/guardian/firmware-1.3.0.bin
```

Crear o actualizar el manifest:

```json
{
  "manifest_version": 2,
  "project": "3D Print Guardian",
  "version": "1.3.0",
  "build": "20260706.4",
  "bin_url": "https://tu-servidor.com/ota/guardian/firmware-1.3.0.bin",
  "size": 1180000,
  "sha256": "7cacb004f3eced6d519ed527120610a309d4e7492098c533038d3df86ed98d61",
  "changelog": "Upload directo de firmware por panel, rearme STOP tras reboot en corte, cache de manifest y deteccion de rebuilds"
}
```

Los valores del ejemplo corresponden a la build local `1.3.0` / `20260706.4` generada durante esta preparacion. Reemplaza `size` y `sha256` cada vez que vuelvas a compilar.

## 4. Configurar El Dispositivo

En `include/secrets.h`:

```cpp
#define GUARDIAN_OTA_METADATA_URL "https://tu-servidor.com/ota/guardian/manifest.json"
#define GUARDIAN_OTA_AUTH_TOKEN ""
#define GUARDIAN_OTA_CA_CERT_PEM ""
#define GUARDIAN_OTA_ALLOW_INSECURE_HTTPS false
```

Para produccion, usa certificado TLS valido o pega la CA raiz/intermedia en `GUARDIAN_OTA_CA_CERT_PEM`.

## 5. Flujo En El Panel

1. Abrir el panel web.
2. Entrar a `Actualizacion`.
3. Presionar `Buscar actualizacion`.
4. Verificar que aparezcan version, tamano esperado y SHA del manifest.
5. Presionar `Instalar`.

La instalacion se cancela si el tamano o SHA-256 descargado no coinciden con el manifest.

## Notas

- Si cambias particiones, haz primero un upload serial completo.
- Si usas token, el firmware envia `Authorization: Bearer <token>`.
- El hash SHA-256 protege integridad contra archivos corruptos o equivocados; para cadena de suministro estricta, el siguiente paso es firma criptografica del manifest o del firmware.
