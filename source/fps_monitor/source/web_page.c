/* Settings web app page (source/fps_monitor/web/index.html), served by
 * web_server.cpp. Path comes from CMake, like embedded.c.
 */
#ifndef FPS_MONITOR_WEB_PAGE
#define FPS_MONITOR_WEB_PAGE "../web/index.html"
#endif

__asm__(
    ".global fps_monitor_web_page_start\n"
    ".type   fps_monitor_web_page_start, @object\n"
    ".global fps_monitor_web_page_end\n"
    ".type   fps_monitor_web_page_end, @object\n"
    ".align  16\n"
    "fps_monitor_web_page_start:\n"
    ".incbin \"" FPS_MONITOR_WEB_PAGE "\"\n"
    "fps_monitor_web_page_end:\n");
