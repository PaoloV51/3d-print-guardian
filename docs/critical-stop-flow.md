# Flujo Critico UPS A STOP

Este flujo cubre la ruta de seguridad principal:

```text
UPS FORZA FX-1500 -> ESP32-S3 -> WebSocket impresoras -> STOP
```

## Estados UPS

El firmware interpreta el byte posterior al marcador `#` del reporte HID:

- `2`: UPS en bateria, corte electrico confirmado.
- `>= 100`: UPS en linea, energia restaurada o normal.
- Otros codigos: se registran como no mapeados y no cambian el estado actual.

Si la UPS reporta bateria y luego deja de entregar lecturas, el supervisor mantiene el corte activo y dispara STOP cuando vence el temporizador. Esto evita depender de una segunda lectura para ejecutar la accion de seguridad.

## Supervisor STOP

El supervisor corre en cada `loop()` y hace tres cosas:

1. Si `on_battery` sigue activo y el corte supera `ups_stop_delay_s`, activa el latch STOP.
2. Si el latch STOP esta activo, intenta enviar `STOP_COMMAND` a cada impresora conectada.
3. Si una impresora no entrega ACK probable tras `STOP_MAX_ATTEMPTS`, la marca como `exhausted`.

El latch STOP persiste en NVS. Si el ESP32 reinicia durante una emergencia, restaura el latch y sigue intentando al reconectar WiFi/WebSocket.

El estado `on_battery` tambien persiste en NVS. Si el ESP32 reinicia en medio de un corte antes de que venza el temporizador, al arrancar rearma el temporizador de forma conservadora (contando desde el boot) sin esperar una lectura fresca del UPS. Si la luz ya volvio, la primera lectura del UPS (~12 s) limpia el estado sin efectos.

## Reintentos Y ACK

- Reintento minimo: `STOP_RETRY_INTERVAL_MS`.
- Intentos maximos por impresora: `STOP_MAX_ATTEMPTS`.
- ACK probable: cualquier mensaje WebSocket que parezca contener `stop`, `paused`, `pause`, `cancel` o `abort`.

El ACK es una pista operacional, no una confirmacion criptografica ni una respuesta formal del firmware de la impresora.

## Diagnostico Esperado

En `/api/status` y `/api/diagnostics` revisar:

- `stop_active`
- `stop_delivered_count`
- `stop_pending_count`
- `stop_exhausted_count`
- `ups.read_stale`
- `printers[].stop_attempts`
- `printers[].ack_state`
- `printers[].last_stop_attempt_*`

## Checklist De Prueba Fisica

1. Con UPS en linea, confirmar `ups.state = line` y `stop_active = false`.
2. Cortar energia de entrada de la UPS.
3. Confirmar que el panel muestra `on_battery = true` y el contador empieza.
4. Esperar menos que el temporizador y restaurar energia: no debe activar STOP.
5. Repetir corte y esperar que venza el temporizador.
6. Confirmar `stop_active = true`.
7. Confirmar `stop_delivered_count = 2` si ambas impresoras estaban conectadas.
8. Desconectar una impresora, activar STOP y reconectarla: debe recibir STOP al reconectar.
9. Reiniciar el ESP32 con latch STOP activo: debe restaurar el latch desde NVS.
9b. Reiniciar el ESP32 en medio de un corte (antes de vencer el temporizador): al arrancar debe mostrar `on_battery = true` con el temporizador rearmado, y limpiar el estado si la luz ya volvio.
10. Descargar `/api/diagnostics/export` y guardar el JSON junto con fecha, firmware y resultado.

## Criterio De Aceptacion

El punto 4 se considera aprobado cuando:

- No hay STOP si la energia vuelve antes del temporizador.
- Si el corte supera el temporizador, el latch queda activo.
- El STOP se transmite a ambas impresoras conectadas.
- Una impresora desconectada recibe STOP al reconectar.
- El estado queda visible en panel, logs y diagnostico.
