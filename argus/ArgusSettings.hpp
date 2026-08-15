#pragma once

/**
 * @file ArgusSettings.hpp
 * @brief Everything nvarguscamerasrc exposes, as plain data.
 *
 * Feature parity with the GStreamer element is the requirement, so this mirrors
 * its property set one-for-one (read off the r36.4.4 plugin, not the docs).
 * Keeping it a dumb struct means the settings widget, the JSON serialiser and
 * the session all agree on one definition, and a property nobody has wired yet
 * is visible as an unused field rather than missing entirely.
 *
 * Ranges are the sensor's, not ours: libargus rejects a value outside what the
 * active sensor mode reports, so the session clamps against ISensorMode at
 * apply time rather than trusting anything stored here.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace Gfx::Argus
{

/// Mirrors Argus::AE_ANTIBANDING_MODE_*.
enum class AeAntibanding : std::uint8_t
{
  Off = 0,
  Auto = 1,
  Hz50 = 2,
  Hz60 = 3
};

/// Mirrors Argus::AWB_MODE_*.
enum class AwbMode : std::uint8_t
{
  Off = 0,
  Auto = 1,
  Incandescent = 2,
  Fluorescent = 3,
  WarmFluorescent = 4,
  Daylight = 5,
  CloudyDaylight = 6,
  Twilight = 7,
  Shade = 8,
  Manual = 9
};

/// Mirrors Argus::DENOISE_MODE_* / EDGE_ENHANCE_MODE_*: both enums have the
/// same three values and the plugin exposes them identically.
enum class Quality : std::uint8_t
{
  Off = 0,
  Fast = 1,
  HighQuality = 2
};

/// A closed range. `set == false` means "leave the sensor default alone",
/// which is what every unset gst property does.
struct Range
{
  bool set{false};
  double min{};
  double max{};
};

/// Region of interest plus its weight, for aeregion.
struct AeRegion
{
  bool set{false};
  float left{}, top{}, right{}, bottom{};
  float weight{1.f};
};

struct ArgusSettings
{
  /// Index into ICameraProvider::getCameraDevices().
  std::uint32_t sensorId{0};

  /// Sensors to drive from ONE synchronised capture session, for a rig whose
  /// eyes must expose together. Empty means "just sensorId".
  ///
  /// More than one entry is what buys hardware synchronisation: a single
  /// CaptureSession means one AE/AWB loop, one Request and one repeat clock for
  /// all of them. Two sessions would give two of each, and nothing downstream
  /// can reconstruct a synchronisation the capture never had.
  std::vector<std::uint32_t> sensorIds{};

  /// Name of the rig these sensors make up, empty for a lone camera. Members
  /// find each other by it rather than by pointer: they are built independently
  /// from separate graph nodes, in whatever order the document instantiates
  /// them.
  std::string syncRig{};

  /// This backend's position in `sensorIds`, which is also its member index in
  /// the sync group and the stream index within a shared session. Not a
  /// user-facing setting -- it is the child's position in the device tree.
  std::size_t syncMember{0};

  /// -1 selects the smallest mode that satisfies the requested geometry and
  /// rate, matching the plugin's own algorithm. Anything else is used verbatim.
  std::int32_t sensorMode{-1};

  /// Requested output geometry and rate. On this sensor the difference between
  /// two same-resolution modes is 60 vs 30 fps, so the rate is part of mode
  /// selection, not a post-hoc setting.
  std::uint32_t width{0};
  std::uint32_t height{0};
  double frameRate{0.0};

  Range exposureTimeNs{};   ///< exposuretimerange
  Range gain{};             ///< gainrange
  Range ispDigitalGain{};   ///< ispdigitalgainrange

  float exposureCompensation{0.f}; ///< -2 .. 2
  AeAntibanding aeAntibanding{AeAntibanding::Auto};
  bool aeLock{false};
  bool awbLock{false};
  AwbMode awbMode{AwbMode::Auto};
  AeRegion aeRegion{};

  float saturation{1.f}; ///< 0 .. 2; applying it also enables saturation
  bool saturationSet{false};

  Quality denoiseMode{Quality::Fast};
  float denoiseStrength{-1.f}; ///< -1 == leave to the ISP

  Quality edgeEnhanceMode{Quality::Fast};
  float edgeEnhanceStrength{-1.f};

  /// acquireBuffer timeout. The plugin's default is 5 s; a stalled sensor
  /// should surface as an error rather than a permanent block.
  std::uint64_t acquireTimeoutNs{5'000'000'000ull};

  /// Depth of the buffer pool we hand libargus.
  ///
  /// This bounds THROUGHPUT, not just memory: the borrowed contract keeps a
  /// slot from the ISP until the renderer has finished with it, so too shallow
  /// a pool stalls the sensor. The ladder's correctness minimum is
  /// FramesInFlight + 1 on top of the device's own queue, which 6 satisfies --
  /// and 6 measured 21 fps against a 60 fps mode on a 3552x3556 frame, while
  /// 10 reached the full 57 and 16 gained nothing further. Correctness minimum
  /// and throughput requirement are not the same number.
  std::size_t bufferCount{10};

  /// Log the resolved sensor mode and every clamped setting. On by default:
  /// a silently wrong mode pick reads as a permanent performance bug.
  bool verbose{true};
};

}
