#pragma once

/**
 * @file AjaDmaLock.hpp
 * @brief Thin wrappers over CNTV2Card::DMABufferLock / DMABufferUnlock.
 *
 * Every AJA capture/output interop strategy page-locks its DMA buffers with
 * the same boilerplate — `reinterpret_cast<const ULWord*>(ptr)` +
 * `static_cast<ULWord>(bytes)` + the `inMap=true` convention — repeated across
 * a dozen files. These helpers centralize the casts and the convention so the
 * per-strategy lock/unlock loops read intention-first, and a null card / null
 * pointer is handled uniformly.
 *
 * The `inRDMA` flag stays an explicit argument: it is the one genuine
 * per-strategy difference (false for sysmem/paged DMA, true for GPUDirect
 * P2P into VRAM).
 */

#include <Gfx/Graph/interop/CudaInterop.h>

#include <ntv2card.h>

#include <QDebug>

#include <cstdint>
#include <vector>

namespace Gfx::AJA
{

/**
 * @brief Page-lock @p bytes at @p ptr for AJA DMA.
 * @param rdma true => GPUDirect RDMA (P2P into a GPU device pointer);
 *             false => paged host-memory DMA.
 * @return true on success; false if @p card or @p ptr is null, or the lock
 *         failed.
 */
[[nodiscard]] inline bool ajaDmaLock(
    CNTV2Card* card, const void* ptr, std::uint32_t bytes, bool rdma) noexcept
{
  return card && ptr
         && card->DMABufferLock(
                reinterpret_cast<const ULWord*>(ptr),
                static_cast<ULWord>(bytes), /*inMap=*/true, /*inRDMA=*/rdma);
}

/// Unlock a buffer previously locked with ajaDmaLock(). No-op if null.
inline void ajaDmaUnlock(
    CNTV2Card* card, const void* ptr, std::uint32_t bytes) noexcept
{
  if(card && ptr)
    card->DMABufferUnlock(
        reinterpret_cast<const ULWord*>(ptr), static_cast<ULWord>(bytes));
}

/**
 * @brief Probe whether GPUDirect-RDMA capture (C2H) actually *delivers* data
 *        on this card↔GPU path — not merely whether the DMA call returns
 *        success.
 *
 * On platforms where the AJA card and the GPU sit on different PCIe host
 * bridges (e.g. an AMD Threadripper with the GPU on another root complex),
 * peer-to-peer DMA is broken even though every prerequisite *reports* success:
 * `DMABufferLock(inRDMA=true)` succeeds (nvidia_p2p_get_pages just maps the
 * pages) and `DMAReadFrame`/`AutoCirculateTransfer` return true, yet the card's
 * write into GPU memory is silently dropped (a posted write that never lands).
 * A pin-only check therefore engages a capture path that produces all-zero
 * frames. This probe seeds a pinned GPU buffer with a sentinel, issues a real
 * card→GPU transfer, and reads back to confirm the bytes actually changed.
 *
 * @return true iff the probe buffer's contents changed (P2P really moved data).
 *         false on any allocation/lock/transfer error or if the sentinel
 *         survived unchanged — callers should then fall back to CPU staging.
 */
[[nodiscard]] inline bool ajaCaptureRdmaDelivers(
    CNTV2Card* card, CudaInteropContextHandle cudaCtx,
    std::uint32_t frameByteSize) noexcept
{
  if(!card || !cudaCtx || frameByteSize == 0)
    return false;

  // A modest slice is enough to detect a dropped transfer; DMAReadFrame reads
  // the first `probeBytes` of the framestore. 64 KiB matches the GPU page /
  // RDMA granularity.
  const std::uint32_t probeBytes
      = frameByteSize < (64u * 1024) ? frameByteSize : (64u * 1024);

  void* gpu = nullptr;
  if(cuda_interop_alloc_buffer(cudaCtx, frameByteSize, &gpu) != CUDA_INTEROP_SUCCESS
     || !gpu)
    return false;

  bool delivered = false;
  if(ajaDmaLock(card, gpu, frameByteSize, /*rdma=*/true))
  {
    constexpr unsigned char kSentinel = 0xEE;
    std::vector<unsigned char> seed(probeBytes, kSentinel);
    if(cuda_interop_upload_buffer(cudaCtx, gpu, seed.data(), probeBytes)
       == CUDA_INTEROP_SUCCESS)
    {
      // Real card→GPU transfer of framestore 0 into the pinned buffer.
      if(card->DMAReadFrame(
             0, reinterpret_cast<ULWord*>(gpu),
             static_cast<ULWord>(probeBytes)))
      {
        std::vector<unsigned char> back(probeBytes, kSentinel);
        if(cuda_interop_download_buffer(cudaCtx, back.data(), gpu, probeBytes)
           == CUDA_INTEROP_SUCCESS)
        {
          for(unsigned char b : back)
          {
            if(b != kSentinel)
            {
              delivered = true;
              break;
            }
          }
        }
      }
    }
    ajaDmaUnlock(card, gpu, frameByteSize);
  }
  cuda_interop_free_buffer(cudaCtx, gpu);

  if(!delivered)
    qWarning() << "AJA capture RDMA probe: card→GPU P2P transfer did not "
                  "deliver data (pin/return-code succeeded but the write was "
                  "dropped — GPU likely on a different PCIe host bridge). "
                  "Falling back to CPU staging.";
  return delivered;
}

} // namespace Gfx::AJA
