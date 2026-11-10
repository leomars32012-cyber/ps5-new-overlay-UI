/* Copyright (C) 2025 OnionHEN / LightningMods
 *
 * Serialized Toolbox ELF inject into one SceShellUI PID.
 * When to inject (immediate vs rest) lives in daemon_inject.cpp.
 */
#pragma once

#include <mutex>
#include <sys/types.h>
#include <unistd.h>

#include <onion/proc_query.h>
#include <onion/ready.h>

namespace onion {

enum class ToolboxInjectionResult {
  AlreadyReady,
  Injected,
  TargetNotFound,
  InjectFailed,
  ReadyTimeout,
  TargetChanged,
  TargetDied, // target exited before publishing the marker (crashed)
};

struct ToolboxInjectionOutcome {
  ToolboxInjectionResult result;
  pid_t pid;

  bool ready() const noexcept {
    return result == ToolboxInjectionResult::AlreadyReady ||
           result == ToolboxInjectionResult::Injected;
  }
};

class ToolboxInject final {
public:
  /* marker: ready name the injected ELF publishes with its PID. */
  explicit ToolboxInject(const char *marker = ONION_READY_TOOLBOX) noexcept
      : marker_(marker) {}

  bool is_ready_for(pid_t pid) const noexcept {
    return onion_ready_matches_pid(marker_, pid);
  }

  template <typename ResolvePid, typename InjectFn>
  ToolboxInjectionOutcome inject(ResolvePid resolve_pid, InjectFn inject_fn,
                                 int timeout_ms, int poll_ms,
                                 pid_t expected_pid = 0) {
    std::lock_guard<std::mutex> lock(mutex_);

    const pid_t resolved_before = resolve_pid();
    if (expected_pid > 1) {
      /* An event PID is authoritative; reject only an observed replacement. */
      if (resolved_before > 1 && resolved_before != expected_pid) {
        return {ToolboxInjectionResult::TargetChanged, expected_pid};
      }
    }
    const pid_t pid = expected_pid > 1 ? expected_pid : resolved_before;
    if (pid <= 0) {
      return {ToolboxInjectionResult::TargetNotFound, pid};
    }
    if (is_ready_for(pid)) {
      return {ToolboxInjectionResult::AlreadyReady, pid};
    }

    /* Stale marker is another PID; ready file is not the rest-resume signal. */
    onion_ready_clear(marker_);
    if (!inject_fn(pid)) {
      onion_ready_clear(marker_);
      return {ToolboxInjectionResult::InjectFailed, pid};
    }
    /* Like onion_ready_wait_pid, but stop as soon as the target dies. */
    for (int waited = 0; !is_ready_for(pid); waited += poll_ms) {
      if (!onion_proc_is_alive(pid)) {
        onion_ready_clear(marker_);
        return {ToolboxInjectionResult::TargetDied, pid};
      }
      if (waited >= timeout_ms) {
        onion_ready_clear(marker_);
        return {ToolboxInjectionResult::ReadyTimeout, pid};
      }
      usleep(static_cast<useconds_t>(poll_ms) * 1000);
    }

    const pid_t resolved_after = resolve_pid();
    if (resolved_after > 1 && resolved_after != pid) {
      onion_ready_clear(marker_);
      return {ToolboxInjectionResult::TargetChanged, pid};
    }
    return {ToolboxInjectionResult::Injected, pid};
  }

private:
  const char *marker_;
  mutable std::mutex mutex_;
};

} // namespace onion
