#pragma once

/**
 * @file V4L2InputBackend.hpp
 * @brief V4L2 capture as a score DMACaptureBackend, alongside AJA/DeckLink/etc.
 *
 * Same contract as every other vendor: open() negotiates geometry, a capture
 * thread fills the strategy's slot ring, and the renderer decodes the wire
 * bytes with the shared unpacker shaders.
 *
 * Two things differ from the SDI cards. V4L2 buffers are BORROWED -- every
 * DQBUF must be returned with QBUF or the driver starves. And the source can
 * change format mid-stream (a webcam renegotiating, an HDMI-to-V4L2 bridge
 * relocking), which is published through the ring's seqlock exactly as the SDI
 * cards publish a detected wire format.
 *
 * Two rungs:
 *   - GPU-direct: the queue is brought up in `MmapExport` and each buffer's
 *     DMA-BUF fd is imported once into the renderer's sampled texture
 *     (`DmaBufImportCapture`). No pixel is copied. A dequeued buffer is then
 *     owned by the renderer, so the loop requeues it only when the strategy
 *     says the GPU is finished with it.
 *   - Host-staged: `MmapRead`, the loop copies each frame into a ring slot and
 *     requeues immediately.
 *
 * The GPU rung has to bring the session up in `pickStrategy` rather than
 * `start`: the fds it imports do not exist until the queue is allocated, and
 * the rung must be proven at strategy-init time so a refused import degrades
 * before the renderer commits to it. `start()` puts the session back on
 * `MmapRead` when the renderer settled on the CPU strategy instead.
 */

#include <v4l2/V4L2Session.hpp>

#include <Gfx/Graph/DMACaptureInputNode.hpp>
#include <Gfx/Graph/interop/VideoPixelFormat.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace score::gfx::interop
{
struct BorrowedHostImportCapture;
struct VideoCaptureSlotRing;
struct DmaBufImportCapture;
}

namespace Gfx::V4L2
{

struct V4L2InputSettings
{
  std::string device{"/dev/video0"};
  /// 0 keeps whatever the driver is already configured for, which is what a
  /// webcam or a locked capture bridge usually wants.
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t fourcc{};
  std::size_t slotCount{4};
};

class V4L2InputBackend final : public score::gfx::DMACaptureBackend
{
public:
  V4L2InputBackend(
      V4L2InputSettings settings, score::gfx::interop::VideoCaptureSlotRing& ring);
  ~V4L2InputBackend() override;

  bool open() override;
  int width() const noexcept override { return m_width; }
  int height() const noexcept override { return m_height; }
  std::uint32_t frameByteSize() const noexcept override { return m_frameByteSize; }
  Video::ImageFormat imageFormat() const override;
  std::unique_ptr<score::gfx::GPUVideoDecoder>
  makeDecoder(Video::VideoMetadata& meta) override;
  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
  pickStrategy(
      QRhi::Implementation,
      const score::gfx::interop::GpuCapabilities&) override;

  /// MMAP + VK_EXT_external_memory_host rung, for drivers with no EXPBUF.
  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
  pickBorrowedHostImport(QRhi::Implementation);
  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
  makeCpuStrategy() override;
  void setStrategy(score::gfx::interop::VideoCaptureStrategy* s) noexcept override;
  void start() override;
  void stop() override;

private:
  void runLoopStaged();
  void runLoopDmaBuf();
  /// Hand back every slot the renderer has released. Capture thread only:
  /// `Session::requeue` is not concurrent with `dequeue`.
  void requeueReleasedSlots();

  V4L2InputSettings m_settings;
  score::gfx::interop::VideoCaptureSlotRing& m_ring;

  Session m_session;
  score::gfx::interop::VideoCaptureStrategy* m_strategy{};
  /// Set when the engaged rung borrows the driver's mmap'd buffers: the
  /// capture loop must then requeue only what takeReturnedSlots() releases.
  score::gfx::interop::BorrowedHostImportCapture* m_borrowed{};

  /// The GPU-direct strategy handed out by pickStrategy, kept typed so the
  /// capture loop can reach its slot-return channel. Non-owning; null until
  /// pickStrategy succeeds, and only *active* once the renderer settles on it.
  score::gfx::interop::DmaBufImportCapture* m_gpu{};
  bool m_gpuActive{};

  std::thread m_thread;
  std::atomic<bool> m_running{false};

  int m_width{};
  int m_height{};
  std::uint32_t m_frameByteSize{};
  std::uint32_t m_fourcc{};
  bool m_started{};
};

/// Neutral pixel format for a V4L2 fourcc, for the decoder and the settings UI.
/// Unknown for anything the shared unpackers cannot decode (compressed
/// formats especially: MJPG/H264 need a decode stage this path does not have).
score::gfx::interop::VideoPixelFormat
neutralFromV4L2Fourcc(std::uint32_t fourcc) noexcept;

} // namespace Gfx::V4L2
