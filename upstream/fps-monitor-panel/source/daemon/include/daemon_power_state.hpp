/* Copyright (C) 2025 OnionHEN / LightningMods */
#pragma once

#include <cstdint>

enum class DaemonPowerState : uint8_t {
  Unknown,
  Working,
  MainOnStandby,
  SuspendOnGoing,
  ShutdownOnGoing,
};

/** Read the current SceSystemStateMgr state from its kernel event flag. */
DaemonPowerState daemon_power_state_get();

/** True for a sampled state while the console is entering/being suspended. */
bool daemon_power_state_is_sleeping(DaemonPowerState state);

/** True while the system state manager reports shutdown in progress. */
bool daemon_power_state_is_shutting_down(DaemonPowerState state);

/** True while the console is entering or already in standby suspend. */
bool daemon_power_state_is_sleeping();
