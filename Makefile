# =============================================================================
# switch-lan-play — Unified top-level Makefile
#
# Targets
# -------
#   all         Build sysmodule + ldn_mitm + hbapp (requires devkitPro)
#   sysmodule   Build only the switch-lan-play sysmodule
#   ldn_mitm    Build only ldn_mitm (requires ldn_mitm submodule)
#   hbapp       Build only the LanPlay Setup homebrew configurator NRO
#   package     After a successful build, assemble everything into sd/
#   clean       Remove all build artifacts
#
# Notes
# -----
#   • Requires devkitPro with switch-dev, switch-atmo-tools packages.
#   • ldn_mitm also requires its Atmosphere-libs submodule:
#       git submodule update --init --recursive ldn_mitm
#   • Pre-built binaries are already committed in sd/ for convenience.
# =============================================================================

DEVKITPRO ?= /opt/devkitpro

# Directories
SYSMODULE_DIR := sysmodule
HBAPP_DIR     := hbapp
LDNMITM_DIR   := ldn_mitm

# SD card layout root
SD_ROOT := sd

# Title IDs
LANPLAY_TITLE := 42000000000000B1
LDNMITM_TITLE := 4200000000000010

.PHONY: all sysmodule ldn_mitm hbapp package clean

all: sysmodule ldn_mitm hbapp

# ---------------------------------------------------------------------------
# switch-lan-play sysmodule
# ---------------------------------------------------------------------------
sysmodule:
	@echo "==> Building switch-lan-play sysmodule..."
	$(MAKE) -C $(SYSMODULE_DIR) atmosphere
	@echo "==> sysmodule OK"

# ---------------------------------------------------------------------------
# ldn_mitm (submodule: ldn_mitm/)
# ---------------------------------------------------------------------------
ldn_mitm:
	@if [ ! -f $(LDNMITM_DIR)/Makefile ]; then \
	    echo "[!] ldn_mitm submodule not initialised."; \
	    echo "    Run: git submodule update --init --recursive ldn_mitm"; \
	    exit 1; \
	fi
	@echo "==> Building ldn_mitm..."
	$(MAKE) -C $(LDNMITM_DIR)
	@echo "==> ldn_mitm OK"

# ---------------------------------------------------------------------------
# Homebrew configurator NRO (LanPlay Setup)
# ---------------------------------------------------------------------------
hbapp:
	@echo "==> Building LanPlay Setup homebrew app..."
	$(MAKE) -C $(HBAPP_DIR)
	@echo "==> hbapp OK"

# ---------------------------------------------------------------------------
# package — assemble the complete SD card layout in sd/
# ---------------------------------------------------------------------------
package: sysmodule hbapp
	@echo "==> Assembling SD card package in $(SD_ROOT)/ ..."

	# switch-lan-play sysmodule
	@mkdir -p $(SD_ROOT)/atmosphere/contents/$(LANPLAY_TITLE)/exefs
	@mkdir -p $(SD_ROOT)/atmosphere/contents/$(LANPLAY_TITLE)/flags
	@cp $(SYSMODULE_DIR)/atmosphere/contents/$(LANPLAY_TITLE)/exefs/main \
	    $(SD_ROOT)/atmosphere/contents/$(LANPLAY_TITLE)/exefs/main
	@if [ -f $(SYSMODULE_DIR)/atmosphere/contents/$(LANPLAY_TITLE)/exefs/main.npdm ]; then \
	    cp $(SYSMODULE_DIR)/atmosphere/contents/$(LANPLAY_TITLE)/exefs/main.npdm \
	       $(SD_ROOT)/atmosphere/contents/$(LANPLAY_TITLE)/exefs/main.npdm; \
	fi
	@touch $(SD_ROOT)/atmosphere/contents/$(LANPLAY_TITLE)/flags/boot2.flag

	# Homebrew configurator NRO
	@mkdir -p $(SD_ROOT)/switch/lanplay-setup
	@cp $(HBAPP_DIR)/lanplay-setup.nro $(SD_ROOT)/switch/lanplay-setup/lanplay-setup.nro

	# ldn_mitm (copy from submodule build output if available)
	@if [ -f $(LDNMITM_DIR)/out/sd/atmosphere/contents/$(LDNMITM_TITLE)/exefs.nsp ]; then \
	    mkdir -p $(SD_ROOT)/atmosphere/contents/$(LDNMITM_TITLE)/flags; \
	    cp $(LDNMITM_DIR)/out/sd/atmosphere/contents/$(LDNMITM_TITLE)/exefs.nsp \
	       $(SD_ROOT)/atmosphere/contents/$(LDNMITM_TITLE)/exefs.nsp; \
	    touch $(SD_ROOT)/atmosphere/contents/$(LDNMITM_TITLE)/flags/boot2.flag; \
	    echo "    [OK] ldn_mitm from submodule build"; \
	elif [ -f $(SD_ROOT)/atmosphere/contents/$(LDNMITM_TITLE)/exefs.nsp ]; then \
	    echo "    [OK] ldn_mitm using pre-built binary in sd/"; \
	else \
	    echo "    [!] ldn_mitm binary not found — run 'make ldn_mitm' or copy manually"; \
	fi
	@if [ -f $(LDNMITM_DIR)/out/sd/switch/ldnmitm_config/ldnmitm_config.nro ]; then \
	    mkdir -p $(SD_ROOT)/switch/ldnmitm_config; \
	    cp $(LDNMITM_DIR)/out/sd/switch/ldnmitm_config/ldnmitm_config.nro \
	       $(SD_ROOT)/switch/ldnmitm_config/ldnmitm_config.nro; \
	fi
	@if [ -f $(LDNMITM_DIR)/out/sd/switch/.overlays/ldnmitm_config.ovl ]; then \
	    mkdir -p $(SD_ROOT)/switch/.overlays; \
	    cp $(LDNMITM_DIR)/out/sd/switch/.overlays/ldnmitm_config.ovl \
	       $(SD_ROOT)/switch/.overlays/ldnmitm_config.ovl; \
	fi

	@echo ""
	@echo "==> Package ready in $(SD_ROOT)/"
	@echo "    Copy the contents of $(SD_ROOT)/ to the root of your Switch SD card."

# ---------------------------------------------------------------------------
# clean
# ---------------------------------------------------------------------------
clean:
	$(MAKE) -C $(SYSMODULE_DIR) clean
	$(MAKE) -C $(HBAPP_DIR) clean
	@if [ -f $(LDNMITM_DIR)/Makefile ]; then \
	    $(MAKE) -C $(LDNMITM_DIR) clean; \
	fi
