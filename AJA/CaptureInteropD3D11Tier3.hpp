#pragma once
#include <AJA/AJAInput.hpp>
#include <Gfx/Graph/interop/VideoCaptureStrategy.hpp>

#include <QDebug>

class CNTV2Card;

namespace Gfx::AJA
{

/**
 * @brief D3D11 capture strategy for the future Linux RDMA path.
 *
 * Symmetric inverse of RdmaInteropD3D11Tier3 on the output side. The
 * design lands a CUDA-allocated GPU buffer (DMABufferLock(inRDMA=true))
 * that AJA P2P-DMAs the SDI frame straight into VRAM, with the QRhi
 * D3D11 texture imported via cudaGraphicsD3D11RegisterResource for
 * downstream sampling.
 *
 * Status: NOT YET FUNCTIONAL — init() returns false so AJAInputNode
 * falls through to the DVP strategy on Windows / CPU staging path on
 * Linux. The scaffolding is here so the dispatch + types lock down
 * before the Linux RDMA work begins.
 *
 * The blocker on Windows is the same as for the output side:
 * DMABufferLock(inRDMA=true) is a Linux-only feature in the AJA
 * driver. On Linux this strategy will mirror the (also-not-yet-built)
 * RdmaInteropD3D11Tier3 Linux path.
 */
struct CaptureInteropD3D11Tier3 final : score::gfx::interop::VideoCaptureStrategy
{
  CaptureInteropD3D11Tier3(CNTV2Card* card, AJAInputPixelFormat pixfmt) noexcept
      : m_card{card}, m_pixelFormat{pixfmt} {}

  score::gfx::interop::VideoCaptureStrategyConfig cfg{};
  CNTV2Card* m_card{};
  AJAInputPixelFormat m_pixelFormat{};

  const char* name() const noexcept override { return "RDMA-D3D11/T3"; }

  bool init(const score::gfx::interop::VideoCaptureStrategyConfig& c) override
  {
    cfg = c;
    qDebug() << "AJA RDMA-IN(D3D11/T3): not implemented yet; falling back";
    return false;
  }

  void release() override { }

  std::size_t slotCount() const noexcept override { return 0; }
  void* slotBuffer(std::size_t) const noexcept override { return nullptr; }
  bool ingestFrame(std::size_t) override { return false; }
  QRhiTexture* outputTexture() const noexcept override { return nullptr; }
  void acquireForRender() override { }
  void releaseAfterRender() override { }
};

} // namespace Gfx::AJA
