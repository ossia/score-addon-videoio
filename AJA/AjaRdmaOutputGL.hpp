#pragma once
#include <AJA/AjaWireEncoder.hpp>
#include <Gfx/Graph/interop/RdmaVideoOutput.hpp>
#include <Gfx/Graph/interop/VideoOutputStrategy.hpp>
#include <Gfx/Graph/interop/RdmaRingDepth.hpp>

#include <ntv2card.h>

#include <QDebug>

namespace Gfx::AJA
{

/**
 * @brief OpenGL tier-3 RDMA output for AJA. Thin shim over
 *        `score::gfx::interop::RdmaVideoOutput`.
 */
struct AjaRdmaOutputGL final : score::gfx::interop::VideoOutputStrategy
{
  AjaRdmaOutputGL(CNTV2Card* card, NTV2FrameBufferFormat fmt) noexcept
      : m_card{card}, m_targetFormat{fmt} {}

  CNTV2Card* m_card{};
  NTV2FrameBufferFormat m_targetFormat{};
  score::gfx::interop::RdmaVideoOutput m_output;

  const char* name() const noexcept override { return "RDMA-GL/T3"; }

  static bool isSupported(QRhi* rhi, NTV2FrameBufferFormat fmt, int width)
  {
    return rhi && rhi->isFeatureSupported(QRhi::Compute)
           && ajaWireEncoderSupports(fmt, width);
  }

  bool init(const score::gfx::interop::VideoOutputStrategyConfig& c) override
  {
    if(!isSupported(c.rhi, m_targetFormat, c.width) || !c.state)
      return false;

    score::gfx::interop::RdmaVideoOutputConfig oc{};
    oc.rhi = c.rhi;
    oc.state = c.state;
    oc.sourceTexture = c.sourceTexture;
    oc.width = c.width;
    oc.height = c.height;
    oc.frameByteSize = c.frameByteSize;
    // BAR1 budget: every pinned bounce occupies the GPU's PCIe BAR1
    // aperture (256 MiB on most Quadros) for the card's P2P access. At
    // large rasters (UHD2/8K = 66 MB frames) double-buffering the output
    // bounce would starve the capture side's pins. One slot is race-free
    // here because AutoCirculateTransfer is a blocking DMA — the card has
    // finished reading the bounce before submit returns.
    oc.slotCount = score::gfx::interop::rdmaRingDepthForFrame(
        c.frameByteSize, {/*full=*/2, /*large=*/1});
    oc.debugName = "AJA-RDMA-GL-Storage";
    oc.encoderFactory = [fmt = m_targetFormat] {
      return ajaMakeWireEncoder(fmt);
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
      qWarning() << "AJA RDMA(GL/T3): RdmaVideoOutput init failed";
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
