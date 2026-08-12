#pragma once

/**
 * @file ArgusRuntime.hpp
 * @brief libargus process-wide entry point: availability, devices, modes.
 *
 * libargus is not in-process. `libnvargus.so` is a client that talks to
 * nvargus-daemon over a socket, and the daemon owns the ISP; buffers cross that
 * boundary as dma-buf fds. So "direct Argus" removes GStreamer and a per-frame
 * copy, not the IPC hop -- the hop is inherent, and is equally there today.
 *
 * One CameraProvider per process. Argus refuses a second one, and every session
 * and device handle is scoped to it, so it lives here as a lazily-created
 * singleton rather than being owned by a device that may come and go.
 *
 * Availability is a runtime question, not a compile-time one: the headers may
 * be present on a build host that has no Tegra. Everything degrades to
 * "unavailable" so a non-Tegra build still links and simply never offers the
 * backend.
 */

#include <argus/ArgusSettings.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Gfx::Argus
{

/// One sensor mode as libargus reports it. These numbers are the authority on
/// what the sensor can do -- V4L2 shows the resolutions and none of the rest.
struct SensorModeInfo
{
  std::uint32_t index{};
  std::uint32_t width{};
  std::uint32_t height{};
  /// Derived from the mode's minimum frame duration, which is what bounds fps.
  double maxFrameRate{};
  std::uint64_t minFrameDurationNs{};
  std::uint64_t maxFrameDurationNs{};
  std::uint64_t minExposureNs{};
  std::uint64_t maxExposureNs{};
  float minGain{};
  float maxGain{};
  /// SENSOR_MODE_TYPE_* as a string, for logs and the settings UI.
  std::string type;
};

struct CameraInfo
{
  std::uint32_t index{};
  std::string model;
  std::vector<SensorModeInfo> modes;
};

/// True when built against the Argus headers AND libnvargus is loadable AND a
/// CameraProvider could be created (which needs nvargus-daemon running).
bool argusAvailable() noexcept;

/// Compiled with Argus support at all. Distinguishes "no Tegra here" from
/// "Tegra, but the daemon is down", which are very different bug reports.
bool argusCompiledIn() noexcept;

/// Cameras the provider reports, with their full mode tables. Empty when
/// unavailable. Cached after the first successful call.
const std::vector<CameraInfo>& argusCameras();

/// Re-query the provider, discarding the cache.
void argusRefreshCameras();

/**
 * @brief Resolve `sensorMode`, reproducing nvarguscamerasrc's own algorithm.
 *
 * Reimplemented from the r36.4.4 plugin source rather than guessed, because
 * "best match" is a real search and getting it wrong is invisible: on the
 * sensor on our bench, modes 0 and 4 are both 3552x3556 and differ only in
 * being 60 vs 30 fps.
 *
 * With `requested >= 0` the mode is used as-is (bounds-checked). With -1: among
 * the modes at least as large as the requested geometry whose minimum frame
 * duration can satisfy the requested rate, prefer an exact resolution match,
 * otherwise take the smallest qualifying resolution.
 *
 * @returns the mode index, or -1 when nothing qualifies.
 */
std::int32_t resolveSensorMode(
    const CameraInfo& cam, std::int32_t requested, std::uint32_t reqWidth,
    std::uint32_t reqHeight, double reqRate) noexcept;

}
