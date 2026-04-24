import sys

with open("sysmodule/source/main.cpp", "r") as f:
    content = f.read()

search = """    if (R_FAILED(g_rc_socket) || R_FAILED(g_rc_fs) || R_FAILED(g_rc_bsd)) {
        LLOG(LLOG_ERROR, "CRITICAL: A required service failed to initialize. Check your firmware/npdm.");
        write_status_error("Sysmodule libnx init failed!");
        svcSleepThread(5000000000LL);
        return 0;
    }"""

replace = """    if (R_FAILED(g_rc_fs)) {
        LLOG(LLOG_ERROR, "CRITICAL: FS service failed to initialize.");
        write_status_error("Sysmodule FS init failed!");
        svcSleepThread(5000000000LL);
        return 0;
    }"""

if search in content:
    content = content.replace(search, replace)
    with open("sysmodule/source/main.cpp", "w") as f:
        f.write(content)
    print("Success")
else:
    print("Search string not found")
