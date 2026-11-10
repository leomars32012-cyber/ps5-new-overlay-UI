/* Copyright (C) 2026 OnionHEN / LightningMods
 *
 * Overlay-only ShellUI module for the standalone FPS monitor.
 * Trimmed boot of shellui/src/prx.cpp: resolve what the monitor bar needs,
 * hook PUI Application.Update, then keep the module alive. Drawing and
 * sampling live in the shared shellui/src/prx_overlay.cpp.
 */
#include "defs.h"
#include "detour.h"
#include "external_symbols.hpp"
#include "hook_lifecycle.hpp"
#include "hooked_funcs.hpp"
#include "proc.h"
#include "ps5/kernel.h"
#include "ucred.h"
#include <onion/log.h>
#include <onion/log_settings.hpp>
#include <onion/ready.h>
#include <onion/settings.hpp>

#include <cstdint>
#include <ctime>
#include <unistd.h>

/* Globals the shared overlay files expect (normally defined by prx.cpp). */
onion::Settings g_settings;
OverlayLayout g_overlay_layout;
MonoImage *pui_img = nullptr;
MonoObject *Game = nullptr;
MonoObject *rootWidget = nullptr;
MonoObject *font = nullptr;
bool has_hv_bypass = false;
void (*OnRender_orig)(MonoObject *instance);

void OnRender_Hook(MonoObject *instance);
void overlay_reload_settings();

namespace {

constexpr size_t kMprotectProbeSize = 100;
constexpr int kProtRwx = 0x7; // PROT_READ | PROT_WRITE | PROT_EXEC
constexpr unsigned kKeepAliveSleepSec = 0x100000;
bool hooked = false;

struct AuthIdGuard {
  pid_t pid;
  uintptr_t old_authid;
  ~AuthIdGuard() { set_proc_authid(pid, old_authid); }
};

bool resolve_symbols(pid_t pid) {
  const int libkernelsys = get_module_handle(pid, "libkernel_sys.sprx");
  KERNEL_DLSYM(libkernelsys, sceKernelMprotect);
  KERNEL_DLSYM(libkernelsys, sceKernelGetSocPowerConsumption); /* optional */
  KERNEL_DLSYM(libkernelsys, sceKernelGetCurrentFanDuty); /* optional */
  KERNEL_DLSYM(libkernelsys, sceKernelGetSocClock); /* optional */
  KERNEL_DLSYM(libkernelsys, sceKernelGetCpuCoreClock); /* optional */
  KERNEL_DLSYM(libkernelsys, sceKernelGetCpuFrequency); /* optional */

  const int libmono = get_module_handle(pid, "libmonosgen-2.0.sprx");
  KERNEL_DLSYM(libmono, mono_get_root_domain);
  KERNEL_DLSYM(libmono, mono_thread_attach);
  KERNEL_DLSYM(libmono, mono_domain_assembly_open);
  KERNEL_DLSYM(libmono, mono_assembly_get_image);
  KERNEL_DLSYM(libmono, mono_class_from_name);
  KERNEL_DLSYM(libmono, mono_class_get_method_from_name);
  KERNEL_DLSYM(libmono, mono_compile_method);
  KERNEL_DLSYM(libmono, mono_runtime_invoke);
  KERNEL_DLSYM(libmono, mono_string_new);
  KERNEL_DLSYM(libmono, mono_domain_get);
  KERNEL_DLSYM(libmono, mono_class_get_property_from_name);
  KERNEL_DLSYM(libmono, mono_property_get_get_method);
  KERNEL_DLSYM(libmono, mono_property_get_set_method);
  KERNEL_DLSYM(libmono, mono_object_new);
  KERNEL_DLSYM(libmono, mono_object_unbox);
  KERNEL_DLSYM(libmono, mono_runtime_object_init);
  /* The scene renderer pins every widget it caches. */
  KERNEL_DLSYM(libmono, mono_gchandle_new);
  KERNEL_DLSYM(libmono, mono_gchandle_free); /* optional; null if missing */
  const struct {
    const char *name;
    bool found;
  } required[] = {
      {"sceKernelMprotect", sceKernelMprotect != nullptr},
      {"mono_get_root_domain", mono_get_root_domain != nullptr},
      {"mono_thread_attach", mono_thread_attach != nullptr},
      {"mono_domain_assembly_open", mono_domain_assembly_open != nullptr},
      {"mono_assembly_get_image", mono_assembly_get_image != nullptr},
      {"mono_class_from_name", mono_class_from_name != nullptr},
      {"mono_class_get_method_from_name",
       mono_class_get_method_from_name != nullptr},
      {"mono_compile_method", mono_compile_method != nullptr},
      {"mono_runtime_invoke", mono_runtime_invoke != nullptr},
      {"mono_string_new", mono_string_new != nullptr},
      {"mono_domain_get", mono_domain_get != nullptr},
      {"mono_class_get_property_from_name",
       mono_class_get_property_from_name != nullptr},
      {"mono_property_get_get_method", mono_property_get_get_method != nullptr},
      {"mono_property_get_set_method", mono_property_get_set_method != nullptr},
      {"mono_object_new", mono_object_new != nullptr},
      {"mono_object_unbox", mono_object_unbox != nullptr},
      {"mono_runtime_object_init", mono_runtime_object_init != nullptr},
      {"mono_gchandle_new", mono_gchandle_new != nullptr},
  };
  bool ok = true;
  for (const auto &sym : required) {
    if (!sym.found) {
      LOG_ERROR("fps-monitor: symbol %s not resolved", sym.name);
      ok = false;
    }
  }
  if (!ok)
    LOG_ERROR("fps-monitor: symbol resolution failed (libkernel_sys=%d "
              "libmonosgen=%d)",
              libkernelsys, libmono);
  if (!mono_gchandle_free)
    LOG_WARN("fps-monitor: mono_gchandle_free not resolved; widget pins leak");
  return ok;
}

time_t g_config_mtime = 0;

/* Same policy as the host: the FPS bar, never IP / fan. [overlay] enabled
 * is the user's on/off switch (settings web app). */
void load_settings() {
  /* Stat first: a write landing during the read shows up as newer next time. */
  const time_t mtime = onion::settings_config_newest_mtime();
  onion::Settings s{};
  /* false = defaults kept; a rejected file is warned about in settings.cpp. */
  (void)onion::settings_load(&s);
  onion::apply_log_settings(s);
  g_settings = s;
  apply_overlay_layout();
  /* mtime has 1 s resolution: a file written this second may change again
   * within it, so keep it looking newer for one more check. */
  g_config_mtime = time(nullptr) == mtime ? mtime - 1 : mtime;
}

/* prx.cpp resolve_game_container: the in-game layer the bar attaches to. */
bool resolve_game_container() {
  MonoImage *app_system = getDLLimage("Sce.Vsh.ShellUI.AppSystem.dll");
  if (!app_system)
    return false;
  MonoClass *layer_manager = mono_class_from_name(
      app_system, "Sce.Vsh.ShellUI.AppSystem", "LayerManager");
  MonoMethod *find =
      layer_manager ? mono_class_get_method_from_name(
                          layer_manager, "FindContainerSceneByPath", 1)
                    : nullptr;
  if (!find) {
    LOG_ERROR("fps-monitor: LayerManager.FindContainerSceneByPath missing");
    return false;
  }
  void *args[1] = {mono_string_new(mono_domain_get(), "Game")};
  MonoObject *exception = nullptr;
  Game = mono_runtime_invoke(find, nullptr, args, &exception);
  if (exception || !Game) {
    LOG_ERROR("fps-monitor: Game scene lookup failed (exception=%p scene=%p)",
              static_cast<void *>(exception), static_cast<void *>(Game));
    return false;
  }
  return true;
}

bool install_render_hook() {
  char probe[kMprotectProbeSize];
  const int mprotect_rc = sceKernelMprotect(probe, sizeof(probe), kProtRwx);
  has_hv_bypass = (mprotect_rc == 0);
  LOG_INFO("fps-monitor: mprotect RWX probe rc=0x%x (hv bypass %s)",
           mprotect_rc, has_hv_bypass ? "yes" : "no");
  const uint64_t addr = Get_Address_of_Method(pui_img, "Sce.PlayStation.PUI",
                                              "Application", "Update", 0);
  if (!addr) {
    LOG_ERROR("fps-monitor: Application.Update address not resolved");
    return false;
  }
  if (!InstallDetour(addr, reinterpret_cast<void *>(&OnRender_Hook),
                     reinterpret_cast<void **>(&OnRender_orig))) {
    LOG_ERROR("fps-monitor: Application.Update hook failed at 0x%lx",
              static_cast<unsigned long>(addr));
    return false;
  }
  return true;
}

} // namespace

/* OnRender_Hook calls this about once a second on the UI thread, so
 * g_settings never changes under the render path. No AuthIdGuard: the
 * /user/data view is readable without it (the log is written there too). */
void overlay_reload_settings() {
  /* Reload when the file's mtime changes; a one-time note when the file goes
   * away (the last loaded settings stay in effect). */
  static bool config_missing = false;
  const time_t newest = onion::settings_config_newest_mtime();
  if (newest == 0) {
    if (!config_missing && g_config_mtime > 0)
      LOG_WARN("fps-monitor: config.ini removed; keeping last settings");
    config_missing = true;
    return;
  }
  config_missing = false;
  /* Any change, not only newer: an upload may keep an older file date. */
  if (newest == g_config_mtime)
    return;
  load_settings();
  LOG_INFO("fps-monitor: settings reloaded (overlay %s)",
           g_settings.overlay_enabled ? "on" : "off");
}

int main(int argc, char const *argv[]) {
  (void)argc;
  (void)argv;
  if (hooked)
    return 0;

  const pid_t pid = getpid();
  {
    AuthIdGuard auth{pid, set_ucred_to_ptrace()};
    /* ShellUI's sandbox sees /user/data, not /data (see prx.cpp main). */
    onion_log_configure("FPSMon", "/user/data/fps-monitor/fps-monitor.log");
    /* Step logs: a hang/crash during ShellUI boot otherwise leaves no trace. */
    LOG_INFO("fps-monitor: module start pid=%d", static_cast<int>(pid));
    if (auth.old_authid == 0)
      LOG_WARN("fps-monitor: authid elevation failed for pid=%d",
               static_cast<int>(pid));

    if (!resolve_symbols(pid))
      return -1;
    load_settings();

    Root_Domain = mono_get_root_domain();
    if (!Root_Domain) {
      LOG_ERROR("fps-monitor: no Mono root domain");
      return -1;
    }
    if (!mono_thread_attach(Root_Domain))
      LOG_WARN("fps-monitor: mono_thread_attach returned null");
    else
      LOG_INFO("fps-monitor: mono attached");

    pui_img = getDLLimage("Sce.PlayStation.PUI.dll");
    if (!pui_img) {
      LOG_ERROR("fps-monitor: Sce.PlayStation.PUI.dll not found");
      return -1;
    }
    if (!resolve_game_container())
      return -1;
    LOG_INFO("fps-monitor: Game scene resolved");

    shellui_hooks_begin_install();
    if (!install_render_hook()) {
      shellui_hooks_publish_failed();
      return -1;
    }
  } // restore authid before long-running work

  shellui_hooks_publish_ready();
  hooked = true;
  LOG_INFO("fps-monitor: overlay hooked in SceShellUI pid=%d",
           static_cast<int>(pid));

  /* Host waits on this marker; the thread must never return. */
  (void)onion_ready_signal_pid(ONION_READY_FPS_MONITOR_UI, pid); // ready.c logs failures
  while (true)
    sleep(kKeepAliveSleepSec);
}
