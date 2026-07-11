#pragma once
#include <AJA/Tier3Common.hpp>
#include <Gfx/Graph/interop/GpuDirectOutput.hpp>
#include <Gfx/Graph/interop/GpuDirectStrategy.hpp>

#include <ntv2card.h>

#include <QDebug>

namespace Gfx::AJA
{

/**
 * @brief OpenGL tier-3 RDMA output for AJA. Thin shim over
 *        `score::gfx::interop::GpuDirectOutput`.
 */
struct RdmaInteropGLTier3 final : score::gfx::interop::GpuDirectStrategy
{
  RdmaInteropGLTier3(CNTV2Card* card, NTV2FrameBufferFormat fmt) noexcept
      : m_card{card}, m_targetFormat{fmt} {}

  CNTV2Card* m_card{};
  NTV2FrameBufferFormat m_targetFormat{};
  score::gfx::interop::GpuDirectOutput m_output;

  const char* name() const noexcept override { return "RDMA-GL/T3"; }

  static bool isSupported(QRhi* rhi, NTV2FrameBufferFormat fmt, int width)
  {
    return rhi && rhi->isFeatureSupported(QRhi::Compute)
           && tier3SupportsFormat(fmt, width);
  }

  bool init(const score::gfx::interop::GpuDirectStrategyConfig& c) override
  {
    if(!isSupported(c.rhi, m_targetFormat, c.width) || !c.state)
      return false;

    score::gfx::interop::GpuDirectOutputConfig oc{};
    oc.rhi = c.rhi;
    oc.state = c.state;
    oc.sourceTexture = c.sourceTexture;
    oc.width = c.width;
    oc.height = c.height;
    oc.frameByteSize = c.frameByteSize;
    oc.slotCount = 2;
    oc.debugName = "AJA-RDMA-GL-Storage";
    oc.encoderFactory = [fmt = m_targetFormat] {
      return makeTier3Encoder(fmt);
    };
    oc.colorConversion = score::gfx::colorMatrixOut(
        AVCOL_SPC_BT709, AVCOL_TRC_BT709, AVCOL_RANGE_MPEG, AVCOL_PRI_BT709);
    oc.registrar.registerSlot
        = [card = m_card](void* gpuPtr, std::uint32_t size) {
            return card->DMABufferLock(
                reinterpret_cast<ULWord*>(gpuPtr), size,
                /*inMap=*/true, /*inRDMA=*/true);
          };
    oc.registrar.releaseSlot
        = [card = m_card](void* gpuPtr, std::uint32_t size) {
            card->DMABufferUnlock(reinterpret_cast<ULWord*>(gpuPtr), size);
          };
    // Playout-direction probe: the card must DMA-*read* from the pinned GPU
    // buffer (H2C). On platforms where the AJA card and GPU sit on different
    // PCIe host bridges, this non-posted read is blocked even though the pin
    // and the capture-direction write both succeed — so probe it once with a
    // real transfer into a scratch framestore before committing. Runs before
    // AutoCirculateStart, so writing a framestore here is harmless.
    oc.registrar.verifyTransfer
        = [card = m_card](void* gpuPtr, std::uint32_t size) {
            return card->DMAWriteFrame(
                /*frameNumber=*/0, reinterpret_cast<ULWord*>(gpuPtr), size);
          };

    if(!m_output.init(oc))
    {
      qWarning() << "AJA RDMA(GL/T3): GpuDirectOutput init failed";
      return false;
    }
    return true;
  }

  void release() override { m_output.release(); }
  void encodeFrame(QRhiCommandBuffer& cb) override { m_output.encodeFrame(cb); }
  void* prepareNextFrame() override
  {
    void* gpuPtr = m_output.prepareNextFrame();
    m_output.advance();
    return gpuPtr;
  }
};

} // namespace Gfx::AJA
