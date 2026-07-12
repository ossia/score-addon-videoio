#pragma once
#include <AJA/AjaWireEncoder.hpp>
#include <Gfx/Graph/interop/VideoOutputStrategy.hpp>

#include <ntv2card.h>

#include <QDebug>

namespace Gfx::AJA
{

/**
 * @brief D3D12 RDMA output for AJA — STUB.
 *
 * QRhi D3D12 backend allocates StorageBuffers via D3D12MA on a regular
 * DEFAULT heap with no `D3D12_HEAP_FLAG_SHARED` and no NT-shared handle
 * — so `cudaImportExternalMemory(D3D12_RESOURCE)` cannot pick them up.
 * Same dispatch-side gap as Vulkan output: would need raw D3D12
 * compute via `QRhi::beginExternal` against an externally-allocated
 * SHARED+UAV resource. AJA's Windows driver also doesn't expose
 * `DMABufferLock(inRDMA=true)` so this is dead-on-Windows-AJA today —
 * kept as scaffolding for future SDKs (BlackMagic etc) that might
 * activate the D3D12 path.
 */
struct AjaRdmaOutputD3D12 final : score::gfx::interop::VideoOutputStrategy
{
  AjaRdmaOutputD3D12(CNTV2Card* card, NTV2FrameBufferFormat fmt) noexcept
      : m_card{card}, m_targetFormat{fmt} {}

  CNTV2Card* m_card{};
  NTV2FrameBufferFormat m_targetFormat{};

  const char* name() const noexcept override { return "RDMA-D3D12/T3"; }

  bool init(const score::gfx::interop::VideoOutputStrategyConfig& c) override
  {
    if(!c.rhi || !c.state)
      return false;
    qDebug() << "AJA RDMA(D3D12/T3): not implemented yet; falling back to "
                "encoder + CPU staging";
    return false;
  }

  void release() override { }
  void encodeFrame(QRhiCommandBuffer&) override { }
  void* prepareNextFrame() override { return nullptr; }
};

} // namespace Gfx::AJA
