# Fix aplicado (resumen ejecutable)

## Objetivo
Que funcione mejor el modo inalambrico con `ldn_mitm` sobre relay LAN Play, sin romper el modo LAN Play clasico.

## Cambios aplicados

### 1) `ldn_mitm` anuncia IP virtual cuando LAN Play esta activo
Archivo: `ldn_mitm-1.25.1/ldn_mitm/source/lan_discovery.cpp`

- Se agrego lectura de `sdmc:/tmp/lanplay.status` para obtener `my_ip`.
- Si `active=1` y `my_ip` pertenece a `10.13.0.0/16`, se usa esa IP en `nodes[0].ipv4Address`.
- Si no hay estado valido, se mantiene el comportamiento original (IP WiFi real por `nifmGetCurrentIpAddress`).

Resultado esperado:
- Mejor consistencia de `ScanResp` para peers remotos en relay.
- Menor dependencia de reescritura posterior en el bridge.

### 2) El sysmodule publica `my_ip` y telemetria de power
Archivo: `sysmodule/source/main.cpp`

- En `sdmc:/tmp/lanplay.status` ahora se escriben:
  - `my_ip`
  - `power_evt`
  - `power_restart`

Resultado esperado:
- `ldn_mitm` puede leer la IP virtual correcta.
- Se puede observar desde fuera cuantas recuperaciones por reposo/reanudacion ocurren.

### 3) Recuperacion explicita ante suspend/resume
Archivo: `sysmodule/source/main.cpp`

- Se agrego `appletHook` para marcar eventos de energia.
- En loop principal se valida:
  - IP actual de NIFM
  - cambio de IP respecto a la inicial
  - probe de ruta al relay (`sendto` UDP de prueba)
- Si falla, el servicio reinicia limpio (TAP/relay/bridge/sockets).

Resultado esperado:
- Menor probabilidad de bloqueo tras sleep/wake.
- Recuperacion automatica de red sin reboot manual completo.

## Compatibilidad buscada

- Modo LAN Play clasico: se conserva el pipeline actual.
- Modo inalambrico con `ldn_mitm`: mejora de anuncio de sala y recuperacion de red tras reposo.

## Riesgos conocidos

- La validacion fina requiere hardware real (Switch) para confirmar todos los juegos.
- El entorno local puede mostrar errores de include en `ldn_mitm` por falta de toolchain/submodulos, pero el cambio sigue el patron de API ya usado por el proyecto.

## Prueba rapida recomendada

1. Iniciar sysmodule + `ldn_mitm`.
2. Crear sala inalambrica en un juego.
3. Verificar desde `pc-peer` que aparece `ScanResp` y datos de sala.
4. Entrar en reposo con boton power.
5. Reanudar y comprobar que:
   - no queda bloqueado
   - se recupera ruta/estado
   - `power_evt` aumenta y `power_restart` refleja reinicios necesarios.
