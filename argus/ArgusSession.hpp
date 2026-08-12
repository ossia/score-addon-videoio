#pragma once

/**
 * @file ArgusSession.hpp
 * @brief One camera: capture session, buffer pool, and the acquire/release loop.
 *
 * The pool is ours, not libargus's. Argus can be handed application buffers
 * only as EGLImages -- BUFFER_TYPE_EGL_IMAGE is the only BufferType it defines
 * -- so each slot is an NvBufSurface, mapped to an EGLImage, registered with a
 * BufferOutputStream. The ISP then writes straight into memory we already own
 * and can hand to the GPU as a dma-buf fd.
 *
 * That is the whole point of using STREAM_TYPE_BUFFER rather than the
 * STREAM_TYPE_EGL path nvarguscamerasrc uses. The GStreamer element consumes an
 * EGLStream through a FrameConsumer and then calls copyToNvBuffer once per
 * frame (gstnvarguscamerasrc.cpp:689) -- a full-frame copy it cannot avoid,
 * before nvvidconv copies again. Here there is no such copy.
 *
 * BORROWED-BUFFER LIFETIME
 * ------------------------
 * A slot acquired from Argus must not be released back until the GPU has
 * finished sampling it, which is exactly the contract the capture ladder's
 * borrowed rungs already arbitrate. So the loop is:
 *
 *   acquireBuffer()            -- Argus hands back a filled Buffer
 *   getClientData()            -- recover which slot it is
 *   onFrame(slot)              -- publish to the renderer
 *   drain returned slots       -- releaseBuffer() only those the renderer freed
 *
 * Releasing a slot the renderer still holds corrupts the frame on screen;
 * never releasing starves the ISP. Both failure modes are silent, which is why
 * the release is driven by the ladder's returned-slot bitmask and not by a
 * timer or a fixed depth.
 */

#include <argus/ArgusRuntime.hpp>
#include <argus/ArgusSettings.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace Gfx::Argus
{

/// One pool slot as the GPU side needs to see it.
/// Upper bound on sensors in one synchronised session. Argus's dual-sensor
/// support is the documented case; the array bounds here are what keep a
/// mis-configured settings object from indexing off the end.
inline constexpr std::size_t kMaxSyncSensors = 4;

struct ArgusSlot
{
  int dmabufFd{-1};
  std::uint32_t offset[3]{};
  std::uint32_t pitch[3]{};
  std::uint32_t planeCount{1};
  std::uint32_t totalBytes{};
  /// Host mapping of plane 0, when mapped. Kept for callers that want a single
  /// pointer, but see planeHost: the planes are mapped separately and are not
  /// guaranteed contiguous, so this alone does not describe the frame.
  void* host{};
  /// Per-plane host mappings, valid after mapHost().
  void* planeHost[3]{};
};

class ArgusSession
{
public:
  ArgusSession();
  ~ArgusSession();
  ArgusSession(const ArgusSession&) = delete;
  ArgusSession& operator=(const ArgusSession&) = delete;

  /// Opens the camera, resolves the sensor mode, allocates and registers the
  /// buffer pool, and builds the repeating request. False on any failure, with
  /// the reason logged -- the caller then declines the backend rather than
  /// running a half-configured session.
  bool open(const ArgusSettings& settings);
  void close();

  bool isOpen() const noexcept;

  std::uint32_t width() const noexcept;
  std::uint32_t height() const noexcept;
  /// Total bytes of one frame across all planes, which is what the capture
  /// strategies size their imports from.
  std::uint32_t frameByteSize() const noexcept;
  /// The mode actually selected, after resolveSensorMode and any clamping.
  std::int32_t resolvedSensorMode() const noexcept;
  double resolvedFrameRate() const noexcept;

  /// Number of sensors in this session: 1 unless it is a synchronised rig.
  std::size_t streamCount() const noexcept;

  /// Slots of sensor `stream`. slots() is slots(0).
  const std::vector<ArgusSlot>& slots(std::size_t stream) const noexcept;
  const std::vector<ArgusSlot>& slots() const noexcept;

  /// Map every slot into host memory. Only needed by the CPU rung; the
  /// zero-copy rungs never touch the mapping, and mapping unnecessarily costs
  /// a coherency flush per frame on Tegra.
  bool mapHost();

  /// Starts the repeating capture and the acquire loop.
  ///
  /// @p onFrame is called on the capture thread with the slot index that just
  ///    filled; it must publish and return promptly.
  /// @p takeReturned is polled each iteration for the bitmask of slots the
  ///    renderer has finished with; those are released back to Argus.
  /// Start the capture. `onFrames` is called once per capture with the slot
  /// each stream filled and its start-of-frame stamp on the Tegra TSC (0 when
  /// the timestamp extension is unavailable). One entry today; a multi-sensor
  /// session reports the whole set in one call, which is what lets the renderer
  /// bind frames belonging to the same capture.
  bool start(
      std::function<void(const std::size_t*, const std::uint64_t*, std::size_t)>
          onFrames,
      std::function<std::uint32_t(std::size_t)> takeReturned);
  void stop();

  /// Frames Argus has handed us. The device's own cadence, independent of how
  /// fast the renderer consumes -- a harness reporting only its consumption
  /// rate cannot tell a stalled sensor from a slow renderer.
  std::uint64_t capturedFrames() const noexcept;

  /// Sensor-start-of-frame to frame-published, in nanoseconds.
  ///
  /// Deliberately the same span nvarguscamerasrc's `show-latency` reports
  /// ("capture latency between start of frame and GstBuffer push"), so the two
  /// numbers can be compared directly rather than through two different
  /// definitions of "latency".
  ///
  /// `unusable` counts frames whose sensor timestamp did not land in a
  /// plausible range relative to CLOCK_MONOTONIC. Argus is documented to stamp
  /// on the monotonic clock, but a mismatch there would otherwise produce a
  /// confident and completely wrong figure, so those frames are excluded and
  /// counted instead.
  struct LatencyStats
  {
    std::uint64_t count{};
    std::uint64_t unusable{};
    std::uint64_t minNs{};
    std::uint64_t maxNs{};
    double meanNs{};
  };
  LatencyStats latency() const noexcept;
  void resetLatency() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

}
