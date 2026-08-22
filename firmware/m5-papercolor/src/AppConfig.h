#pragma once

#include <Arduino.h>

namespace inkloop {

static constexpr char kBuildVersion[] = "0.3.0-beta.1";
static constexpr char kProtocolFirmwareVersion[] = "0.2.0";
static constexpr char kSkuId[] = "m5-papercolor-c151";
// Direct PlatformIO/test-channel flashes do not pass through the browser's
// server-slot patcher, so their safe production fallback must match the
// public Inkloop host where users claim devices.
static constexpr char kDefaultApiUrl[] = "https://inkloop.mess.host/api/devices";
static constexpr char kUnpatchedApiPrefix[] = "INKLOOP_";
static constexpr char kTasksPath[] = "/tasks.json";
static constexpr uint32_t kSyncIntervalMs = 30000;
static constexpr uint32_t kScheduleTickMs = 30000;
static constexpr size_t kMaxFrameBytes = 1500000;

struct FeatureConfig {
  bool myAiEnabled = false;
  bool albumEnabled = true;
  bool experimentalRenderEnabled = false;
  bool deepSleepEnabled = false;
};

}  // namespace inkloop
