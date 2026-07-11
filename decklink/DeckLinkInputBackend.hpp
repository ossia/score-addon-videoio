#pragma once
#include <decklink/DeckLink.hpp>
#include <decklink/DeckLinkComPtr.hpp>

#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <atomic>
#include <cstdint>

namespace score::gfx::interop
{
struct GpuDirectCaptureSlotRing;
}

namespace Gfx::DeckLink
{

struct DeckLinkInputSettings
{
  int deviceIndex{0};
  BMDDisplayMode displayMode{bmdModeHD1080p5994};
  BMDPixelFormat pixelFormat{bmdFormat8BitYUV};
  /// Physical input connector (bmdVideoConnectionSDI / ...HDMI / ...); 0 keeps
  /// the card's currently-configured connection. Cards with multiple inputs
  /// (e.g. DeckLink Studio 4K) need this to pick SDI vs HDMI capture.
  BMDVideoConnection connection{BMDVideoConnection(0)};
};

/**
 * @brief DeckLink capture backend (score::gfx::DMACaptureBackend).
 *
 * Push model: the SDK delivers frames via IDeckLinkInputCallback on its own
 * thread; the callback copies each frame into the capture strategy's next slot
 * and publishes it in the node's slot ring. Host-staged today (DeckLinkCpuCapture);
 * DVP GPU-direct is a later pass.
 */
class DeckLinkInputBackend final : public score::gfx::DMACaptureBackend
{
public:
  DeckLinkInputBackend(
      DeckLinkInputSettings settings,
      score::gfx::interop::GpuDirectCaptureSlotRing& ring);
  ~DeckLinkInputBackend() override;

  bool open() override;
  int width() const noexcept override { return m_width; }
  int height() const noexcept override { return m_height; }
  uint32_t frameByteSize() const noexcept override { return m_frameByteSize; }
  Video::ImageFormat imageFormat() const override;
  std::unique_ptr<score::gfx::GPUVideoDecoder>
  makeDecoder(Video::VideoMetadata& meta) override;
  std::unique_ptr<score::gfx::interop::GpuDirectCaptureStrategy>
  pickStrategy(QRhi::Implementation impl) override;
  std::unique_ptr<score::gfx::interop::GpuDirectCaptureStrategy>
  makeCpuStrategy() override;
  void setStrategy(score::gfx::interop::GpuDirectCaptureStrategy* s) noexcept override
  {
    m_strategy = s;
  }
  void start() override;
  void stop() override;

private:
  DeckLinkInputSettings m_settings;
  score::gfx::interop::GpuDirectCaptureSlotRing& m_ring;

  ComPtr<IDeckLink> m_device;
  ComPtr<IDeckLinkInput> m_input;
  ComPtr<IDeckLinkInputCallback> m_callback;
  score::gfx::interop::GpuDirectCaptureStrategy* m_strategy{};

  int m_width{};
  int m_height{};
  int m_rowBytes{};
  uint32_t m_frameByteSize{};
  BMDVideoInputFlags m_enableFlags{};
  /// Owned by the callback; lets stop() park its re-arm logic first.
  std::atomic<bool>* m_cbStopping{};
  bool m_started{};
};

} // namespace Gfx::DeckLink
