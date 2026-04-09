#include "config.h"

/* --------------------------------------------------------------------------
 * Tiny INI parser
 * Handles:
 *   [section]
 *   key = value   (leading/trailing space stripped)
 *   ; comment or # comment
 * -------------------------------------------------------------------------- */

static void trim(char *s)
{
    /* leading */
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    /* trailing */
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' '  || s[len-1] == '\t' ||
                       s[len-1] == '\r' || s[len-1] == '\n'))
        s[--len] = '\0';
}

static void set_defaults(nx_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->my_ip,       NX_CONFIG_IP_DEF,  NX_IP_MAX  - 1);
    strncpy(cfg->subnet_net,  SUBNET_NET,         NX_IP_MAX  - 1);
    strncpy(cfg->subnet_mask, SUBNET_MASK,        NX_IP_MAX  - 1);
}

int nx_config_load(nx_config_t *cfg)
{
    set_defaults(cfg);

    FILE *f = fopen(NX_CONFIG_PATH, "r");
    if (!f) {
        LLOG(LLOG_WARNING, "config: cannot open %s", NX_CONFIG_PATH);
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        trim(line);

        /* skip comments and section headers */
        if (line[0] == '\0' || line[0] == ';' ||
            line[0] == '#'  || line[0] == '[')
            continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if      (strcmp(key, "relay_addr")   == 0)
            strncpy(cfg->relay_addr,  val, NX_STR_MAX - 1);
        else if (strcmp(key, "ip")           == 0)
            strncpy(cfg->my_ip,       val, NX_IP_MAX  - 1);
        else if (strcmp(key, "subnet_net")   == 0)
            strncpy(cfg->subnet_net,  val, NX_IP_MAX  - 1);
        else if (strcmp(key, "subnet_mask")  == 0)
            strncpy(cfg->subnet_mask, val, NX_IP_MAX  - 1);
        else if (strcmp(key, "username")     == 0)
            strncpy(cfg->username,    val, NX_STR_MAX - 1);
        else if (strcmp(key, "password")     == 0)
            strncpy(cfg->password,    val, NX_STR_MAX - 1);
    }

    fclose(f);

    if (cfg->relay_addr[0] == '\0') {
        LLOG(LLOG_WARNING, "config: relay_addr not set in %s", NX_CONFIG_PATH);
        return -1;
    }

    LLOG(LLOG_INFO, "config: relay=%s ip=%s", cfg->relay_addr, cfg->my_ip);
    return 0;
}

int nx_config_write_default(void)
{
    /* Create directory if needed */
    mkdir(NX_CONFIG_DIR, 0777);

    FILE *f = fopen(NX_CONFIG_PATH, "w");
    if (!f) {
        LLOG(LLOG_ERROR, "config: cannot create %s", NX_CONFIG_PATH);
        return -1;
    }

    fprintf(f,
        "; switch-lan-play sysmodule configuration\n"
        "; Edit this file with a text editor on your PC, then put the SD\n"
        "; card back in the Switch and reboot.\n"
        "\n"
        "[server]\n"
        "; Address of the relay server (domain:port or ip:port)\n"
        "relay_addr = YOUR_SERVER:11451\n"
        "\n"
        "[network]\n"
        "; IP address the Switch will use for LAN play (10.13.0.0/16 range)\n"
        "; Each player needs a DIFFERENT IP in the same subnet.\n"
        "ip = 10.13.0.1\n"
        "subnet_net  = 10.13.0.0\n"
        "subnet_mask = 255.255.0.0\n"
        "\n"
        "[auth]\n"
        "; Optional: only needed if your relay server requires a password\n"
        "; username =\n"
        "; password =\n"
    );

    fclose(f);
    LLOG(LLOG_INFO, "config: default config written to %s", NX_CONFIG_PATH);
    return 0;
}
