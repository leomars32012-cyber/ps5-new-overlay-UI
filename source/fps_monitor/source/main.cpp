/* Copyright (C) 2026 OnionHEN / LightningMods
 *
 * Standalone FPS monitor host. Runs the daemon FPS sampler (daemon_fps.cpp)
 * and keeps the overlay-only ShellUI ELF injected across rest mode and
 * ShellUI restarts. No Toolbox, IPC, plugins or cheats.
 */
#include "daemon_ops.hpp"
#include "daemon_power_state.hpp"
#include "globalconf.hpp"
#include "toolbox_injection.hpp"
#include "web_server.hpp"

#include <onion/elfldr.h>
#include <onion/log.h>
#include <onion/log_settings.hpp>
#include <onion/notify.h>
#include <onion/proc_query.h>
#include <onion/ready.h>
#include <onion/settings.hpp>

#include <ps5/kernel.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef struct app_info {
  uint32_t app_id;
  uint64_t unknown1;
  uint32_t app_type;
  char title_id[10];
  char unknown2[0x3c];
} app_info_t;

extern "C" {
#include <ps5/payload.h>

bool Inject_Toolbox(int pid, uint8_t *elf);
extern uint8_t monitor_shellui_elf_start[];

int sceKernelSendNotificationRequest(int32_t device, void *req, size_t size,
                                     int32_t blocking);
int sceKernelGetProcessName(int pid, char *name);
int sceKernelGetAppInfo(int pid, app_info_t *info);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
}

/* Symbols daemon_fps.cpp and libhijacker expect from their host process. */
onion::SettingsStore g_settings;
std::atomic_bool g_stack_shutting_down{false};
uintptr_t kernel_base = 0;

namespace {

constexpr const char *kLogDir = "/data/fps-monitor";
constexpr const char *kLogPath = "/data/fps-monitor/fps-monitor.log";
constexpr const char *kCrashLogPath = "/data/fps-monitor/crash.log";
constexpr const char *kShellUiReadySprx[] = {
    "libSceNpTrophy.sprx",
    "libSceNpTrophy2.sprx",
};
constexpr int kSprxPollMax = 60;
constexpr useconds_t kSprxPollUs = 500 * 1000;
constexpr int kInjectTimeoutMs = 45 * 1000;
constexpr int kInjectPollMs = 250;
constexpr int kInjectMaxFailures = 3;
/* Injecting into a still-booting ShellUI (payload autoloaded at boot next to
 * kstuff etc.) hung its Mono UI and got it killed; wait for it to settle.
 * shortcut: fixed guesses, retune if console logs show earlier is safe. */
constexpr time_t kBootSettleSec = 120;
constexpr time_t kShellUiSettleSec = 10;
/* A ShellUI that dies this soon after an inject (before or after the overlay
 * reported ready) counts as crashed by us. kMaxShellUiCrashes in a row stops
 * injecting for the session: a restarted ShellUI has a new pid, so the
 * per-pid kInjectMaxFailures alone would retry a crashing overlay forever. */
constexpr time_t kCrashWindowSec = 120;
constexpr int kMaxShellUiCrashes = 3;

/* Atomic: the web server thread also calls LoadSettings after a save. */
std::atomic<time_t> g_config_mtime{0};
std::atomic_bool g_config_loaded{false};

/* The monitor is the FPS bar: always sample FPS, never show IP / fan.
 * overlay.enabled is the user's on/off switch (settings web app); while it
 * is off the FPS samplers idle. */
void apply_monitor_policy(onion::Settings &s) {
  s.overlay_fps = true;
  s.overlay_ip = false;
  s.overlay_fan = false;
}

const char *fault_name(int sig) {
  switch (sig) {
  case SIGSEGV:
    return "SIGSEGV";
  case SIGBUS:
    return "SIGBUS";
  case SIGILL:
    return "SIGILL";
  case SIGFPE:
    return "SIGFPE";
  case SIGABRT:
    return "SIGABRT";
  case SIGSYS:
    return "SIGSYS";
  default:
    return "?";
  }
}

/* Async-signal-safe only: onion_log_emergency, thr_self, raise. SA_RESETHAND
 * already restored SIG_DFL, so the re-raise kills the process as before. */
void on_fault(int sig, siginfo_t *info, void *ctx) {
  long tid = -1;
  (void)syscall(SYS_thr_self, &tid);
  const ucontext_t *uc = static_cast<const ucontext_t *>(ctx);
  onion_log_emergency(
      "fps-monitor: CRASH %s (%d) code=%d addr=%p rip=%#lx rsp=%#lx tid=%ld",
      fault_name(sig), sig, info ? info->si_code : 0,
      info ? info->si_addr : nullptr,
      uc ? static_cast<unsigned long>(uc->uc_mcontext.mc_rip) : 0UL,
      uc ? static_cast<unsigned long>(uc->uc_mcontext.mc_rsp) : 0UL, tid);
  raise(sig);
}

/* shortcut: the alt stack covers the main thread only (sigaltstack is
 * per-thread); a stack overflow on a sampler/web thread dies unlogged. */
void install_crash_handler() {
  static char alt_stack[64 * 1024];
  stack_t ss{};
  ss.ss_sp = alt_stack;
  ss.ss_size = sizeof(alt_stack);
  if (sigaltstack(&ss, nullptr) != 0)
    LOG_WARN("fps-monitor: sigaltstack failed errno=%d", errno);
  struct sigaction sa {};
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);
  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS})
    if (sigaction(sig, &sa, nullptr) != 0)
      LOG_WARN("fps-monitor: sigaction(%s) failed errno=%d", fault_name(sig),
               errno);
}

/* [logging] level / max_bytes from config.ini; log the level on change. */
void apply_logging(const onion::Settings &s) {
  static std::atomic<int> applied_level{-1}; // sampler + web threads reload
  const onion_log_level level = onion::apply_log_settings(s);
  const int lvl = static_cast<int>(level);
  if (applied_level.exchange(lvl) != lvl)
    LOG_INFO("fps-monitor: log level %s, max_bytes=%d",
             onion_log_level_name(level), s.log_max_bytes);
}

const char *inject_result_name(onion::ToolboxInjectionResult r) {
  switch (r) {
  case onion::ToolboxInjectionResult::AlreadyReady:
    return "AlreadyReady";
  case onion::ToolboxInjectionResult::Injected:
    return "Injected";
  case onion::ToolboxInjectionResult::TargetNotFound:
    return "TargetNotFound";
  case onion::ToolboxInjectionResult::InjectFailed:
    return "InjectFailed";
  case onion::ToolboxInjectionResult::ReadyTimeout:
    return "ReadyTimeout";
  case onion::ToolboxInjectionResult::TargetChanged:
    return "TargetChanged";
  case onion::ToolboxInjectionResult::TargetDied:
    return "TargetDied";
  }
  return "?";
}

bool alive_marker(const char *name) {
  pid_t pid = -1;
  return onion_ready_read_pid(name, &pid) && pid > 1 && pid != getpid() &&
         onion_proc_is_alive(pid);
}

bool sprx_loaded(pid_t pid, const char *name) {
  uint32_t handle = static_cast<uint32_t>(-1);
  return kernel_dynlib_handle(pid, name, &handle) == 0 &&
         handle != static_cast<uint32_t>(-1);
}

/* ShellUI accepts an ELF only after its trophy SPRX are mapped
 * (same gate as daemon_inject.cpp toolbox_wait_shellui_sprx). */
bool wait_shellui_ready(pid_t pid) {
  static pid_t warned_pid = -1; // retried every ~30 s: warn once per ShellUI
  for (const char *name : kShellUiReadySprx) {
    int i = 0;
    for (; i < kSprxPollMax && !sprx_loaded(pid, name); ++i) {
      if (!onion_proc_is_alive(pid)) {
        LOG_DEBUG("fps-monitor: SceShellUI pid=%d exited while waiting",
                  static_cast<int>(pid));
        return false;
      }
      usleep(kSprxPollUs);
    }
    if (i == kSprxPollMax) {
      if (warned_pid != pid)
        LOG_WARN("fps-monitor: %s missing in SceShellUI pid=%d", name,
                 static_cast<int>(pid));
      warned_pid = pid;
      return false;
    }
  }
  return true;
}

void ensure_overlay(onion::ToolboxInject &injector) {
  if (daemon_power_state_is_sleeping())
    return;
  const pid_t pid = onion_find_pid("SceShellUI");
  /* Uptime; log timestamps use the same clock. */
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  /* The overlay has no crash handler inside ShellUI; a pid change is the
   * only trace a ShellUI crash leaves here. */
  static pid_t last_pid = -1;
  static time_t last_inject_at = 0;
  static int crash_streak = 0;
  static bool gave_up = false;
  if (pid != last_pid) {
    if (last_pid > 1) {
      const bool ours = last_inject_at != 0 &&
                        ts.tv_sec - last_inject_at <= kCrashWindowSec;
      crash_streak = ours ? crash_streak + 1 : 0;
      LOG_WARN("fps-monitor: SceShellUI pid=%d gone (restarted or crashed?)"
               "%s",
               static_cast<int>(last_pid),
               ours ? " within the crash window of our inject" : "");
      if (ours && crash_streak >= kMaxShellUiCrashes && !gave_up) {
        gave_up = true;
        LOG_ERROR("fps-monitor: SceShellUI died %d times in a row right after "
                  "the overlay was injected; not injecting again this session",
                  crash_streak);
        onion_notify(0, "[FPS Monitor] overlay disabled: the system UI kept "
                        "crashing. See fps-monitor.log");
      }
    }
    last_pid = pid;
  }
  if (gave_up || pid <= 1 || injector.is_ready_for(pid))
    return;
  /* How long we have seen this pid. Not the pid's real age: a payload sent
   * later still waits kShellUiSettleSec once. */
  static pid_t seen_pid = -1;
  static time_t seen_at = 0;
  if (pid != seen_pid) {
    seen_pid = pid;
    seen_at = ts.tv_sec;
  }
  if (ts.tv_sec < kBootSettleSec || ts.tv_sec - seen_at < kShellUiSettleSec)
    return;
  /* A ShellUI that rejected the overlay kInjectMaxFailures times is left
   * alone until it restarts (new pid); one toast per ShellUI. */
  static pid_t failed_pid = -1;
  static int failures = 0;
  if (pid == failed_pid && failures >= kInjectMaxFailures)
    return;
  if (!wait_shellui_ready(pid))
    return;

  LOG_INFO("fps-monitor: injecting overlay into SceShellUI pid=%d",
           static_cast<int>(pid));
  last_inject_at = ts.tv_sec ? ts.tv_sec : 1; // 0 means "never injected"
  const onion::ToolboxInjectionOutcome outcome = injector.inject(
      []() -> pid_t { return onion_find_pid("SceShellUI"); },
      [](pid_t target) -> bool {
        return Inject_Toolbox(static_cast<int>(target),
                              monitor_shellui_elf_start);
      },
      kInjectTimeoutMs, kInjectPollMs, pid);
  if (outcome.ready()) {
    LOG_INFO("fps-monitor: overlay online pid=%d",
             static_cast<int>(outcome.pid));
    failed_pid = -1;
    return;
  }
  LOG_ERROR("fps-monitor: overlay inject failed pid=%d result=%s",
            static_cast<int>(outcome.pid), inject_result_name(outcome.result));
  if (outcome.pid != failed_pid) {
    failed_pid = outcome.pid;
    failures = 0;
    onion_notify(0, "[FPS Monitor] overlay failed to load");
  }
  if (++failures == kInjectMaxFailures)
    LOG_WARN("fps-monitor: giving up on SceShellUI pid=%d until it restarts",
             static_cast<int>(outcome.pid));
}

} // namespace

/* daemon_fps.cpp polls this once per second; re-read only when the file
 * changed. Unlike the daemon, never create a default config. */
bool LoadSettings(bool force) {
  /* mtime before the load: a write landing mid-load is seen next poll. Any
   * change counts (an upload may keep an older date); a deleted file keeps
   * the last settings, as the overlay does. */
  const time_t mtime = onion::settings_config_newest_mtime();
  if (!force && g_config_loaded.load() &&
      (mtime == 0 || mtime == g_config_mtime.load()))
    return true;
  onion::Settings s{};
  /* false = defaults kept; a rejected file is warned about in settings.cpp. */
  (void)onion::settings_load(&s);
  apply_logging(s);
  apply_monitor_policy(s);
  g_settings.store(s);
  g_config_mtime.store(mtime);
  g_config_loaded.store(true);
  return true;
}

int main() {
  (void)syscall(SYS_thr_set_name, -1, "fps-monitor.elf");
  payload_args_t *args = payload_get_args();
  if (args)
    kernel_base = args->kdata_base_addr;
  const int priv_rc = elfldr_raise_privileges(getpid());

  const int mkdir_errno = mkdir(kLogDir, 0777) == 0 ? 0 : errno;
  onion_log_configure("FPSMon", kLogPath);
  onion_log_configure_crash(kCrashLogPath);
  install_crash_handler();
  /* Failures from before the log existed (klog still gets these if the
   * file sink could not open). */
  if (!args)
    LOG_ERROR("fps-monitor: no payload args; kernel_base unknown");
  if (priv_rc != 0)
    LOG_ERROR("fps-monitor: raising privileges failed rc=%d", priv_rc);
  if (mkdir_errno != 0 && mkdir_errno != EEXIST)
    LOG_ERROR("fps-monitor: mkdir %s failed errno=%d", kLogDir, mkdir_errno);
  onion_notify_set_send(reinterpret_cast<onion_notify_send_fn>(
      sceKernelSendNotificationRequest));

  if (alive_marker(ONION_READY_FPS_MONITOR)) {
    LOG_WARN("fps-monitor: already running; exiting");
    onion_notify(0, "[FPS Monitor] already running");
    return 0;
  }
  (void)onion_ready_signal_pid(ONION_READY_FPS_MONITOR, getpid()); // ready.c logs failures

  (void)LoadSettings(true);
  onion_proc_set_sce_hooks(
      [](int pid, char *name) -> int {
        return sceKernelGetProcessName(pid, name);
      },
      [](pid_t pid, void *info) -> int {
        return sceKernelGetAppInfo(pid, static_cast<app_info_t *>(info));
      },
      []() -> int { return sceSystemServiceGetAppIdOfRunningBigApp(); });

  pthread_t fps_thr = nullptr;
  pthread_t vsync_thr = nullptr;
  if (const int rc =
          pthread_create(&fps_thr, nullptr, fps_sampler_thread, nullptr))
    LOG_ERROR("fps-monitor: fps sampler thread failed to start rc=%d", rc);
  if (const int rc = pthread_create(&vsync_thr, nullptr,
                                    vsync_fps_sampler_thread, nullptr))
    LOG_ERROR("fps-monitor: vsync sampler thread failed to start rc=%d", rc);

  LOG_INFO("fps-monitor: started pid=%d", static_cast<int>(getpid()));
  onion_notify(0, "[FPS Monitor] started");
  web_server_start();

  onion::ToolboxInject injector(ONION_READY_FPS_MONITOR_UI);
  while (!g_stack_shutting_down.load(std::memory_order_acquire)) {
    ensure_overlay(injector);
    sleep(1);
  }
  LOG_INFO("fps-monitor: shutting down");
  return 0;
}
