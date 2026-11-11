/* Copyright (C) 2025 OnionHEN / LightningMods
 *
 * fps-monitor game overlay: onionHEN 0.9.0's scene overlay (minimal pill,
 * compact line, corner panel, full panel) without the legacy edge bar or the
 * Toolbox polls.
 */

#include "hooked_funcs.hpp"
#include "ipc.hpp"
#include "external_symbols.hpp"
#include "overlay_scene.hpp"
#include "overlay_scene_renderer.hpp"
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
#include "overlay_perf_diagnostics.hpp"
#define ONION_TELEMETRY_CALL(call, expression)                                \
  onion::overlay::perf::measure_telemetry_call(                                \
      onion::overlay::perf::TelemetryCall::call, [&]() { return (expression); })
#else
#define ONION_TELEMETRY_CALL(call, expression) (expression)
#endif
#include <onion/fps_sample.h>
#include <onion/net.h>
#include <onion/settings.hpp>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern MonoObject* rootWidget;
extern OverlayLayout g_overlay_layout;
extern onion::Settings g_settings;
void overlay_reload_settings(); /* shellui/main.cpp */
#include "shellui_state.hpp"

struct OrbisKernelTimespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* sceKernelGetCpuUsage returns timeval-style CPU counters, not timespecs. */
struct OrbisKernelTimeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct Proc_Stats
{
    int32_t lo_data;								//0x00
    uint32_t td_tid;						//0x04
    OrbisKernelTimeval user_cpu_usage_time;	//0x08
    OrbisKernelTimeval system_cpu_usage_time;  //0x18
}; //0x28

extern "C" {
    int sceKernelGetSocSensorTemperature(int sensorId, int* soctime);
    int get_page_table_stats(int vm, int type, int* total, int* free);
    int sceKernelGetCpuUsage(struct Proc_Stats* out, int32_t* size);
    int sceKernelGetThreadName(uint32_t id, char* out);
	int sceKernelGetCpuTemperature(int* cputemp);
    int sceKernelClockGettime(int clockId, OrbisKernelTimespec* tp);
}

struct thread_usages
{
    OrbisKernelTimespec current_time;	//0x00
    int Thread_Count;					//0x10
    char padding0[0x4];					//0x14
    Proc_Stats Threads[3072];			//0x18
};

int Thread_Count = 0;
float Usage[8] = { 0 };

Proc_Stats Stat_Data[3072];
thread_usages gThread_Data[2];

/* Samplers run several times a second: log a source's first failure and its
 * recovery, never every sample. flips counts state changes (odd = failing);
 * only the first two are logged so a flapping source stays quiet. */
static void log_source_state(bool ok, int &flips, const char *what,
                             int code) {
    const bool failing = (flips & 1) != 0;
    if (ok == !failing)
        return;
    if (++flips > 2)
        return;
    if (!ok)
        LOG_WARN("overlay: %s failed (code %d / 0x%x)", what, code, code);
    else
        LOG_INFO("overlay: %s recovered", what);
}

bool Get_Page_Table_Stats(int vm, int type, int* Used, int* Free, int* Total)
{
    int _Total = 0, _Free = 0;

    static int failing = 0;
    const int rc = ONION_TELEMETRY_CALL(PageTableStats,
                                        (get_page_table_stats(vm, type, &_Total, &_Free)));
    const bool ok = rc == 0;
    log_source_state(ok, failing, "get_page_table_stats", rc);
    if (!ok)
        return false;

    if (Used)
        *Used = (_Total - _Free);

    if (Free)
        *Free = _Free;

    if (Total)
        *Total = _Total;
    return true;
}

static bool valid_monotonic_time(const OrbisKernelTimespec &time) {
    return time.tv_sec >= 0 && time.tv_nsec >= 0 &&
           time.tv_nsec < 1000000000;
}

static bool valid_cpu_time(const OrbisKernelTimeval &time) {
    return time.tv_sec >= 0 && time.tv_usec >= 0 &&
           time.tv_usec < 1000000;
}

static double cpu_time_seconds(const OrbisKernelTimeval &time) {
    return static_cast<double>(time.tv_sec) +
           static_cast<double>(time.tv_usec) / 1000000.0;
}

void calc_usage(unsigned int idle_tid[8], thread_usages* cur,
                thread_usages* prev, float usage_out[8]) {
    if (cur->Thread_Count <= 0 || cur->Thread_Count > 3072 ||
        prev->Thread_Count <= 0 || prev->Thread_Count > 3072 ||
        !valid_monotonic_time(cur->current_time) ||
        !valid_monotonic_time(prev->current_time))
        return;

    const double previous_wall =
        static_cast<double>(prev->current_time.tv_sec) +
        static_cast<double>(prev->current_time.tv_nsec) / 1000000000.0;
    const double current_wall =
        static_cast<double>(cur->current_time.tv_sec) +
        static_cast<double>(cur->current_time.tv_nsec) / 1000000000.0;
    const double wall_delta = current_wall - previous_wall;

    /* Equal, reversed, or invalid clock samples must never update CPU load. */
    if (!std::isfinite(wall_delta) || wall_delta <= 0.0)
        return;

    struct Data_s {
        Proc_Stats* Cur;
        Proc_Stats* Prev;
    } Data[8] = {};

    for (int i = 0; i < cur->Thread_Count; ++i) {
        for (int j = 0; j < 8; ++j) {
            if (idle_tid[j] != 0 && idle_tid[j] == cur->Threads[i].td_tid)
                Data[j].Cur = &cur->Threads[i];
        }
    }

    for (int i = 0; i < prev->Thread_Count; ++i) {
        for (int j = 0; j < 8; ++j) {
            if (idle_tid[j] != 0 && idle_tid[j] == prev->Threads[i].td_tid)
                Data[j].Prev = &prev->Threads[i];
        }
    }

    for (int i = 0; i < 8; ++i) {
        if (!Data[i].Cur || !Data[i].Prev)
            continue;

        const OrbisKernelTimeval &prev_system =
            Data[i].Prev->system_cpu_usage_time;
        const OrbisKernelTimeval &prev_user =
            Data[i].Prev->user_cpu_usage_time;
        const OrbisKernelTimeval &cur_system =
            Data[i].Cur->system_cpu_usage_time;
        const OrbisKernelTimeval &cur_user =
            Data[i].Cur->user_cpu_usage_time;

        if (!valid_cpu_time(prev_system) || !valid_cpu_time(prev_user) ||
            !valid_cpu_time(cur_system) || !valid_cpu_time(cur_user))
            continue;

        const double previous_idle =
            cpu_time_seconds(prev_system) + cpu_time_seconds(prev_user);
        const double current_idle =
            cpu_time_seconds(cur_system) + cpu_time_seconds(cur_user);
        const double idle_delta = current_idle - previous_idle;

        /* A reset or regressing counter invalidates this interval. */
        if (!std::isfinite(idle_delta) || idle_delta < 0.0)
            continue;

        double idle_ratio = idle_delta / wall_delta;
        if (!std::isfinite(idle_ratio))
            continue;
        if (idle_ratio > 1.0)
            idle_ratio = 1.0;
        if (idle_ratio < 0.0)
            idle_ratio = 0.0;

        usage_out[i] = static_cast<float>((1.0 - idle_ratio) * 100.0);
    }
}

namespace {

constexpr int kMaxProcThreads = 3072;
constexpr int kCpuCores = 8;
constexpr int kOverlayInitDelayFrames = 30;
constexpr int kFpsUpdateIntervalFrames = 2;

constexpr int kSceneUpdateIntervalFrames = 4;
/* CPU / GPU / MEM row: about twice a second at 60 Hz. */
constexpr int kSystemUpdateIntervalFrames = 30;

constexpr int kClockIdRealtime = 4;
constexpr int kVmSystem = 1;
constexpr int kPageTableRam = 1;
constexpr int kPageTableVram = 2;

namespace scene = onion::overlay::scene;
static_assert(scene::kSystemMetricCount ==
              onion::kOverlaySystemMetricCount,
              "settings and scene system metric IDs must stay aligned");
static_assert(scene::kSystemCpu == onion::kOverlaySystemMetricCpu &&
              scene::kSystemGpu == onion::kOverlaySystemMetricGpu &&
              scene::kSystemMemory == onion::kOverlaySystemMetricMemory &&
              scene::kSystemFan == onion::kOverlaySystemMetricFan &&
              scene::kSystemIp == onion::kOverlaySystemMetricIp &&
              scene::kSystemCpuClock == onion::kOverlaySystemMetricCpuClock &&
              scene::kSystemGpuClock == onion::kOverlaySystemMetricGpuClock &&
              scene::kSystemSsdTemp == onion::kOverlaySystemMetricSsdTemp &&
              scene::kSystemSocPower == onion::kOverlaySystemMetricSocPower,
              "settings and scene metric IDs must match");

/* Latest numeric readings and the frame-time history the scene draws. */
scene::Readings g_readings;
scene::Histories g_history;
/* Frame spans from the daemon's running frame count: FPS, AVG, LOW, frame
 * times. */
scene::FrameTimeline g_timeline;
/* 20 buckets × 250 ms: a new frame cap must hold ~5 s before it sticks. */
scene::TargetTracker g_fps_target{20};
/* Exact per-bucket rate from the daemon's running frame count. */
scene::FrameCountWindow g_frame_window;
/* Older daemons publish only a smoothed rate: integrate it into a running
 * count of our own so the same timeline serves both. */
uint64_t g_synth_frames_milli = 0;
uint64_t g_synth_ns = 0;
/* The game the FPS stats belong to (title + pid, so a relaunch starts over). */
std::string g_fps_title;
int32_t g_fps_pid = 0;
bool g_fps_tracking = false;
int64_t g_fps_start_s = 0;
int64_t g_fps_last_valid_s = 0;
int64_t g_fps_last_bucket_ms = 0;
/* Daemon publish counter of the last sample folded into the stats; the
 * daemon and this loop both run near 30 Hz, so the same sample can be read
 * twice or skipped, and only a new seq is a new measurement. */
uint32_t g_fps_last_seq = 0;

/* A dropped sample (loading screen, brief stall) keeps the last reading this
 * long before the FPS section collapses to "NO GAME DETECTED". */
constexpr int64_t kFpsGraceSeconds = 2;
/* After this long without frames the session is over; the same title coming
 * back counts as a new session. */
constexpr int64_t kFpsSessionEndSeconds = 10;

bool detail_is(int detail) { return g_settings.overlay_detail == detail; }

/* FPS samples also feed frame-time and history charts, even when the
 * current-FPS number itself is hidden. */
bool wants_fps() { return g_settings.overlay_enabled; }
/* The system row is a master switch; each metric below is independently
 * selectable. Keep the sampler asleep when every system metric is hidden. */
bool wants_system() {
  return g_settings.overlay_enabled && g_settings.overlay_system &&
         !detail_is(onion::kOverlayDetailMinimal) &&
         (g_settings.overlay_cpu || g_settings.overlay_gpu ||
          g_settings.overlay_ram || g_settings.overlay_ip ||
          g_settings.overlay_fan || g_settings.overlay_cpu_clock ||
          g_settings.overlay_gpu_clock || g_settings.overlay_ssd_temp ||
          g_settings.overlay_soc_power);
}

scene::Detail scene_detail() {
  switch (g_settings.overlay_detail) {
  case onion::kOverlayDetailMinimal:
    return scene::Detail::Minimal;
  case onion::kOverlayDetailPanel:
    return scene::Detail::Panel;
  case onion::kOverlayDetailFull:
    return scene::Detail::Full;
  default:
    return scene::Detail::Compact;
  }
}

/* Clock id 4 is CLOCK_MONOTONIC on the PS5 (FreeBSD numbering). */
int64_t monotonic_seconds() {
  OrbisKernelTimespec ts{};
  sceKernelClockGettime(kClockIdRealtime, &ts);
  return ts.tv_sec;
}

int64_t monotonic_ms() {
  OrbisKernelTimespec ts{};
  sceKernelClockGettime(kClockIdRealtime, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint64_t monotonic_ns() {
  OrbisKernelTimespec ts{};
  sceKernelClockGettime(kClockIdRealtime, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

int update_idle_thread_ids_from_records(
    const Proc_Stats *threads, int count,
    unsigned int idle_tid[kCpuCores], bool replace_existing) {
  if (!threads || count <= 0 || count > kMaxProcThreads)
    return 0;

  int found = 0;
  for (int i = 0; i < count; ++i) {
    char thread_name[0x40] = {};
    int core = -1;
    if (sceKernelGetThreadName(threads[i].td_tid, thread_name) != 0 ||
        sscanf(thread_name, "SceIdleCpu%d", &core) != 1 ||
        core < 0 || core >= kCpuCores)
      continue;

    if (replace_existing || idle_tid[core] == 0 ||
        idle_tid[core] != threads[i].td_tid) {
      idle_tid[core] = threads[i].td_tid;
      ++found;
    }
  }
  return found;
}

void refresh_missing_idle_thread_ids(
    unsigned int idle_tid[kCpuCores], const thread_usages &sample) {
  bool missing = false;
  for (int core = 0; core < kCpuCores && !missing; ++core) {
    if (idle_tid[core] == 0) {
      missing = true;
      break;
    }
    bool present = false;
    for (int i = 0; i < sample.Thread_Count; ++i) {
      if (sample.Threads[i].td_tid == idle_tid[core]) {
        present = true;
        break;
      }
    }
    missing = !present;
  }

  if (!missing)
    return;

  const int refreshed = update_idle_thread_ids_from_records(
      sample.Threads, sample.Thread_Count, idle_tid, false);
  if (refreshed > 0)
    LOG_INFO("overlay: refreshed %d idle CPU thread IDs", refreshed);
}

void discover_idle_thread_ids(unsigned int idle_tid[kCpuCores]) {
  int count = kMaxProcThreads;
  const int rc =
      sceKernelGetCpuUsage(reinterpret_cast<Proc_Stats*>(&Stat_Data), &count);
  if (rc || count <= 0 || count > kMaxProcThreads) {
    LOG_WARN("overlay: sceKernelGetCpuUsage failed (rc=0x%x count=%d); CPU "
             "load hidden",
             rc, count);
    return;
  }

  const int found = update_idle_thread_ids_from_records(
      Stat_Data, count, idle_tid, true);
  if (found < kCpuCores)
    LOG_WARN("overlay: found %d of %d idle CPU threads in %d threads", found,
             kCpuCores, count);
}

/* init_overlay_once retries every frame until it succeeds: log each distinct
 * reason once. */
bool init_failed(const char *why) {
  static const char *last = nullptr;
  if (why != last) {
    last = why;
    LOG_WARN("overlay: init deferred: %s", why);
  }
  return false;
}

bool init_overlay_once(unsigned int idle_tid[kCpuCores]) {
  rootWidget = Get_Property<MonoObject*>(pui_img, "Sce.PlayStation.PUI.UI2", "Scene",
                                         Game, "RootWidget");
  if (!rootWidget)
    return init_failed("Game scene has no RootWidget");

  float screen_w = 0.0f;
  float screen_h = 0.0f;
  if (!resolve_root_dimensions(rootWidget, &screen_w, &screen_h))
    return init_failed("RootWidget size unavailable");

  LOG_INFO("overlay: init on %.0f x %.0f canvas", screen_w, screen_h);
  apply_overlay_layout(screen_w, screen_h);
  discover_idle_thread_ids(idle_tid);
  if (!g_settings.overlay_enabled)
    return true;

  /* Widgets are created on the first scene_render(). */
  overlay_scene_remove_all();
  return true;
}

/**
 * Dual-bank CPU sample into the global Usage[]: the first successful
 * GetCpuUsage seeds the other bank, the next one can compute a delta. Bound
 * retries so a stuck/failing probe cannot block the rest of the render
 * tick. False when no delta was available (a core whose idle thread is
 * missing keeps its previous value, see calc_usage).
 */
bool sample_cpu_cores(unsigned int idle_tid[kCpuCores], int& current_bank) {
  constexpr int kMaxAttempts = 4;
  static int failing = 0;
  int last_rc = 0;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    gThread_Data[current_bank].Thread_Count = kMaxProcThreads;
    last_rc = ONION_TELEMETRY_CALL(
        CpuUsage,
        (sceKernelGetCpuUsage(
            reinterpret_cast<Proc_Stats*>(&gThread_Data[current_bank].Threads),
            &gThread_Data[current_bank].Thread_Count)));
    if (last_rc)
      continue;

    /* The kernel reports the number of valid records through Thread_Count.
     * Reject malformed results before this bank can be used by calc_usage(). */
    const int returned_count = gThread_Data[current_bank].Thread_Count;
    if (returned_count <= 0 || returned_count > kMaxProcThreads) {
      last_rc = EOVERFLOW;
      continue;
    }

    Thread_Count = returned_count;
    const int clock_rc = sceKernelClockGettime(
        kClockIdRealtime, &gThread_Data[current_bank].current_time);
    if (clock_rc != 0 ||
        !valid_monotonic_time(gThread_Data[current_bank].current_time)) {
      last_rc = clock_rc != 0 ? clock_rc : EINVAL;
      continue;
    }

    refresh_missing_idle_thread_ids(
        idle_tid, gThread_Data[current_bank]);
    current_bank = !current_bank;

    if (gThread_Data[current_bank].Thread_Count <= 0)
      continue;

    calc_usage(idle_tid, &gThread_Data[!current_bank], &gThread_Data[current_bank],
               Usage);
    log_source_state(true, failing, "CPU telemetry sample", 0);
    return true;
  }
  log_source_state(false, failing, "CPU telemetry sample", last_rc);
  return false;
}

/** Read the daemon's FPS sample into the frame timeline, then derive FPS,
 *  frame time, AVG, LOW and both chart histories from it. */
void sample_fps() {
  scene::Readings &r = g_readings;
  const int64_t now = monotonic_seconds();
  OnionFpsSample sample{};
  if (!wants_fps() || onion_fps_read(&sample) != 0 || !sample.valid ||
      sample.fps < 1.f) {
    if (g_fps_tracking && now - g_fps_last_valid_s > kFpsSessionEndSeconds)
      g_fps_tracking = false;
    if (r.fps_valid && now - g_fps_last_valid_s <= kFpsGraceSeconds)
      return; /* hold the last reading through a short gap */
    r.fps_valid = false;
    r.title_id.clear();
    return;
  }

  const std::string title(sample.title_id,
                          strnlen(sample.title_id, sizeof(sample.title_id)));
  if (!g_fps_tracking || title != g_fps_title || sample.pid != g_fps_pid) {
    /* New game or relaunch: every window, chart and the target restart. */
    LOG_INFO("overlay: FPS session %s pid=%d", title.c_str(),
             static_cast<int>(sample.pid));
    g_fps_title = title;
    g_fps_pid = sample.pid;
    g_fps_tracking = true;
    g_fps_start_s = now;
    g_timeline.reset();
    g_history = scene::Histories{};
    g_fps_target.reset(scene::infer_target(sample.fps));
    g_frame_window.reset();
    g_fps_last_bucket_ms = 0;
    g_synth_ns = 0;
    r.fps = sample.fps;
    r.frame_ms = 1000.0f / sample.fps;
    r.frame_ms_max = -1.0f;
    r.fps_avg = sample.fps;
    r.fps_low = sample.fps;
    r.fps_target = scene::infer_target(sample.fps);
    r.session.reset();
  }
  g_fps_last_valid_s = now;
  r.session_s = static_cast<int>(now - g_fps_start_s);

  /* Daemons that publish the running frame count feed the timeline
   * directly; for older ones, integrate the smoothed rate per new publish. */
  const bool frame_count = sample.ext_layout == ONION_FPS_EXT_FRAMES;
  uint32_t epoch = sample.ext_epoch;
  uint64_t frames_milli = sample.frames_milli;
  uint64_t mono_ns = sample.frames_mono_ns;
  if (!frame_count) {
    if (sample.seq == g_fps_last_seq && g_synth_ns != 0)
      return;
    g_fps_last_seq = sample.seq;
    const uint64_t t = monotonic_ns();
    if (g_synth_ns != 0 && t > g_synth_ns)
      g_synth_frames_milli += static_cast<uint64_t>(
          static_cast<double>(sample.fps) * static_cast<double>(t - g_synth_ns) /
          1e6);
    g_synth_ns = t;
    epoch = 0;
    frames_milli = g_synth_frames_milli;
    mono_ns = t;
  }

  const int added = g_timeline.push(epoch, frames_milli, mono_ns);
  for (int i = g_timeline.size() - added; i < g_timeline.size(); ++i) {
    const scene::FrameTimeline::Span &s = g_timeline.at(i);
    if (s.frames > 0.0f) {
      g_history.frame_ms.push(s.frame_ms);
      g_history.long_ms.push(s.frame_ms);
    }
  }

  const float fps = g_timeline.rate(scene::kFpsWindowNs);
  if (fps > 0.0f) {
    r.fps = fps;
    r.frame_ms = 1000.0f / fps;
  }
  r.frame_ms_max = g_timeline.max_frame_ms(scene::kMaxWindowNs);
  r.fps_valid = r.fps >= 1.0f;
  r.title_id = title;
  /* Spans average ~33 ms of frames, so LOW is not a per-frame 1% low. */
  r.low_per_frame = false;

  const int64_t now_ms = monotonic_ms();
  if (now_ms - g_fps_last_bucket_ms >= scene::kBucketMs) {
    g_fps_last_bucket_ms = now_ms;
    float bucket_fps = sample.fps;
    if (frame_count) {
      const float exact = g_frame_window.take(
          sample.ext_epoch, sample.frames_milli, sample.frames_mono_ns);
      /* No usable window yet: skip the bucket rather than invent one. */
      bucket_fps = exact >= 0.0f ? exact : -1.0f;
    }
    if (bucket_fps >= 0.0f)
      g_history.fps.push(bucket_fps);
    /* Window stats at bucket rate, not every read (sorting). */
    const float avg = g_timeline.rate(scene::kStatsWindowNs);
    const float low = g_timeline.low_fps(scene::kStatsWindowNs);
    if (avg > 0.0f)
      r.fps_avg = avg;
    if (low > 0.0f)
      r.fps_low = low;
    r.fps_target = g_fps_target.update(r.fps_avg);
    if (bucket_fps >= 0.0f)
      r.session.add(bucket_fps, r.fps_target);
  }
}

/**
 * CPU load and temperature, SoC temperature, GPU core power and system
 * memory into g_readings.sys. Each source is independent: one that fails
 * renders as a dash while the others keep updating.
 */
void sample_system(unsigned int idle_tid[kCpuCores], int& current_bank) {
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
  const uint64_t system_sample_start_ns = onion::overlay::perf::now_ns();
#endif
  scene::SystemReadings &s = g_readings.sys;

  /* The first success spans the microseconds between the two seed
   * snapshots, so it is dropped. Two failures in a row blank the column
   * rather than leave a stale number that looks live. */
  static bool cpu_seeded = false;
  static int cpu_failures = 0;
  bool have_idle = false;
  for (int i = 0; i < kCpuCores; ++i)
    have_idle = have_idle || idle_tid[i] != 0;
  if (have_idle && sample_cpu_cores(idle_tid, current_bank)) {
    cpu_failures = 0;
    if (cpu_seeded) {
      float sum = 0.0f;
      for (int i = 0; i < kCpuCores; ++i) {
        s.cpu_core[i] = Usage[i];
        sum += Usage[i];
      }
      s.cpu_pct = sum / static_cast<float>(kCpuCores);
      s.cpu_valid = std::isfinite(s.cpu_pct);
    }
    cpu_seeded = true;
  } else if (++cpu_failures >= 2) {
    s.cpu_valid = false;
  }

  /* code is the rc, or the out-of-range reading when rc was 0. */
  static int cpu_temp_failing = 0;
  static int soc_temp_failing = 0;
  int t = 0;
  int rc = ONION_TELEMETRY_CALL(CpuTemperature,
                                (sceKernelGetCpuTemperature(&t)));
  s.cpu_temp_valid = rc == 0 && t > 0 && t < 150;
  log_source_state(s.cpu_temp_valid, cpu_temp_failing,
                   "sceKernelGetCpuTemperature", rc ? rc : t);
  if (s.cpu_temp_valid)
    s.cpu_temp = t;
  t = 0;
  rc = ONION_TELEMETRY_CALL(SocTemperature,
                            (sceKernelGetSocSensorTemperature(0, &t)));
  s.soc_temp_valid = rc == 0 && t > 0 && t < 150;
  log_source_state(s.soc_temp_valid, soc_temp_failing,
                   "sceKernelGetSocSensorTemperature", rc ? rc : t);
  if (s.soc_temp_valid)
    s.soc_temp = t;

  /* M.2 NVMe temperature is exposed by SoC sensor channel 2. */
  static int ssd_temp_failing = 0;
  t = 0;
  rc = ONION_TELEMETRY_CALL(SsdTemperature,
                            (sceKernelGetSocSensorTemperature(2, &t)));
  s.ssd_temp_valid = rc == 0 && t >= 20 && t <= 130;
  log_source_state(s.ssd_temp_valid, ssd_temp_failing,
                   "sceKernelGetSocSensorTemperature(channel 2)", rc ? rc : t);
  if (s.ssd_temp_valid)
    s.ssd_temp = t;

  /* CPU clock: highest reported core clock, then the kernel frequency API. */
  static int cpu_clock_failing = 0;
  int cpu_mhz = 0;
  int core_clocks[16] = {};
  int cpu_clock_rc = -1;
  if (sceKernelGetCpuCoreClock &&
      (cpu_clock_rc = ONION_TELEMETRY_CALL(
           CpuCoreClock, (sceKernelGetCpuCoreClock(core_clocks)))) == 0) {
    for (int i = 0; i < 16; ++i)
      if (core_clocks[i] > cpu_mhz)
        cpu_mhz = core_clocks[i];
  }
  if (cpu_mhz <= 0 && sceKernelGetCpuFrequency) {
    const long hz = ONION_TELEMETRY_CALL(CpuFrequency,
                                         (sceKernelGetCpuFrequency()));
    if (hz > 0)
      cpu_mhz = static_cast<int>(hz / 1000000L);
  }
  s.cpu_clock_valid = cpu_mhz > 0 && cpu_mhz <= 10000;
  log_source_state(s.cpu_clock_valid, cpu_clock_failing,
                   "CPU clock telemetry", s.cpu_clock_valid ? cpu_mhz : cpu_clock_rc);
  if (s.cpu_clock_valid)
    s.cpu_clock_mhz = cpu_mhz;

  /* GPU GFXCLK is domain 20 in the SoC clock array. */
  static int gpu_clock_failing = 0;
  uint32_t soc_clocks[32] = {};
  int gpu_clock_rc = -1;
  uint32_t gpu_mhz = 0;
  if (sceKernelGetSocClock &&
      (gpu_clock_rc = ONION_TELEMETRY_CALL(
           SocClock, (sceKernelGetSocClock(soc_clocks)))) == 0)
    gpu_mhz = soc_clocks[20];
  s.gpu_clock_valid = gpu_clock_rc == 0 && gpu_mhz > 0 && gpu_mhz <= 10000;
  log_source_state(s.gpu_clock_valid, gpu_clock_failing,
                   "sceKernelGetSocClock(GFXCLK)", s.gpu_clock_valid ? static_cast<int>(gpu_mhz) : gpu_clock_rc);
  if (s.gpu_clock_valid)
    s.gpu_clock_mhz = static_cast<int>(gpu_mhz);

  int used = 0, free_mb = 0, total = 0;
  s.ram_valid = Get_Page_Table_Stats(kVmSystem, kPageTableRam, &used, &free_mb,
                                     &total) &&
                total > 0;
  if (s.ram_valid) {
    s.ram_used_mb = used;
    s.ram_total_mb = total;
  }

  /* VRAM page-table counters are kept independent from system RAM. */
  used = 0;
  free_mb = 0;
  total = 0;
  s.vram_valid =
      Get_Page_Table_Stats(kVmSystem, kPageTableVram, &used, &free_mb, &total) &&
      total > 0 && used >= 0 && used <= total;
  if (s.vram_valid) {
    s.vram_used_mb = used;
    s.vram_total_mb = total;
  }

  /* Console IPv4 is sampled independently from hardware telemetry. */
  if (g_settings.overlay_ip) {
    char ip[ONION_NET_IP_ADDRESS_SIZE] = {};
    s.ip_valid = ONION_TELEMETRY_CALL(
                      IpAddress, (onion_net_get_ip_address(ip, sizeof(ip)))) ==
                  ONION_NET_IP_OK;
    s.ip_address = s.ip_valid ? std::string(ip) : std::string{};
  } else {
    s.ip_valid = false;
    s.ip_address.clear();
  }

  /* Fan duty is an independent optional libkernel_sys symbol. */
  static int fan_failing = 0;
  uint16_t fan_duty = 0;
  uint64_t fan_chassis = 0;
  int fan_rc = -1;
  s.fan_valid = sceKernelGetCurrentFanDuty &&
                (fan_rc = ONION_TELEMETRY_CALL(
                     FanDuty,
                     (sceKernelGetCurrentFanDuty(&fan_duty, &fan_chassis)))) == 0 &&
                fan_duty <= 1024;
  log_source_state(s.fan_valid, fan_failing, "sceKernelGetCurrentFanDuty",
                   fan_rc ? fan_rc : static_cast<int>(fan_duty));
  if (s.fan_valid) {
    s.fan_duty_pct =
        static_cast<int>((static_cast<uint32_t>(fan_duty) * 100u + 512u) / 1024u);
  }

  /* 0x70 bytes of u32: eight rails × (power mW, voltage mV, current mA);
   * rail 0 is the GPU core. Decoding follows ps5-hwinfo, so the raw words
   * are logged once for a check on the console. */
  static bool power_logged = false;
  static int power_failing = 0;
  constexpr int kPowerWords = 0x70 / 4;
  /* The record size is community-sourced; the buffer is oversized so a
   * larger firmware record cannot overrun the shell's stack. */
  uint32_t raw[0x200 / 4] = {};
  s.power_valid = false;
  s.soc_power_valid = false;
  if (sceKernelGetSocPowerConsumption) {
    const int prc = ONION_TELEMETRY_CALL(
        SocPower, (sceKernelGetSocPowerConsumption(raw)));
    log_source_state(prc == 0, power_failing, "sceKernelGetSocPowerConsumption",
                     prc);
    if (prc == 0) {
      if (!power_logged) {
        power_logged = true;
        char line[kPowerWords * 11 + 1];
        size_t n = 0;
        for (int i = 0; i < kPowerWords && n < sizeof(line); ++i)
          n += static_cast<size_t>(snprintf(line + n, sizeof(line) - n, "%s%u",
                                            i ? " " : "", raw[i]));
        LOG_INFO("soc power raw[%d]: %s", kPowerWords, line);
      }
      /* Keep GPU core rail power separate from the sum of all eight SoC rails. */
      s.gpu_power_w = static_cast<float>(raw[0]) / 1000.0f;
      s.power_valid = raw[0] > 0;
      uint64_t total_mw = 0;
      for (int lane = 0; lane < 8; ++lane)
        total_mw += raw[lane * 3];
      s.soc_power_w = static_cast<float>(total_mw) / 1000.0f;
      s.soc_power_valid = total_mw > 0;
    }
  } else if (!power_logged) {
    power_logged = true;
    LOG_INFO("sceKernelGetSocPowerConsumption not resolved; GPU power hidden");
  }
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
  onion::overlay::perf::record_system_sample(
      onion::overlay::perf::now_ns() - system_sample_start_ns);
#endif
}

void scene_render() {
  /*
   * Widget IDs are stable across ordinary value/layout changes, so keep the
   * cache for those. Changes that alter the scene's widget inventory should
   * release the previous inventory once, before building the new scene.
   *
   * This prevents a Full + graph scene from leaving hundreds of pinned,
   * off-screen widgets in the cache when switching to Compact / Minimal.
   * The first scene is already preceded by overlay_scene_remove_all() during
   * initialization.
   */
  struct WidgetInventory {
    int detail;
    bool frame_graph;
    bool brand;
    bool fps_pill;
    bool show_system;
    bool show_fps;
    bool show_cpu;
    bool show_gpu;
    bool show_memory;
    bool show_ip_address;
    bool show_fan_duty;
    bool show_cpu_clock;
    bool show_gpu_clock;
    bool show_ssd_temp;
    bool show_soc_power;
  };
  static WidgetInventory previous{};
  static bool inventory_initialized = false;

  const WidgetInventory current{
      g_settings.overlay_detail,
      g_settings.overlay_frame_graph,
      g_settings.overlay_brand,
      g_settings.overlay_fps_pill,
      g_settings.overlay_system,
      g_settings.overlay_fps,
      g_settings.overlay_cpu,
      g_settings.overlay_gpu,
      g_settings.overlay_ram,
      g_settings.overlay_ip,
      g_settings.overlay_fan,
      g_settings.overlay_cpu_clock,
      g_settings.overlay_gpu_clock,
      g_settings.overlay_ssd_temp,
      g_settings.overlay_soc_power};

  const bool inventory_changed =
      inventory_initialized &&
      (previous.detail != current.detail ||
       previous.frame_graph != current.frame_graph ||
       previous.brand != current.brand ||
       previous.fps_pill != current.fps_pill ||
       previous.show_system != current.show_system ||
       previous.show_fps != current.show_fps ||
       previous.show_cpu != current.show_cpu ||
       previous.show_gpu != current.show_gpu ||
       previous.show_memory != current.show_memory ||
       previous.show_ip_address != current.show_ip_address ||
       previous.show_fan_duty != current.show_fan_duty ||
       previous.show_cpu_clock != current.show_cpu_clock ||
       previous.show_gpu_clock != current.show_gpu_clock ||
       previous.show_ssd_temp != current.show_ssd_temp ||
       previous.show_soc_power != current.show_soc_power);

  if (inventory_changed) {
    overlay_scene_remove_all();
  }
  previous = current;
  inventory_initialized = true;

  scene::Readings &r = g_readings;
  scene::Options o{};
  o.screen_w = g_overlay_layout.screen_w;
  o.screen_h = g_overlay_layout.screen_h;
  o.edge = onion::overlay::bar_edge_from_pos(g_settings.overlay_pos);
  o.align = onion::overlay::bar_align_from_value(g_settings.overlay_align);
  o.detail = scene_detail();
  o.font_pt = onion::overlay_font_pt(g_settings.overlay_font_size);
  o.opacity = g_settings.overlay_opacity;
  o.brand = g_settings.overlay_brand;
  o.fps_pill = g_settings.overlay_fps_pill;
  o.show_system = g_settings.overlay_system;
  o.show_fps = g_settings.overlay_fps;
  o.show_cpu = g_settings.overlay_cpu;
  o.show_gpu = g_settings.overlay_gpu;
  o.show_memory = g_settings.overlay_ram;
  o.show_ip_address = g_settings.overlay_ip;
  o.show_fan_duty = g_settings.overlay_fan;
  o.show_cpu_clock = g_settings.overlay_cpu_clock;
  o.show_gpu_clock = g_settings.overlay_gpu_clock;
  o.show_ssd_temp = g_settings.overlay_ssd_temp;
  o.show_soc_power = g_settings.overlay_soc_power;
  o.system_order = g_settings.overlay_metrics_order;
  o.frame_graph = g_settings.overlay_frame_graph;
  o.graph_opacity = g_settings.overlay_frame_graph_opacity;
  /* kOverlayTheme* values follow the scene::Theme order. */
  o.theme = static_cast<scene::Theme>(g_settings.overlay_theme);
  o.margin = static_cast<float>(g_settings.overlay_margin);
#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)
  const uint64_t build_start_ns = onion::overlay::perf::now_ns();
  scene::Scene built_scene = scene::build_scene(o, r, g_history);
  const uint64_t build_elapsed_ns =
      onion::overlay::perf::now_ns() - build_start_ns;
  onion::overlay::perf::record_scene_build(
      build_elapsed_ns, built_scene.rects.size(), built_scene.texts.size());

  const uint64_t apply_start_ns = onion::overlay::perf::now_ns();
  overlay_scene_apply(built_scene);
  const uint64_t apply_elapsed_ns =
      onion::overlay::perf::now_ns() - apply_start_ns;
  onion::overlay::perf::record_scene_apply(apply_elapsed_ns);

  /* Aggregate for five seconds; never emit a log line per rendered scene. */
  static uint64_t last_report_ns = 0;
  const uint64_t report_now_ns = onion::overlay::perf::now_ns();
  if (last_report_ns == 0)
    last_report_ns = report_now_ns;
  if (report_now_ns - last_report_ns >= 5000000000ull) {
    const onion::overlay::perf::Stats stats =
        onion::overlay::perf::take_interval();
    const uint64_t scenes = stats.scene_count ? stats.scene_count : 1;
    const uint64_t apply_count = stats.scene_count ? stats.scene_count : 1;
    const auto telemetry_avg_ms = [&stats](onion::overlay::perf::TelemetryCall call) {
      const size_t i = static_cast<size_t>(call);
      return stats.telemetry_call_count[i]
                 ? static_cast<double>(stats.telemetry_call_total_ns[i]) /
                       stats.telemetry_call_count[i] / 1000000.0
                 : 0.0;
    };
    const auto telemetry_max_ms = [&stats](onion::overlay::perf::TelemetryCall call) {
      return static_cast<double>(
                 stats.telemetry_call_max_ns[static_cast<size_t>(call)]) /
             1000000.0;
    };
    const auto telemetry_count = [&stats](onion::overlay::perf::TelemetryCall call) {
      return static_cast<unsigned long long>(
          stats.telemetry_call_count[static_cast<size_t>(call)]);
    };
    const double system_avg_ms =
        stats.system_sample_count
            ? static_cast<double>(stats.system_sample_total_ns) /
                  stats.system_sample_count / 1000000.0
            : 0.0;
    LOG_INFO(
        "overlay telemetry/5s: samples=%llu total_avg=%.3fms total_max=%.3fms "
        "cpu_usage=%.3f/%.3fms#%llu cpu_temp=%.3f/%.3fms#%llu "
        "soc_temp=%.3f/%.3fms#%llu ssd_temp=%.3f/%.3fms#%llu "
        "page_stats=%.3f/%.3fms#%llu",
        static_cast<unsigned long long>(stats.system_sample_count),
        system_avg_ms,
        static_cast<double>(stats.system_sample_max_ns) / 1000000.0,
        telemetry_avg_ms(onion::overlay::perf::TelemetryCall::CpuUsage),
        telemetry_max_ms(onion::overlay::perf::TelemetryCall::CpuUsage),
        telemetry_count(onion::overlay::perf::TelemetryCall::CpuUsage),
        telemetry_avg_ms(onion::overlay::perf::TelemetryCall::CpuTemperature),
        telemetry_max_ms(onion::overlay::perf::TelemetryCall::CpuTemperature),
        telemetry_count(onion::overlay::perf::TelemetryCall::CpuTemperature),
        telemetry_avg_ms(onion::overlay::perf::TelemetryCall::SocTemperature),
        telemetry_max_ms(onion::overlay::perf::TelemetryCall::SocTemperature),
        telemetry_count(onion::overlay::perf::TelemetryCall::SocTemperature),
        telemetry_avg_ms(onion::overlay::perf::TelemetryCall::SsdTemperature),
        telemetry_max_ms(onion::overlay::perf::TelemetryCall::SsdTemperature),
        telemetry_count(onion::overlay::perf::TelemetryCall::SsdTemperature),
        telemetry_avg_ms(onion::overlay::perf::TelemetryCall::PageTableStats),
        telemetry_max_ms(onion::overlay::perf::TelemetryCall::PageTableStats),
        telemetry_count(onion::overlay::perf::TelemetryCall::PageTableStats));
    LOG_INFO(
        "overlay telemetry/5s: cpu_coreclk=%.3f/%.3fms#%llu "
        "cpu_freq=%.3f/%.3fms#%llu soc_clk=%.3f/%.3fms#%llu "
        "ip=%.3f/%.3fms#%llu fan=%.3f/%.3fms#%llu "
        "soc_power=%.3f/%.3fms#%llu",
        telemetry_avg_ms(onion::overlay::perf::TelemetryCall::CpuCoreClock),
        telemetry_max_ms(onion::overlay::perf::TelemetryCall::CpuCoreClock),
        telemetry_count(onion::overlay::perf::TelemetryCall::CpuCoreClock),
        telemetry_avg_ms(onion::overlay::perf::TelemetryCall::CpuFrequency),
        telemetry_max_ms(onion::overlay::perf::TelemetryCall::CpuFrequency),
        telemetry_count(onion::overlay::perf::TelemetryCall::CpuFrequency),
        telemetry_avg_ms(onion::overlay::perf::TelemetryCall::SocClock),
        telemetry_max_ms(onion::overlay::perf::TelemetryCall::SocClock),
        telemetry_count(onion::overlay::perf::TelemetryCall::SocClock),
        telemetry_avg_ms(onion::overlay::perf::TelemetryCall::IpAddress),
        telemetry_max_ms(onion::overlay::perf::TelemetryCall::IpAddress),
        telemetry_count(onion::overlay::perf::TelemetryCall::IpAddress),
        telemetry_avg_ms(onion::overlay::perf::TelemetryCall::FanDuty),
        telemetry_max_ms(onion::overlay::perf::TelemetryCall::FanDuty),
        telemetry_count(onion::overlay::perf::TelemetryCall::FanDuty),
        telemetry_avg_ms(onion::overlay::perf::TelemetryCall::SocPower),
        telemetry_max_ms(onion::overlay::perf::TelemetryCall::SocPower),
        telemetry_count(onion::overlay::perf::TelemetryCall::SocPower));
    LOG_INFO(
        "overlay perf/5s: scenes=%llu build_avg=%.3fms build_max=%.3fms "
        "apply_avg=%.3fms apply_max=%.3fms rect_avg=%.1f text_avg=%.1f "
        "created=%llu property_updates=%llu cache=%zu",
        static_cast<unsigned long long>(stats.scene_count),
        static_cast<double>(stats.scene_build_total_ns) / scenes / 1000000.0,
        static_cast<double>(stats.scene_build_max_ns) / 1000000.0,
        static_cast<double>(stats.scene_apply_total_ns) / apply_count / 1000000.0,
        static_cast<double>(stats.scene_apply_max_ns) / 1000000.0,
        static_cast<double>(stats.rect_total) / scenes,
        static_cast<double>(stats.text_total) / scenes,
        static_cast<unsigned long long>(stats.widget_creations),
        static_cast<unsigned long long>(stats.property_updates),
        stats.widget_cache_size);
    last_report_ns = report_now_ns;
  }
#else
  overlay_scene_apply(scene::build_scene(o, r, g_history));
#endif
}

} // namespace

void overlay_scene_reset_state() {
  g_history = scene::Histories{};
  g_timeline.reset();
  g_fps_target.reset();
  g_frame_window.reset();
  g_fps_tracking = false;
  g_fps_last_bucket_ms = 0;
  g_synth_ns = 0;
  g_readings.fps_valid = false;
  g_readings.session.reset();
  g_readings.session_s = 0;
  g_readings.sys = scene::SystemReadings{};
}

void OnRender_Hook(MonoObject* instance) {
  if (!shellui_hooks_are_ready()) {
    if (OnRender_orig)
      OnRender_orig(instance);
    return;
  }

  static bool inited = false;
  static unsigned int idle_thread_id[kCpuCores] = {};
  static int current_bank = 0;
  static int frames_until_fps = 0;
  static int frames_until_scene = 0;
  static int frames_until_system = 0;
  static int frames_until_init = kOverlayInitDelayFrames;

  if (!inited) {
    if (frames_until_init > 0) {
      --frames_until_init;
    } else if (init_overlay_once(idle_thread_id)) {
      inited = true;
    }
    if (!inited) {
      OnRender_orig(instance);
      return;
    }
  }

  /* Config edits apply here, between frames: an mtime check per second. */
  static int64_t last_settings_check_s = 0;
  const int64_t now_s = monotonic_seconds();
  if (now_s != last_settings_check_s) {
    last_settings_check_s = now_s;
    overlay_reload_settings();
  }

  if (frames_until_system <= 0) {
    if (wants_system())
      sample_system(idle_thread_id, current_bank);
    else if (g_readings.sys.cpu_valid || g_readings.sys.ram_valid ||
             g_readings.sys.vram_valid || g_readings.sys.fan_valid ||
             g_readings.sys.power_valid || g_readings.sys.soc_power_valid ||
             g_readings.sys.cpu_clock_valid || g_readings.sys.gpu_clock_valid ||
             g_readings.sys.ssd_temp_valid || g_readings.sys.cpu_temp_valid ||
             g_readings.sys.soc_temp_valid)
      g_readings.sys = scene::SystemReadings{};
    frames_until_system = kSystemUpdateIntervalFrames;
  } else {
    frames_until_system--;
  }
  if (frames_until_fps <= 0) {
    sample_fps();
    frames_until_fps = kFpsUpdateIntervalFrames;
  } else {
    frames_until_fps--;
  }
  if (frames_until_scene <= 0) {
    if (g_settings.overlay_enabled)
      scene_render();
    else
      overlay_scene_remove_all();
    frames_until_scene = kSceneUpdateIntervalFrames;
  } else {
    frames_until_scene--;
  }

  OnRender_orig(instance);
}
