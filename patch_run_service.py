import sys

with open("sysmodule/source/main.cpp", "r") as f:
    content = f.read()

search = """static int run_service(void)
{
    if (R_SUCCEEDED(g_rc_fs)) ensure_tmp_dir();

    if (!g_applet_hook_installed) {
        appletHook(&g_applet_hook_cookie, applet_power_hook_cb, NULL);
        g_applet_hook_installed = true;
    }

    LLOG(LLOG_INFO, "=== switch-lan-play sysmodule v1.14 starting ===");"""

replace = """static int run_service(void)
{
    if (R_SUCCEEDED(g_rc_fs)) ensure_tmp_dir();

    LLOG(LLOG_INFO, "=== switch-lan-play sysmodule v1.14 starting stack ===");

    /* Initialize network services dynamically so they can be torn down on sleep */
    g_rc_nifm = nifmInitialize(NifmServiceType_Admin);
    if (R_FAILED(g_rc_nifm)) {
        g_rc_nifm = nifmInitialize(NifmServiceType_User);
    }

    g_rc_bsd = bsdInitialize(&g_bsd_config, g_socket_config.num_bsd_sessions, g_socket_config.bsd_service_type);
    if (R_SUCCEEDED(g_rc_bsd)) {
        g_rc_socket = socketInitialize(&g_socket_config);
    } else {
        g_rc_socket = g_rc_bsd;
    }

    if (R_FAILED(g_rc_socket) || R_FAILED(g_rc_nifm)) {
        LLOG(LLOG_ERROR, "CRITICAL: Network services failed to initialize.");
        write_status_error("Sysmodule red init failed!");
        return 1;
    }"""

if search in content:
    content = content.replace(search, replace)
    with open("sysmodule/source/main.cpp", "w") as f:
        f.write(content)
    print("Success1")
else:
    print("Search string 1 not found")

search2 = """        /* Explicit power-event handling from applet hook. */
        if (g_applet_power_event) {
            g_applet_power_event = false;

            u32 wake_ip = 0;
            if (R_SUCCEEDED(g_rc_nifm)) {
                nifmGetCurrentIpAddress(&wake_ip);
            }

            if (wake_ip == 0 || wake_ip != lp->wifi_ip || !relay_route_probe(&lp->server_addr)) {
                g_applet_power_restart_count++;
                LLOG(LLOG_WARNING,
                     "Suspend/resume network event detected (old=%08x new=%08x) — restarting service",
                     lp->wifi_ip, wake_ip);
                write_status_error("Reanudando red tras reposo...");
                break;
            } else {
                LLOG(LLOG_INFO,
                     "Suspend/resume event recovered without restart (ip=%08x relay route OK)",
                     wake_ip);
            }
        }"""

replace2 = """        /* We no longer use appletHook for power events since sysmodules have no applet context.
         * The passive safety net above (nifmGetCurrentIpAddress == 0 or changing) handles hibernation adequately. */"""

if search2 in content:
    content = content.replace(search2, replace2)
    with open("sysmodule/source/main.cpp", "w") as f:
        f.write(content)
    print("Success2")
else:
    print("Search string 2 not found")


search3 = """    ldn_bridge_close(lp);
    lan_play_free(lp);

    /* Small delay before loop restart */
    svcSleepThread(500000000LL);
    return 1; /* returning 1 here would exit main, we need the loop outside */
}"""

replace3 = """    ldn_bridge_close(lp);
    lan_play_free(lp);

    /* Tear down network services completely so they can be rebuilt cleanly
     * on the next run_service loop, resolving deep sleep/hibernation stack corruption. */
    if (R_SUCCEEDED(g_rc_socket)) socketExit();
    if (R_SUCCEEDED(g_rc_bsd)) bsdExit();
    if (R_SUCCEEDED(g_rc_nifm)) nifmExit();

    g_rc_socket = -1;
    g_rc_bsd = -1;
    g_rc_nifm = -1;

    /* Small delay before loop restart */
    svcSleepThread(500000000LL);
    return 1; /* returning 1 here would exit main, we need the loop outside */
}"""

if search3 in content:
    content = content.replace(search3, replace3)
    with open("sysmodule/source/main.cpp", "w") as f:
        f.write(content)
    print("Success3")
else:
    print("Search string 3 not found")
