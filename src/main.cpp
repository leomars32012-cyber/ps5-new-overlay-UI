#include "ps5_overlay.h"
#include "monitor.h"
#include "overlay_ui.h"
#include "notify.h"
#include "config.h"
#include "shellui_inject.h"
#include "embedded_shellui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>

static volatile bool s_running = true;
static const char* kPidPath = "/data/ps5_overlay/ps5_overlay.pid";

static void signal_handler(int sig) {
    (void)sig;
    s_running = false;
}

static bool read_pid_file(pid_t* pid_out) {
    if (!pid_out) return false;
    FILE* fp = fopen(kPidPath, "r");
    if (!fp) return false;
    long value = 0;
    bool ok = (fscanf(fp, "%ld", &value) == 1 && value > 1);
    fclose(fp);
    if (!ok) return false;
    *pid_out = (pid_t)value;
    return true;
}

static bool pid_is_alive(pid_t pid) {
    if (pid <= 1) return false;
    if (kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

static bool create_pid_file(void) {
    int fd = open(kPidPath, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return false;

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    bool ok = (write(fd, buf, len) == len);
    close(fd);

    if (!ok) unlink(kPidPath);
    return ok;
}

static void remove_pid_file(void) {
    pid_t pid = 0;
    if (read_pid_file(&pid) && pid == getpid()) {
        unlink(kPidPath);
    }
}

static bool toggle_existing_daemon(void) {
    pid_t existing_pid = 0;
    if (!read_pid_file(&existing_pid)) return false;

    if (!pid_is_alive(existing_pid)) {
        unlink(kPidPath);
        return false;
    }

    OverlayConfig config{};
    if (!config_load(&config, PS5_OVERLAY_DEFAULT_CONFIG_PATH)) {
        /*
         * A resident daemon without config.ini is a valid first-launch
         * state. config_load() already populated safe defaults, so create
         * the persistent file now and use the default enabled=true state.
         */
        if (!config_save(&config, PS5_OVERLAY_DEFAULT_CONFIG_PATH)) {
            fprintf(stderr, "[TOGGLE] Existing daemon found, but config initialization failed.\n");
            return true;
        }
    }

    config.enabled = !config.enabled;
    if (!config_save(&config, PS5_OVERLAY_DEFAULT_CONFIG_PATH)) {
        fprintf(stderr, "[TOGGLE] Failed to save %s\n", PS5_OVERLAY_DEFAULT_CONFIG_PATH);
        return true;
    }

    printf("[TOGGLE] Overlay %s (daemon PID %d)\n",
           config.enabled ? "ENABLED" : "DISABLED",
           (int)existing_pid);

    bool toast_sent = notify_send_hud("PS5 Overlay",
                                      config.enabled ? "Overlay ENABLED" : "Overlay DISABLED");
    if (!toast_sent) {
        fprintf(stderr, "[TOGGLE] State saved, but sceNotificationSend did not confirm the toast.\\n");
    }
    return true;
}

int main(int argc, char** argv) {
    printf("=========================================\n");
    printf("   PS5 Hardware Overlay v%s\n", PS5_OVERLAY_VERSION);
    printf("=========================================\n");

    bool test_mode = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("ps5_overlay version %s\n", PS5_OVERLAY_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--test") == 0 || strcmp(argv[i], "-t") == 0) {
            test_mode = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: ps5_overlay [options]\n");
            printf("Options:\n");
            printf("  -v, --version  Show version\n");
            printf("  -t, --test     Run one sample test and exit\n");
            printf("  -h, --help     Show this help message\n");
            return 0;
        }
    }

    /*
     * Single-ELF persistent toggle:
     * - First launch starts the resident overlay daemon.
     * - Launching the same ELF again while the daemon is running toggles
     *   the persistent state and exits immediately.
     * Use this from the PS5 home screen, not during gameplay.
     */
    if (!test_mode) {
        if (mkdir("/data/ps5_overlay", 0777) != 0 && errno != EEXIST) {
            fprintf(stderr, "[ERROR] Failed to create /data/ps5_overlay (errno=%d)\n", errno);
            return 1;
        }

        if (toggle_existing_daemon()) {
            return 0;
        }

        unlink(kPidPath);

        if (!create_pid_file()) {
            if (toggle_existing_daemon()) return 0;
            fprintf(stderr, "[ERROR] Failed to create daemon PID file.\n");
            return 1;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    OverlayConfig config;
    bool config_loaded = config_load(&config, PS5_OVERLAY_DEFAULT_CONFIG_PATH);

    if (config_loaded) {
        printf("[CONFIG] Loaded settings from %s\n", PS5_OVERLAY_DEFAULT_CONFIG_PATH);
    } else {
        /*
         * First launch: config_load() has already populated config with
         * safe defaults. Persist those defaults immediately so the ShellUI
         * injector and the second invocation of this ELF share the same
         * authoritative state file.
         */
        if (config_save(&config, PS5_OVERLAY_DEFAULT_CONFIG_PATH)) {
            printf("[CONFIG] Created default settings at %s\n",
                   PS5_OVERLAY_DEFAULT_CONFIG_PATH);
        } else {
            fprintf(stderr, "[WARNING] Could not create %s; using in-memory defaults.\n",
                    PS5_OVERLAY_DEFAULT_CONFIG_PATH);
        }
    }

    if (!config.enabled) {
        printf("[INFO] Overlay starts disabled; persistent toggle remains active.\n");
    }

    if (!monitor_init()) {
        fprintf(stderr, "[ERROR] Failed to initialize hardware monitor.\n");
        remove_pid_file();
        return 1;
    }

    if (!overlay_ui_init(&config)) {
        fprintf(stderr, "[WARNING] Overlay UI init returned false; proceeding with fallback.\n");
    }

    bool hud_injected = false;
    if (!test_mode) {
        unlink("/system_tmp/ps5_overlay_ready");
        unlink("/system_tmp/ps5_overlay.log");

        pid_t shellui_pid = shellui_find_pid();
        if (shellui_pid > 0) {
            printf("[STATUS] Found SceShellUI (PID: %d). Injecting in-game overlay...\n", shellui_pid);
            if (shellui_inject_elf(shellui_pid, g_overlay_shellui_elf, g_overlay_shellui_elf_size)) {
                printf("[STATUS] Successfully injected HUD into SceShellUI!\n");
                hud_injected = true;
            } else {
                fprintf(stderr, "[WARNING] Failed to inject HUD into SceShellUI.\n");
            }
        } else {
            fprintf(stderr, "[WARNING] SceShellUI process not found.\n");
        }
    }

    if (hud_injected) {
        notify_send_hud("PS5 Overlay Active", "HUD Injected! Launch any game to view overlay");
    } else {
        notify_send_hud("PS5 Overlay Warning", "HUD injection failed. Check /system_tmp/ps5_overlay.log");
    }

    printf("[STATUS] Overlay daemon running. Toast interval: %d s | Polling: %d ms\n",
           config.toast_interval_sec, config.update_interval_ms);

    HardwareMetrics metrics{};
    char hud_text[256]{};
    char hud_line1[128]{};
    char hud_line2[128]{};
    time_t last_toast_time = 0;

    while (s_running) {
        /*
         * config.ini is the shared source of truth. Reload it while resident
         * so a second launcher invocation is reflected by the daemon too.
         * ShellUI independently reads this same file for immediate teardown.
         */
        OverlayConfig latest_config{};
        if (config_load(&latest_config, PS5_OVERLAY_DEFAULT_CONFIG_PATH)) {
            config = latest_config;
        }

        if (monitor_update(&metrics)) {
            monitor_format_hud_string(&metrics, &config, hud_text, sizeof(hud_text));
            monitor_format_hud_lines(&metrics, &config, hud_line1, sizeof(hud_line1), hud_line2, sizeof(hud_line2));

            overlay_ui_update(hud_text);

            if (config.toast_notifications) {
                time_t now = time(nullptr);
                if (now - last_toast_time >= config.toast_interval_sec) {
                    notify_send_hud(hud_line1, hud_line2);
                    last_toast_time = now;
                }
            }
        }

        if (test_mode) {
            printf("[TEST] HUD Line 1: %s\n", hud_line1);
            printf("[TEST] HUD Line 2: %s\n", hud_line2);
            printf("[TEST] HUD Full:   %s\n", hud_text);
            break;
        }

        usleep(config.update_interval_ms * 1000);
    }

    printf("\n[STATUS] Shutting down PS5 Overlay daemon...\n");
    overlay_ui_shutdown();
    monitor_cleanup();
    remove_pid_file();
    printf("[STATUS] Goodbye!\n");
    return 0;
}