# switch-lan-play

[![Estado del build](https://github.com/shaklinedj/switch-lan-play/workflows/Build/badge.svg)](https://github.com/shaklinedj/switch-lan-play/actions?query=workflow%3ABuild)
[![Chat en Discord](https://img.shields.io/badge/chat-en%20discord-7289da.svg)](https://discord.gg/zEMCu5n)

Juega con tus amigos en modo multijugador local (LAN) a través de internet.

---

## Tabla de contenidos

1. [¿Cómo funciona?](#cómo-funciona)
2. [Uso rápido — Switch (sin PC)](#uso-rápido--switch-sin-pc)
3. [Servidor relay](#servidor-relay)
4. [Cliente PC (modo alternativo)](#cliente-pc-modo-alternativo)
5. [Compilación](#compilación)
   - [Cliente PC](#compilar-el-cliente-pc)
   - [Sysmodule Switch](#compilar-el-sysmodule-switch)
   - [App configuradora (homebrew)](#compilar-la-app-configuradora-homebrew)
6. [Configuración avanzada](#configuración-avanzada)
7. [Protocolo](#protocolo)
8. [Solución de problemas](#solución-de-problemas)

## Características Técnicas y Mejoras Recientes (Nativo)

Este proyecto ha sido reescrito y optimizado profundamente para integrarse al 100% con las entrañas de *Horizon OS* (El sistema operativo de Nintendo Switch) mediante Atmosphere, logrando lo siguiente:
- **Ejecución Silenciosa y Autónoma:** El Sysmodule corre enteramente en segundo plano bajo el Title ID `01000000000000B1`, liberando completamente la memoria de las aplicaciones.
- **Bypass de Traducción DNS (inet_pton):** Si introduces una dirección IP directa (Ej: `10.111.x.x`), el código saltará las rutinas del DNS oficial de Nintendo (`sfdnsres`) evitando el infame error *System Busy (EAI_AGAIN)*, pero manteniendo la compatibilidad con nombres de dominio usando resolución nativa.
- **Spoofing de Afinidad de Hilos:** En lugar de crashear por restricciones de CPU (Error de Kernel `0xe201`), el sysmodule extrae dinámicamente sus permisos de hilo permitidos (`svcGetThreadPriority`) y los clona en tiempo real, garantizando compatibilidad absoluta sea ejecutado como NRO o como Sysmodule de arranque.
- **Generación de IP Permanente:** Lee el Número de Serie de hardware único de tu Switch y autogenera matemáticamente una IP de túnel `10.13.X.X` inmutable que será tuya para siempre sin requerir DHCP.

---

## ¿Cómo funciona?

```
Switch A (juego en modo LAN)          Switch B (juego en modo LAN)
      |                                       |
  sysmodule                               sysmodule
      |          UDP por WiFi               |
      +---------> Servidor relay <----------+
                  (IP pública)
```

Cada Switch obtiene automáticamente una **IP virtual única** en el rango `10.13.0.0/16`
(calculada a partir del número de serie del dispositivo).  
El servidor relay reenvía los paquetes entre todos los jugadores conectados.  
El juego cree que todos están en la misma red local — sin necesidad de configurar IPs manualmente.

> **¿Por qué `10.13.x.x`?**  
> Es una dirección privada de uso interno, únicamente visible dentro del túnel virtual.
> Tu Switch sigue usando su IP real de WiFi (por ejemplo `192.168.1.50`) para llegar al servidor.
> La IP `10.13.x.x` es la que *los juegos* usan para reconocerse entre sí.

---

## Uso rápido — Switch (sin PC)

> Requiere Nintendo Switch con **Atmosphere CFW** y conexión WiFi.

### Paso 1 — Instalar en la tarjeta SD

Copia las carpetas `atmosphere/` y `switch/` del release a la **raíz de tu tarjeta SD**
y reinicia la Switch.

```
sdmc:/
├── atmosphere/
│   └── contents/
│       └── 42000000000000B1/
│           ├── exefs/
│           │   ├── main           ← sysmodule NSO
│           │   └── main.npdm      ← permisos
│           └── flags/
│               └── boot2.flag     ← arranque automático
└── switch/
    └── lanplay-setup/
        └── lanplay-setup.nro      ← app configuradora
```

### Paso 2 — Configurar el servidor

1. Abre el **Homebrew Menu** en tu Switch.
2. Lanza **"LanPlay Setup"**.
3. Pulsa **A**, escribe la dirección del servidor relay (p. ej. `relay.ejemplo.com:11451`) y pulsa **+** para confirmar.
4. Reinicia la Switch.

¡Listo! No hace falta ajustar IPs, no hace falta ningún PC.

> **Primera vez sin configuración:** el sysmodule crea el archivo de config automáticamente
> y muestra un aviso en pantalla. Abre "LanPlay Setup" para rellenarlo.

### Archivo de configuración (opcional / avanzado)

Ubicación: `sdmc:/config/lan-play/config.ini`

```ini
[server]
relay_addr = relay.ejemplo.com:11451

; Opcional: autenticación (si el servidor lo requiere)
; username = miusuario
; password = miclave

; Opcional: fijar una IP virtual específica (por defecto se genera automáticamente)
; ip = 10.13.5.10
```

---

## Jugar títulos sin modo LAN oficial (`ldn_mitm`, integrado)

Tu nueva versión instalada del Lan Play intercepta automáticamente juegos que traen un **"Modo LAN Oficial"** de fábrica (ej: Mario Kart 8 Deluxe usando L+R+L-Stick).
¿Pero qué pasa si quieres jugar a títulos como *Super Mario 3D World* o *Pokémon* que sólo tienen "Inalámbrico Local"?

**`ldn_mitm` está incluido en este repositorio** (como submodule en `ldn_mitm/`) y su binario precompilado ya se encuentra en `sd/`. Los dos sysmodules trabajan juntos automáticamente:

1. Copia el contenido de la carpeta `sd/` a la raíz de tu MicroSD.
2. Reinicia tu Switch.
3. Ahora, cuando entres al "Modo Local" de CUALQUIER juego inalámbrico, `ldn_mitm` (Title ID `4200000000000010`) convertirá las señales locales a señales LAN, y el sysmodule `switch-lan-play` (Title ID `42000000000000B1`) las tuneará al servidor relay a través de internet.

```
sdmc:/
├── atmosphere/
│   └── contents/
│       ├── 42000000000000B1/         ← switch-lan-play (relay a internet)
│       │   ├── exefs/main
│       │   ├── exefs/main.npdm
│       │   └── flags/boot2.flag
│       └── 4200000000000010/         ← ldn_mitm (intercepta ldn:u)
│           ├── exefs.nsp
│           ├── toolbox.json
│           └── flags/boot2.flag
└── switch/
    ├── lanplay-setup/
    │   └── lanplay-setup.nro         ← configurador de relay
    ├── ldnmitm_config/
    │   └── ldnmitm_config.nro        ← configurador de ldn_mitm
    └── .overlays/
        └── ldnmitm_config.ovl        ← overlay Tesla para ldn_mitm
```

### Flujo de paquetes con ambos sysmodules activos

```
Juego (modo Inalámbrico Local)
    │
    ▼ llamada a ldn:u
[ldn_mitm] ─intercept─► convierte LDN a paquetes UDP en 10.13.x.x:11452
    │
    ▼ paquetes IP en subred 10.13.0.0/16
[switch-lan-play sysmodule] ─raw socket─► relay server (internet)
    │
    ▼ relay server
Otras consolas de los jugadores (misma cadena en sentido inverso)
```

---

## Servidor relay

El servidor escucha en el puerto `11451/UDP` y reenvía paquetes entre las consolas conectadas.
Requiere una IP pública accesible por tus jugadores.

### Opción A — Docker (recomendado)

```sh
docker run -d \
  -p 11451:11451/udp \
  -p 11451:11451/tcp \
  shaklinedj/switch-lan-play
```

Con `docker compose` (desde la carpeta `server/`):

```sh
cd server
docker compose up -d
```

### Opción B — Node.js directamente

```sh
git clone https://github.com/shaklinedj/switch-lan-play
cd switch-lan-play/server
npm install
npm run build
npm start
```

Opciones disponibles:

| Parámetro | Descripción |
|-----------|-------------|
| `--port 11451` | Puerto UDP (por defecto `11451`) |
| `--simpleAuth usuario:contraseña` | Autenticación básica |
| `--jsonAuth ./users.json` | Autenticación por archivo JSON |
| `--httpAuth https://...` | Autenticación por HTTP externo |

Ejemplo con autenticación y puerto personalizado:

```sh
npm start -- --port 11451 --simpleAuth admin:secreto
```

### Monitor de estado

El servidor también expone un endpoint HTTP en el mismo puerto TCP:

```
GET http://TU_IP_SERVIDOR:11451/info
→ { "online": 5 }
```

### Puertos que deben estar abiertos

| Puerto | Protocolo | Uso |
|--------|-----------|-----|
| 11451 | **UDP** | Relay de partidas (**obligatorio**) |
| 11451 | TCP | API de estado (opcional) |

### Opciones de hosting gratuito

- **Oracle Cloud Free Tier** — 2 VMs ARM de 1 GB RAM para siempre (mejor opción).
- **fly.io** — tier gratuito para hobbistas.
- **Railway.app / Render.com** — desplegables con el Dockerfile incluido.

---

## Cliente PC (modo alternativo)

Si prefieres usar el cliente de PC en lugar del sysmodule en la Switch:

- Tu PC y tu Switch deben estar **en la misma red WiFi/LAN**.
- Ejecuta el cliente en el PC conectándote al servidor relay.
- En la Switch activa el modo LAN y asigna una IP estática en el rango `10.13.x.x`.

```
                       Internet
                          |
                  [Proxy SOCKS5] (opcional)
                          |
        ARP/IPv4          |          Paquetes LAN
Switch <---------> PC (lan-play) <-------------> Servidor
                                        UDP
```

### Usar proxy SOCKS5

```sh
lan-play --socks5-server-addr ejemplo.com:1080
```

> El tráfico hacia el servidor relay **no pasa** por el proxy.

---

## Compilación

### Compilar el cliente PC

#### Ubuntu / Debian

```sh
sudo apt install libpcap0.8-dev git gcc g++ cmake
git clone https://github.com/shaklinedj/switch-lan-play
cd switch-lan-play
mkdir build && cd build
cmake ..
make
```

Para modo Debug:

```sh
cmake -DCMAKE_BUILD_TYPE=Debug ..
```

Para modo Release:

```sh
cmake -DCMAKE_BUILD_TYPE=Release ..
```

#### Windows (MSYS2)

Instala [MSYS2](http://www.msys2.org/) y luego:

```sh
# 64 bits
pacman -Sy
pacman -S make mingw-w64-x86_64-cmake mingw-w64-x86_64-gcc

# Abre "MSYS2 MinGW 64-bit" y compila:
mkdir build && cd build
cmake -G "MSYS Makefiles" ..
make
```

Para 32 bits usa `mingw-w64-i686-cmake` y `mingw-w64-i686-gcc` en "MSYS2 MinGW 32-bit".

#### macOS

```sh
brew install cmake
git clone https://github.com/shaklinedj/switch-lan-play
cd switch-lan-play
mkdir build && cd build
cmake ..
make
```

---

### Compilación unificada (recomendado)

Compila y empaqueta **ambos sysmodules + la app** de una sola vez:

```sh
# 1. Inicializar todos los submodules (ldn_mitm incluye Atmosphere-libs)
git submodule update --init --recursive

# 2. Instalar devkitPro con soporte Switch
dkp-pacman -S switch-dev switch-atmo-tools

# 3. Compilar todo
make all

# 4. Ensamblar el paquete SD card completo en sd/
make package
# Resultado: carpeta sd/ → copia su contenido a la raíz de la SD
```

---

### Compilar el sysmodule Switch (por separado)

Requiere **devkitPro** con soporte para Switch:

```sh
# Instalar herramientas
dkp-pacman -S switch-dev switch-atmo-tools

# Compilar
cd sysmodule
make atmosphere
# Resultado: carpeta atmosphere/ → copia a la raíz de la SD
```

---

### Compilar ldn_mitm (por separado)

```sh
# Inicializar el submodule con sus propias dependencias
git submodule update --init --recursive ldn_mitm

# Compilar
make ldn_mitm
# O directamente:
cd ldn_mitm && make
```

---

### Compilar la app configuradora (homebrew)

```sh
cd hbapp
make
# Resultado: lanplay-setup.nro
# Instalar en: sdmc:/switch/lanplay-setup/lanplay-setup.nro
```

---

## Configuración avanzada

### Autenticación en el servidor

Edita `server/users.json` (ver `users_schema.json` para el formato) e inicia con:

```sh
npm start -- --jsonAuth ./users.json
```

Los jugadores añaden en su `config.ini`:

```ini
username = miusuario
password = miclave
```

### Fijar IP virtual en la Switch

Por defecto la IP se genera automáticamente desde el número de serie.
Para fijar una IP concreta edita `sdmc:/config/lan-play/config.ini`:

```ini
[server]
relay_addr = relay.ejemplo.com:11451
ip = 10.13.5.10
```

---

## Protocolo

El protocolo es simple: cada paquete lleva un byte de tipo seguido del payload.

```c
struct packet {
    uint8_t type;
    uint8_t payload[packet_len - 1];
};
```

```c
enum type {
    KEEPALIVE  = 0,
    IPV4       = 1,
    PING       = 2,
    IPV4_FRAG  = 3
};
```

El servidor extrae la IP de destino del payload y la compara con su tabla de caché
(IP origen → IP LAN virtual).  Si no hay coincidencia, el paquete se difunde
a todos los clientes de la sala.

---

## Solución de problemas

| Síntoma | Qué revisar |
|---------|-------------|
| El sysmodule no arranca | Verifica la versión de Atmosphere; comprueba que `boot2.flag` existe |
| "LanPlay Setup" no aparece en hbmenu | Verifica que el NRO está en `sdmc:/switch/lanplay-setup/` |
| Mensaje de "sin servidor configurado" al arrancar | Abre "LanPlay Setup" y configura `relay_addr` |
| No resuelve el servidor relay | Comprueba que el WiFi funciona; revisa `relay_addr` en `config.ini` |
| Latencia alta | Usa un servidor relay geográficamente cercano a todos los jugadores |
| El cliente PC no detecta la Switch | Verifica que PC y Switch están en la misma red local |

---

## Licencia

[MIT](LICENSE.txt)
