#pragma once

#include "nx_common.h"

#define NX_CONFIG_PATH      "sdmc:/config/lan-play/config.ini"
#define NX_CONFIG_DIR       "sdmc:/config/lan-play"
#define NX_CONFIG_RELAY_DEF ""          /* empty = require explicit config */
#define NX_CONFIG_IP_DEF    "10.13.0.1"

#define NX_IP_MAX  16
#define NX_STR_MAX 256

typedef struct {
    char relay_addr[NX_STR_MAX]; /* e.g.  relay.example.com:11451 */
    char my_ip[NX_IP_MAX];       /* e.g.  10.13.0.1               */
    char subnet_net[NX_IP_MAX];  /* default: 10.13.0.0            */
    char subnet_mask[NX_IP_MAX]; /* default: 255.255.0.0          */
    char username[NX_STR_MAX];   /* optional auth username        */
    char password[NX_STR_MAX];   /* optional auth password        */
} nx_config_t;

/**
 * Load configuration from NX_CONFIG_PATH on the SD card.
 * Returns 0 on success, -1 if the file cannot be opened.
 * Missing keys get sensible defaults.
 */
int nx_config_load(nx_config_t *cfg);

/** Write a default config file so the user can edit it. */
int nx_config_write_default(void);
