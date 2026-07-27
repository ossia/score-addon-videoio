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
 * DQBUF must be returned with QBUF or the driver starves -- so the loop
 * dequeues, copies into the strategy slot, and requeues immediately rather
 * than holding the buffer for the renderer. And the source can change format
 * mid-stream (a webcam renegotiating, an HDMI-to-V4L2 bridge relocking), which
 * is published through the ring's seqlock exactly as the SDI cards publish a
 * detected wire format.
 *
 * pickStrategy() returns nothing today: the zero-copy path needs a
 * VideoCaptureStrategy that imports the V4L2 DMA-BUF into the renderer's
 * texture, which is a separate piece of work. The session's MmapExport and
 * DmaBufImport modes are implemented and validated (V4L2CaptureTest), so that
 * strategy has a working foundation to sit on -- but until it exists this
 * backend is host-staged, like Magewell.
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
struct VideoCaptureSlotRing;
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
  pickStrategy(QRhi::Implementation) override;
  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
  makeCpuStrategy() override;
  void setStrategy(score::gfx::interop::VideoCaptureStrategy* s) noexcept override;
  void start() override;
  void stop() override;

private:
  void runLoop();

  V4L2InputSettings m_settings;
  score::gfx::interop::VideoCaptureSlotRing& m_ring;

  Session m_session;
  score::gfx::interop::VideoCaptureStrategy* m_strategy{};

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
