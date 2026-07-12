#pragma once
#include <bluefish/Bluefish.hpp>
#include <bluefish/BluefishSettings.hpp>

#include <Gfx/Graph/DirectVideoOutputBackend.hpp>

#include <QString>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace Gfx::Bluefish
{

/**
 * @brief Bluefish444 SDI playout backend (score::gfx::DirectVideoOutputBackend).
 *
 * Host-staged v1 using the FrameStore engine + application host buffers, exactly
 * as the SDK Playback sample: 4 page-aligned bfAlloc golden buffers are cycled.
 * CpuStagedVideoOutput renders + reads back the wire-format frame into its ring, then
 * pacingHooks pace + submit:
 *   - waitForTick(): bfcWaitVideoOutputSync(UPD_FMT_FRAME) blocks to the output
 *     interrupt (genlock pacing).
 *   - submit(ptr):  memcpy the ring frame into the current bfAlloc buffer, then
 *     bfcDmaWriteToCardAsync (blocking, no SyncInfo) + bfcRenderBufferUpdate, and
 *     advance the 4-slot cursor.
 *
 * GPU-direct (BlueGpuDirect) is deliberately out of scope here
 * (gpuDirectCandidates returns {}); the node stays on the host-staged path.
 */
class BluefishOutputBackend final : public score::gfx::DirectVideoOutputBackend
{
public:
  explicit BluefishOutputBackend(BluefishOutputSettings settings);
  ~BluefishOutputBackend() override;

  bool open(score::gfx::GraphicsApi api) override;
  void close() override;

  int width() const noexcept override { return m_width; }
  int height() const noexcept override { return m_height; }
  double frameRate() const noexcept override { return m_frameRate; }
  bool isOpen() const noexcept override { return m_open; }
  uint32_t frameByteSize() const noexcept override { return m_frameByteSize; }
  int visibleRows() const noexcept override { return m_height; }
  score::gfx::interop::VideoPixelFormat wireFormat() const noexcept override;
  bool prefersFloatRender() const noexcept override;
  QString colorConversion() const override;
  std::vector<score::gfx::interop::HostStagedPlane> planes() const override;
  score::gfx::interop::VendorDmaRegistrar registrar() override;
  std::vector<std::function<std::unique_ptr<score::gfx::interop::VideoOutputStrategy>()>>
  gpuDirectCandidates(QRhi* rhi, score::gfx::GraphicsApi api) override;
  score::gfx::interop::PacedFramePump::Hooks pacingHooks() override;

private:
  bool waitForTick();
  bool submitFrame(void* framePtr);

  BluefishOutputSettings m_settings;

  BLUEVELVETC_HANDLE m_bvc{nullptr};

  static constexpr std::size_t kBufferCount = 4;
  std::array<void*, kBufferCount> m_buffers{}; ///< bfAlloc golden host buffers
  std::size_t m_bufId{0};

  int m_width{};
  int m_height{};
  int m_rowBytes{};
  uint32_t m_frameByteSize{}; ///< visible bytes (rowBytes * height)
  uint32_t m_goldenBytes{};   ///< bfAlloc buffer size (page-padded, DMA size)
  double m_frameRate{60.0};
  bool m_open{false};
};

} // namespace Gfx::Bluefish
