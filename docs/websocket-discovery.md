# Descubrimiento WebSocket De Impresoras

El firmware incluye un modo de descubrimiento para observar el protocolo real de las impresoras antes de agregar comandos mas potentes.

## Que Registra

El panel guarda los ultimos mensajes WebSocket en un ring buffer:

- `rx`: texto recibido desde la impresora.
- `tx`: texto enviado por el ESP32.
- `ev`: eventos de conexion, desconexion o error.

El registro aparece en la pestaña `Impresoras`, bloque `Descubrimiento WS`, y tambien en:

- `/api/status` -> `ws_discovery`
- `/api/diagnostics` -> `ws_discovery`
- `/api/diagnostics/export`

## Telemetria Pasiva

Cuando un mensaje `rx` parece JSON, el firmware intenta extraer campos conocidos sin asumir un protocolo unico:

- estado: `state`, `print_state`, `printing_state`, `gcode_state`
- estado/texto: `status`, `message`, `msg`
- progreso: `progress`, `print_progress`, `percent`, `percentage`
- hotend: objetos como `extruder`, `tool0`, `nozzle`, `hotend`
- cama: objetos como `heater_bed`, `bed`, `build_plate`

La telemetria aparece dentro de cada impresora:

```json
{
  "telemetry": {
    "rx_count": 4,
    "tx_count": 2,
    "json_ok_count": 3,
    "json_error_count": 0,
    "state": "printing",
    "progress_pct": 42.5,
    "hotend_temp_c": 214.8,
    "bed_temp_c": 59.7
  }
}
```

Si el protocolo real usa otros nombres, el mensaje crudo queda igual en `ws_discovery` para ajustar el parser despues.

## Probes Disponibles

Los probes son comandos `get` de lectura. No mueven ejes, no cambian temperaturas y no sustituyen al STOP critico.

```json
{"method":"get","params":{"status":1}}
{"method":"get","params":{"state":1}}
{"method":"get","params":{"temperature":1}}
{"method":"get","params":{"info":1}}
```

Desde el panel puedes enviarlos por impresora:

- `P1 status`, `P1 state`, `P1 temp`, `P1 info`
- `P2 status`, `P2 state`, `P2 temp`, `P2 info`

## Endpoints

Enviar probe:

```http
POST /api/ws/probe
Content-Type: application/json
Authorization: Basic ...

{"printer":0,"probe":"status"}
```

Limpiar registro:

```http
POST /api/ws/discovery/clear
Authorization: Basic ...
```

## Como Usarlo

1. Conectar ambas impresoras y confirmar `WS activo`.
2. Limpiar el registro.
3. Enviar `status`, luego `state`, luego `temp`, luego `info`.
4. Esperar respuestas `rx` en el registro.
5. Descargar diagnostico y guardar el JSON.
6. Comparar si las dos impresoras responden igual.

## Siguiente Paso

Cuando sepamos que mensajes responde cada impresora, se pueden agregar comandos controlados como:

- pausa segura,
- reanudar,
- cancelar,
- lectura formal de progreso,
- lectura formal de temperaturas.

No conviene agregar G-code ni controles de temperatura hasta confirmar el protocolo real observado.
