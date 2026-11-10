/* Settings web app: HTTP server on port 1994 inside fps-monitor.elf.
 * GET / serves the page; GET/POST /api/config read and edit the [overlay]
 * keys of /data/fps-monitor/config.ini.
 */
#pragma once

/* Start the server on its own detached thread. */
void web_server_start();
