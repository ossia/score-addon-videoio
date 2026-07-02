#pragma once
#include <Gfx/Graph/interop/CaptureStrategyCommon.hpp>
#include <Gfx/Graph/interop/GpuDirectCaptureStrategy.hpp>

#include <QtGui/private/qrhi_p.h>

#include <array>
#include <cstdint>
#include <vector>

namespace Gfx::Deltacast
{

/// Host-staged capture: the VHD reception loop copies each arrived frame into one
/// of these slots; the render thread uploads the slot into the decoder's input
/// texture (portable QRhi path; works on every backend). The universal fallback
/// when no GPU-direct path is available (RDMA is a later pass).
struct DeltacastCpuCapture final : score::gfx::interop::GpuDirectCaptureStrategy
{
  score::gfx::interop::GpuDirectCaptureStrategyConfig cfg{};
  static constexpr std::size_t kSlotCount = 3;
  std::array<std::vector<std::uint8_t>, kSlotCount> m_slots;
  score::gfx::interop::CaptureSlotPublisher m_publisher;

  const char* name() const noexcept override { return "Deltacast-CPU"; }

  bool init(const score::gfx::interop::GpuDirectCaptureStrategyConfig& c) override
  {
    cfg = c;
    if(!cfg.rhi || !cfg.outputTexture || cfg.frameByteSize == 0)
      return false;
    for(auto& s : m_slots)
      s.assign(cfg.frameByteSize, 0);
    return true;
  }

  void release() override
  {
    for(auto& s : m_slots)
      s.clear();
    m_publisher.reset();
  }

  std::size_t slotCount() const noexcept override { return kSlotCount; }
  void* slotBuffer(std::size_t i) const noexcept override
  {
    return (i < kSlotCount) ? const_cast<std::uint8_t*>(m_slots[i].data())
                            : nullptr;
  }
  bool ingestFrame(std::size_t i) override
  {
    if(i >= kSlotCount)
      return false;
    m_publisher.publish(i);
    return true;
  }
  QRhiTexture* outputTexture() const noexcept override { return cfg.outputTexture; }

  void acquireForRender() override { }
  void acquireForRender(QRhiResourceUpdateBatch& res) override
  {
    const int slot = m_publisher.consume();
    if(slot < 0 || static_cast<std::size_t>(slot) >= kSlotCount)
      return;
    QRhiTextureSubresourceUploadDescription sub(
        m_slots[static_cast<std::size_t>(slot)].data(), cfg.frameByteSize);
    // Source stride = the card raster's true pitch. For V210 the VHD raster
    // is 48-pixel-group padded (((w+47)/48)*128), which exceeds the decoder
    // texture's texWidth*4 bytes whenever w % 48 != 0 (e.g. 720p) — using the
    // texture width as the stride sheared every such frame diagonally.
    sub.setDataStride(
        cfg.height > 0 ? cfg.frameByteSize / static_cast<quint32>(cfg.height)
                       : 0);
    res.uploadTexture(
        cfg.outputTexture,
        QRhiTextureUploadDescription{QRhiTextureUploadEntry{0, 0, sub}});
  }
  void releaseAfterRender() override { }
};

} // namespace Gfx::Deltacast
