switch-lan-play SD card package
================================

INSTALL
-------
Copy the CONTENTS of this folder to the ROOT of your Switch SD card.
(The folders atmosphere/, switch/, config/ go directly at the root)

CONTENTS
--------
atmosphere/contents/42000000000000B1/   <- switch-lan-play sysmodule
    Runs at boot (boot2.flag). Connects to the relay server and creates
    a virtual 10.13.0.0/16 network for the LDN layer.

atmosphere/contents/4200000000000010/   <- ldn_mitm (patched)
    Intercepts LDN calls and forwards discovery packets through the
    switch-lan-play IPC bridge (127.0.0.1:11453).

switch/lanplay-setup/lanplay-setup.nro  <- Homebrew configurator
    Run via the Homebrew Launcher to configure the relay server IP.
    Writes to sdmc:/config/lan-play/config.ini.

switch/ldnmitm_config/ldnmitm_config.nro  <- ldn_mitm config app
    Run via the Homebrew Launcher to toggle ldn_mitm settings.

switch/.overlays/ldnmitm_config.ovl    <- Tesla overlay
    Shows ldn_mitm status in the Tesla overlay menu.

config/lan-play/config.ini             <- sysmodule config template
    Edit with the IP/hostname of your relay server before first use.

REQUIREMENTS
------------
- Atmosphere CFW >= 1.5.0
- Tesla-Menu (for the overlay, optional)
- A running switch-lan-play relay server on your PC or VPS

USAGE
-----
1. Copy sd/ contents to your Switch SD root
2. Edit sdmc:/config/lan-play/config.ini — set host= to your relay server
3. Boot with Atmosphere; both sysmodules load automatically
4. Open any game that uses local wireless (LDN); it will route via the relay
