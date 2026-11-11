/* Optional, compile-time-gated HUD performance counters.
 * This header has no runtime footprint unless PS5_OVERLAY_PERF_DIAGNOSTICS
 * is explicitly enabled for the ShellUI target.
 */
#pragma once

#if defined(PS5_OVERLAY_PERF_DIAGNOSTICS)

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace onion::overlay::perf {

enum class TelemetryCall : size_t {
  CpuUsage,
  CpuTemperature,
  SocTemperature,
  SsdTemperature,
  CpuCoreClock,
  CpuFrequency,
  SocClock,
  PageTableStats,
  IpAddress,
  FanDuty,
  SocPower,
  Count
};

constexpr size_t kTelemetryCallCount =
    static_cast<size_t>(TelemetryCall::Count);

struct Stats {
  uint64_t scene_count = 0;
  uint64_t scene_build_total_ns = 0;
  uint64_t scene_build_max_ns = 0;
  uint64_t scene_apply_total_ns = 0;
  uint64_t scene_apply_max_ns = 0;
  uint64_t rect_total = 0;
  uint64_t text_total = 0;
  uint64_t widget_creations = 0;
  uint64_t property_updates = 0;
  size_t widget_cache_size = 0;
  uint64_t system_sample_count = 0;
  uint64_t system_sample_total_ns = 0;
  uint64_t system_sample_max_ns = 0;
  uint64_t telemetry_call_count[kTelemetryCallCount] = {};
  uint64_t telemetry_call_total_ns[kTelemetryCallCount] = {};
  uint64_t telemetry_call_max_ns[kTelemetryCallCount] = {};
};

inline Stats &state() {
  static Stats value;
  return value;
}

inline uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

inline void record_telemetry_call(TelemetryCall call,
                                  uint64_t elapsed_ns) {
  const size_t index = static_cast<size_t>(call);
  if (index >= kTelemetryCallCount)
    return;
  Stats &s = state();
  ++s.telemetry_call_count[index];
  s.telemetry_call_total_ns[index] += elapsed_ns;
  if (elapsed_ns > s.telemetry_call_max_ns[index])
    s.telemetry_call_max_ns[index] = elapsed_ns;
}

inline void record_system_sample(uint64_t elapsed_ns) {
  Stats &s = state();
  ++s.system_sample_count;
  s.system_sample_total_ns += elapsed_ns;
  if (elapsed_ns > s.system_sample_max_ns)
    s.system_sample_max_ns = elapsed_ns;
}

template <typename Function>
inline auto measure_telemetry_call(TelemetryCall call, Function &&function) {
  const uint64_t start_ns = now_ns();
  auto result = function();
  record_telemetry_call(call, now_ns() - start_ns);
  return result;
}

inline void record_scene_build(uint64_t elapsed_ns, size_t rects,
                               size_t texts) {
  Stats &s = state();
  ++s.scene_count;
  s.scene_build_total_ns += elapsed_ns;
  if (elapsed_ns > s.scene_build_max_ns)
    s.scene_build_max_ns = elapsed_ns;
  s.rect_total += rects;
  s.text_total += texts;
}

inline void record_scene_apply(uint64_t elapsed_ns) {
  Stats &s = state();
  s.scene_apply_total_ns += elapsed_ns;
  if (elapsed_ns > s.scene_apply_max_ns)
    s.scene_apply_max_ns = elapsed_ns;
}

inline void record_widget_created(size_t cache_size) {
  Stats &s = state();
  ++s.widget_creations;
  s.widget_cache_size = cache_size;
}

inline void record_property_update() { ++state().property_updates; }

inline void set_widget_cache_size(size_t cache_size) {
  state().widget_cache_size = cache_size;
}

/* Return the completed interval and reset counters without losing live cache
 * size. Called only by the ShellUI render thread. */
inline Stats take_interval() {
  Stats &s = state();
  const size_t cache_size = s.widget_cache_size;
  const Stats result = s;
  s = Stats{};
  s.widget_cache_size = cache_size;
  return result;
}

} // namespace onion::overlay::perf

#endif // PS5_OVERLAY_PERF_DIAGNOSTICS
