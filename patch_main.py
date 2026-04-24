import sys

with open("sysmodule/source/main.cpp", "r") as f:
    content = f.read()

search = """/* Power-state hints from applet callbacks. */
static AppletHookCookie g_applet_hook_cookie;
static volatile bool g_applet_hook_installed = false;
static volatile bool g_applet_power_event = false;
static volatile uint32_t g_applet_power_event_count = 0;
static volatile uint32_t g_applet_power_restart_count = 0;

static void applet_power_hook_cb(AppletHookType hook, void* param)
{
    (void)hook;
    (void)param;
    g_applet_power_event = true;
    g_applet_power_event_count++;
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */
extern "C" void __appInit(void)
{
    /* Open the service manager session FIRST! */
    Result sm_rc = smInitialize();
    if (R_FAILED(sm_rc)) {
        return; /* If SM fails, everything else will too */
    }

    g_rc_fs = fsInitialize();
    if (R_SUCCEEDED(g_rc_fs)) {
        Result mount_rc = fsdevMountSdmc();
        if (R_FAILED(mount_rc)) {
            g_rc_fs = mount_rc;
        } else {
            mkdir("sdmc:/config", 0777);
            mkdir("sdmc:/config/lan-play", 0777);
            mkdir("sdmc:/tmp", 0777);
        }
        /* ULTRA EARLY LOG for debugging Atmosphere 1.1.0 boots */
        LLOG(LLOG_INFO, "=== switch-lan-play sysmodule v1.14 EARLY BOOT ===");
    }

    g_rc_setsys = setsysInitialize();

    /* Follow ldn_mitm init order: NIFM, BSD, then socket wrapper. */
    g_rc_nifm = nifmInitialize(NifmServiceType_Admin);
    if (R_FAILED(g_rc_nifm)) {
        g_rc_nifm = nifmInitialize(NifmServiceType_User);
    }

    /* BSD + socket init (same pattern as ldn_mitm) */
    g_rc_bsd = bsdInitialize(&g_bsd_config, g_socket_config.num_bsd_sessions, g_socket_config.bsd_service_type);
    if (R_SUCCEEDED(g_rc_bsd)) {
        g_rc_socket = socketInitialize(&g_socket_config);
    } else {
        g_rc_socket = g_rc_bsd;
    }

    /* Close the service manager session now that lookups are done */
    smExit();
}

extern "C" void __appExit(void)
{
    fsdevUnmountAll();
    nifmExit();
    socketExit();
    bsdExit();
    setsysExit();
    fsExit();
}"""

replace = """/* Power-state hints. Applet callbacks are omitted because
 * sysmodules lack an applet context. We rely on NIFM polling instead. */
static volatile uint32_t g_applet_power_event_count = 0; /* Kept for compat with status format */
static volatile uint32_t g_applet_power_restart_count = 0;

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */
extern "C" void __appInit(void)
{
    /* Open the service manager session FIRST! */
    Result sm_rc = smInitialize();
    if (R_FAILED(sm_rc)) {
        return; /* If SM fails, everything else will too */
    }

    g_rc_fs = fsInitialize();
    if (R_SUCCEEDED(g_rc_fs)) {
        Result mount_rc = fsdevMountSdmc();
        if (R_FAILED(mount_rc)) {
            g_rc_fs = mount_rc;
        } else {
            mkdir("sdmc:/config", 0777);
            mkdir("sdmc:/config/lan-play", 0777);
            mkdir("sdmc:/tmp", 0777);
        }
        /* ULTRA EARLY LOG for debugging Atmosphere 1.1.0 boots */
        LLOG(LLOG_INFO, "=== switch-lan-play sysmodule v1.14 EARLY BOOT ===");
    }

    g_rc_setsys = setsysInitialize();

    /* NIFM and Sockets are intentionally NOT initialized here.
     * They are dynamically initialized in the run_service loop to safely
     * recover and recreate a clean stack from hibernation. */

    /* Close the service manager session now that lookups are done */
    smExit();
}

extern "C" void __appExit(void)
{
    fsdevUnmountAll();
    setsysExit();
    fsExit();
}"""

if search in content:
    content = content.replace(search, replace)
    with open("sysmodule/source/main.cpp", "w") as f:
        f.write(content)
    print("Success")
else:
    print("Search string not found")
