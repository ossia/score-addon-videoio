#pragma once
#include <AJA/AjaWireEncoder.hpp>
#include <Gfx/Graph/interop/RdmaVideoOutput.hpp>
#include <Gfx/Graph/interop/VideoOutputStrategy.hpp>

#include <ntv2card.h>

#include <QDebug>

namespace Gfx::AJA
{

/**
 * @brief D3D11 RDMA output for AJA.
 *
 * Thin shim over `score::gfx::interop::RdmaVideoOutput`. AJA-specific
 * bits (card handle, NTV2 format enum) are held by this class; the
 * vendor-neutral helper drives buffer ring + CUDA + encoder + fence.
 */
struct AjaRdmaOutputD3D11 final : score::gfx::interop::VideoOutputStrategy
{
  AjaRdmaOutputD3D11(CNTV2Card* card, NTV2FrameBufferFormat fmt) noexcept
      : m_card{card}, m_targetFormat{fmt} {}

  CNTV2Card* m_card{};
  NTV2FrameBufferFormat m_targetFormat{};
  score::gfx::interop::RdmaVideoOutput m_output;

  const char* name() const noexcept override { return "RDMA-D3D11/T3"; }

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
    oc.slotCount = 2;
    oc.debugName = "AJA-RDMA-D3D11-Storage";
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

    if(!m_output.init(oc))
    {
      qWarning() << "AJA RDMA(D3D11/T3): RdmaVideoOutput init failed";
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
