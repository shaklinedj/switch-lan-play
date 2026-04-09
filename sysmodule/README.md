# switch-lan-play — Switch Sysmodule

Run LAN play **directly on your Nintendo Switch** — no PC, no manual IP config.

The sysmodule connects your Switch to a relay server over WiFi and bridges LAN-play
game packets through it, letting you play with friends across the internet as if
you were all on the same local network.

---

## How it works

```
Switch (game in LAN mode)
    ↓
sysmodule raw socket
    ↓ UDP over WiFi
Relay server (switch-lan-play server)
    ↑ UDP over WiFi
Other players' Switch consoles
```

The sysmodule replaces the PC-side `lan-play` client entirely.
Each Switch **automatically gets a unique IP** in the `10.13.0.0/16` virtual
LAN derived from its device serial number — players never touch network settings.

---

## Requirements

| Item | Notes |
|------|-------|
| Nintendo Switch with **Atmosphere CFW** | Any version that supports Atmosphere 0.14+ |
| SD card | Any size |
| WiFi connection | Used to reach the relay server |
| A relay server | See [server/README.md](../server/README.md) for free hosting |

---

## Player setup — 2 steps

### Step 1 — Install

Copy the `atmosphere/` and `switch/` folders from the release to the **root
of your SD card** and reboot the Switch.

```
sdmc:/
├── atmosphere/
│   └── contents/
│       └── 010000000000FF01/
│           ├── exefs/
│           │   ├── main          ← sysmodule NSO
│           │   └── main.npdm     ← permissions
│           └── flags/
│               └── boot2.flag    ← auto-start at boot
└── switch/
    └── lanplay-setup/
        └── lanplay-setup.nro     ← Homebrew configurator
```

### Step 2 — Configure the server

1. Open **Homebrew Menu** (hbmenu) on the Switch.
2. Launch **"LanPlay Setup"**.
3. Press **A**, type the relay server address (e.g. `relay.example.com:11451`),
   press **+** to confirm.
4. Reboot.

That's it.  No manual IP settings, no PC-side text editor, no static IP.

> **First time with no config?** The sysmodule writes a template config file
> and shows an on-screen message.  Open "LanPlay Setup" in hbmenu to fill it in.

---

## Automatic IP assignment

Every Switch gets a deterministic unique IP in `10.13.1.1–10.13.254.254`
derived from its serial number.  You can override it in `config.ini` if needed:

```ini
; sdmc:/config/lan-play/config.ini
[server]
relay_addr = relay.example.com:11451

; Optional: pin a specific IP (leave commented for auto)
; ip = 10.13.5.10
```

---

## Building from source

### Prerequisites

```sh
# Install devkitPro with Switch support
dkp-pacman -S switch-dev

# Atmosphere NPDM tool (for sysmodule)
dkp-pacman -S switch-atmo-tools
```

### Build sysmodule

```sh
cd sysmodule
make atmosphere
# Output: atmosphere/ directory — copy to SD card root
```

### Build Homebrew configurator

```sh
cd hbapp
make
# Output: lanplay-setup.nro
# Install to: sdmc:/switch/lanplay-setup/lanplay-setup.nro
```

---

## Optional auth (password-protected server)

Edit `sdmc:/config/lan-play/config.ini` and add:

```ini
username = myname
password = mysecret
```

---

## Troubleshooting

| Symptom | Check |
|---------|-------|
| Sysmodule doesn't start | Check Atmosphere version; verify `boot2.flag` exists |
| "LanPlay Setup" not in hbmenu | Verify NRO is in `sdmc:/switch/lanplay-setup/` |
| No server configured message on boot | Run "LanPlay Setup" homebrew app |
| Cannot resolve relay server | Verify WiFi is working; check `relay_addr` in config |
| High latency | Use a relay server geographically close to all players |

---

## Title ID

`010000000000FF01` — safe range for custom sysmodules.
Change in `Makefile` and `config/main.json` if needed.
