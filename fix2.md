# Fix2: notas tecnicas y razonamiento

## Contexto
El quiebre funcional principal aparece tras la integracion de bridge LDN + cambios de inyeccion en sysmodule (etapa posterior al diseno previo mas directo).

## Hipotesis validada en este ajuste

1. Para salas inalambricas, publicar `nodes[0].ipv4Address` con IP virtual (`10.13.x.x`) mejora la interoperabilidad sobre relay.
2. Tras sleep/wake, el estado de sockets/ruta puede quedar invalido aunque el proceso siga vivo; por eso se requiere reinicio controlado del servicio.

## Por que no se toco BSD global en este paso

- Cambiar globalmente `bsd_service_type` es de alto riesgo por dependencia de sockets raw del TAP.
- Se priorizo un fix incremental y reversible:
  - correccion de IP anunciada para LDN
  - recuperacion robusta de red tras energia

## Implementacion concreta

### A. LDN usa IP virtual cuando procede
- Fuente de verdad: `sdmc:/tmp/lanplay.status`
- Campos usados: `active`, `my_ip`
- Fallback seguro: IP real por NIFM si el archivo no es valido

### B. Sleep/wake con recuperacion determinista
- Hook de applet para evento de energia
- Comprobaciones en loop:
  - `nifmGetCurrentIpAddress`
  - cambio de IP
  - `relay_route_probe`
- Condicion de salida: reinicio limpio del servicio para recrear sockets/hilos

### C. Observabilidad
- `power_evt` y `power_restart` en status para diagnostico en campo

## Resultado esperado

- Menos casos donde la Switch queda inoperante tras hibernacion.
- Mejor anuncio de sala inalambrica en entornos relay.
- Menor necesidad de reinicios manuales completos.

## Proximo paso sugerido

Si aun hay juegos especificos con problemas, agregar trazas por tipo LDN (`Scan`, `ScanResp`, `Connect`, `SyncNetwork`) para aislar si falla discovery, senalizacion o transporte de datos.
