#pragma once
#include <bluefish/Bluefish.hpp>
#include <bluefish/BluefishSettings.hpp>

#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

namespace score::gfx::interop
{
struct GpuDirectCaptureSlotRing;
}

namespace Gfx::Bluefish
{

/**
 * @brief Bluefish444 capture backend (score::gfx::DMACaptureBackend).
 *
 * Host-staged v1 using the AutoCapture engine with SDK-owned (internal) buffers:
 * a reception thread runs the AutoCapture loop
 * (bfcAutoCaptureGetFilledBuffer -> memcpy info.pBufferVideo into the capture
 * strategy's next slot -> bfcAutoCaptureReturnBuffer) and publishes the slot in
 * the node's ring. The render thread uploads the slot via BluefishCpuCapture.
 * Because the SDK owns the DMA buffers, the registrar is a no-op and there is no
 * GPU-direct path here (BlueGpuDirect is a later pass; pickStrategy returns {}).
 */
class BluefishInputBackend final : public score::gfx::DMACaptureBackend
{
public:
  BluefishInputBackend(
      BluefishInputSettings settings,
      score::gfx::interop::GpuDirectCaptureSlotRing& ring);
  ~BluefishInputBackend() override;

  bool open() override;
  int width() const noexcept override { return m_width; }
  int height() const noexcept override { return m_height; }
  uint32_t frameByteSize() const noexcept override { return m_frameByteSize; }
  Video::ImageFormat imageFormat() const override;
  std::unique_ptr<score::gfx::GPUVideoDecoder>
  makeDecoder(Video::VideoMetadata& meta) override;
  std::unique_ptr<score::gfx::interop::GpuDirectCaptureStrategy>
  pickStrategy(QRhi::Implementation) override;
  std::unique_ptr<score::gfx::interop::GpuDirectCaptureStrategy>
  makeCpuStrategy() override;
  void setStrategy(score::gfx::interop::GpuDirectCaptureStrategy* s) noexcept override;
  void start() override;
  void stop() override;

private:
  void runLoop(); ///< AutoCapture reception loop (Get/Return filled buffer).
  /// Copy (or 2SI->SQD convert) a filled SDK buffer into a strategy slot.
  bool ingestBuffer(blue_auto_buffer_info& info, void* dst);

  BluefishInputSettings m_settings;
  score::gfx::interop::GpuDirectCaptureSlotRing& m_ring;

  BLUEVELVETC_HANDLE m_bvc{nullptr};
  /// Lazily-created conversion engine for UHD 2SI -> square-division rasters
  /// (blue_auto_buffer_info::RequiredConversionType).
  BFC_CONVERSION_HANDLE m_conv{nullptr};
  score::gfx::interop::GpuDirectCaptureStrategy* m_strategy{};

  BLUE_U32 m_videoModeExt{VID_FMT_EXT_INVALID};
  std::thread m_thread;
  std::atomic<bool> m_running{false};

  int m_width{};
  int m_height{};
  uint32_t m_frameByteSize{};
  bool m_captureStarted{}; ///< bfcVideoCaptureStart succeeded
  bool m_buffersCreated{}; ///< bfcAutoCaptureCreateInternalBuffers succeeded
  bool m_started{};
};

} // namespace Gfx::Bluefish
