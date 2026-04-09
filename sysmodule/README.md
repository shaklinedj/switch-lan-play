# switch-lan-play — Switch Sysmodule

Run LAN play **directly on your Nintendo Switch** (no PC required).

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

---

## Requirements

| Item | Notes |
|------|-------|
| Nintendo Switch with **Atmosphere CFW** | Any version that supports Atmosphere 0.14+ |
| SD card | Any size |
| WiFi connection | Used to reach the relay server |
| A relay server | See [server/README.md](../server/README.md) for free hosting |

---

## Quick-start (player side)

### Step 1 — Configure your Switch network

In **System Settings → Internet → Internet Settings** set your connection to
a **manual IP** in the `10.13.0.0/16` range:

```
IP Address  : 10.13.0.X    (pick a unique X for each player, e.g. 10.13.0.1, 10.13.0.2 …)
Subnet Mask : 255.255.0.0
Gateway     : 10.13.37.1   (the sysmodule virtual gateway)
DNS Primary : 1.1.1.1
DNS Secondary: 8.8.8.8
```

> Each player needs a **different** IP address in the same subnet.

### Step 2 — Create the config file

On your PC, create the following folder + file on the SD card:

```
sdmc:/config/lan-play/config.ini
```

Minimum contents:

```ini
[server]
relay_addr = YOUR_SERVER:11451

[network]
ip          = 10.13.0.1       ; change for each player
subnet_net  = 10.13.0.0
subnet_mask = 255.255.0.0
```

If you don't have a config file yet, boot the Switch — the sysmodule will
write a default template to the SD card automatically.

### Step 3 — Install the sysmodule

Copy the `atmosphere/` folder from the build output to the **root of your SD card**:

```
sdmc:/
└── atmosphere/
    └── contents/
        └── 010000000000FF01/
            ├── exefs/
            │   ├── main       ← the compiled NSO
            │   └── main.npdm  ← title permissions
            └── flags/
                └── boot2.flag ← tells Atmosphere to start at boot
```

### Step 4 — Play

1. Reboot your Switch with the SD card inserted.
2. Launch a game that supports **LAN play** (e.g. Mario Kart 8 Deluxe, Splatoon 2/3,
   Pokémon Sword/Shield, etc.).
3. Select **LAN play** in the game.
4. Other players on the same relay server will appear as if they were on your
   local network.

---

## Building from source

### Prerequisites

Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the
Nintendo Switch packages:

```sh
# Linux/macOS (via dkp-pacman)
dkp-pacman -S switch-dev

# Then install Atmosphere build tools (for npdmtool)
dkp-pacman -S switch-atmo-tools   # or build atmosphere-libs manually
```

### Build

```sh
cd sysmodule
make            # compile → lan_play_sysmodule.nso
make atmosphere # package into atmosphere/ directory layout
```

### Output

```
sysmodule/lan_play_sysmodule.nso
sysmodule/atmosphere/contents/010000000000FF01/exefs/main
sysmodule/atmosphere/contents/010000000000FF01/exefs/main.npdm
sysmodule/atmosphere/contents/010000000000FF01/flags/boot2.flag
```

Copy the `atmosphere/` directory to the root of your Switch SD card.

---

## Optional auth (password-protected server)

If your relay server requires a password, add to `config.ini`:

```ini
[auth]
username = myname
password = mysecret
```

---

## Troubleshooting

| Symptom | Check |
|---------|-------|
| Sysmodule doesn't start | Check Atmosphere version; verify `boot2.flag` exists |
| "Config missing" in logs | Create `sdmc:/config/lan-play/config.ini` |
| Cannot resolve relay server | Verify WiFi is working; check `relay_addr` in config |
| Games can't find each other | Make sure all players use different IPs in 10.13.0.0/16 |
| High latency | Use a relay server geographically close to all players |

**Reading logs:** If you have a debug setup (ftpd sysmodule + USB serial), the
sysmodule writes `[LanPlay]` prefixed lines to stderr.

---

## Title ID

`010000000000FF01` — in the safe range for custom sysmodules that do not
conflict with official titles.  Change it in `Makefile` and `config/main.json`
if you need a different ID.
