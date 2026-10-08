#include "config.h"
#include "notify.h"

#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

int main() {
    const char* path = PS5_OVERLAY_DEFAULT_CONFIG_PATH;

    /* Ensure the configuration directory exists for first-time installs. */
    if (mkdir("/data/ps5_overlay", 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "[TOGGLE] Failed to create /data/ps5_overlay (errno=%d)\n", errno);
        return 1;
    }

    OverlayConfig config{};
    if (!config_load(&config, path)) {
        /* config_load supplies defaults even when the file is missing. */
        printf("[TOGGLE] No existing config; creating one from defaults.\n");
    }

    config.enabled = !config.enabled;

    if (!config_save(&config, path)) {
        fprintf(stderr, "[TOGGLE] Failed to save %s\n", path);
        return 1;
    }

    printf("[TOGGLE] Overlay %s\n", config.enabled ? "ENABLED" : "DISABLED");
    notify_send_hud("PS5 Overlay", config.enabled ? "Overlay ENABLED" : "Overlay DISABLED");
    return 0;
}
