#include <deltacast/DeltacastOutputBackend.hpp>

#include <deltacast/DeltacastFormats.hpp>
#include <deltacast/DeltacastRdmaOutput.hpp>

#include <Gfx/Graph/encoders/ColorSpaceOut.hpp>
#include <Gfx/Graph/interop/CudaP2PBridge.h>

#include <VideoMasterHD_ApplicationBuffers.h>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <QDebug>

#include <algorithm>
#include <cstring>

namespace Gfx::Deltacast
{

DeltacastOutputBackend::DeltacastOutputBackend(DeltacastOutputSettings settings)
    : m_settings{settings}
{
}

DeltacastOutputBackend::~DeltacastOutputBackend()
{
  close();
}

bool DeltacastOutputBackend::open(score::gfx::GraphicsApi)
{
  if(VHD_OpenBoardHandle(
         static_cast<ULONG>(m_settings.deviceIndex), &m_board, nullptr, 0)
         != VHDERR_NOERROR
     || !m_board)
    return false;

  // Genlock clock divisor: VHD_CLOCKDIV_1001 for /1.001 rates (59.94/29.97/
  // 23.98), else the integer VHD_CLOCKDIV_1 (60/50/30/25/24).
  VHD_SetBoardProperty(
      m_board, VHD_SDI_BP_GENLOCK_CLOCK_DIV,
      m_settings.fractionalClock ? VHD_CLOCKDIV_1001 : VHD_CLOCKDIV_1);

  if(VHD_OpenStreamHandle(
         m_board, VHD_ST_TX0, VHD_SDI_STPROC_DISJOINED_VIDEO, nullptr, &m_stream,
         nullptr)
         != VHDERR_NOERROR
     || !m_stream)
  {
    close();
    return false;
  }

  const auto vstd = static_cast<VHD_VIDEOSTANDARD>(m_settings.videoStandard);
  const auto pack = static_cast<VHD_BUFFERPACKING>(m_settings.bufferPacking);

  ULONG w = 0, h = 0, fr = 0;
  BOOL32 interlaced = FALSE;
  VHD_GetVideoCharacteristics(vstd, &w, &h, &interlaced, &fr);
  m_width = static_cast<int>(w);
  m_height = static_cast<int>(h);
  // VHD reports the integer rate; the genlock divisor makes the true cadence
  // fr/1.001 — report that so the node's manual rendering rate matches the
  // wire instead of running ~0.1% fast (one shed frame every ~16 s at 59.94).
  m_frameRate = fr ? static_cast<double>(fr) : 60.0;
  if(m_settings.fractionalClock)
    m_frameRate /= 1.001;
  m_rowBytes = static_cast<int>(vhdBytesPerLine(pack, m_width));
  m_frameByteSize
      = static_cast<uint32_t>(m_rowBytes) * static_cast<uint32_t>(m_height);

  applyStreamProperties();

  // RDMA "application buffers" must be registered (VHD_CreateSlotEx) BEFORE
  // VHD_StartStream, but the slot GPU buffers only exist after the strategy's
  // init() — which the node runs after open(). So in RDMA mode we DEFER
  // StartStream to registerRdmaOutputSlots(); if the strategy never engages
  // (no Vulkan / CUDA, or init failed → host-staged fallback), submitFrame()
  // starts the stream lazily on the first frame. The host-staged path with
  // RDMA disabled keeps StartStream here, exactly as before.
  if(!m_settings.useRDMA)
  {
    if(VHD_StartStream(m_stream) != VHDERR_NOERROR)
    {
      qWarning() << "Deltacast: VHD_StartStream (TX) failed";
      close();
      return false;
    }
    m_started = true;
  }
  m_open = true;
  return true;
}

bool DeltacastOutputBackend::applyStreamProperties()
{
  if(!m_stream)
    return false;
  const auto vstd = static_cast<VHD_VIDEOSTANDARD>(m_settings.videoStandard);
  VHD_SetStreamProperty(m_stream, VHD_SDI_SP_VIDEO_STANDARD, m_settings.videoStandard);
  VHD_SetStreamProperty(m_stream, VHD_CORE_SP_BUFFERQUEUE_DEPTH, 4);
  VHD_SetStreamProperty(m_stream, VHD_CORE_SP_BUFFERQUEUE_PRELOAD, 2);
  VHD_SetStreamProperty(
      m_stream, VHD_SDI_SP_INTERFACE, vhdInterfaceFromStandard(vstd));
  VHD_SetStreamProperty(m_stream, VHD_CORE_SP_BUFFER_PACKING, m_settings.bufferPacking);
  return true;
}

bool DeltacastOutputBackend::resetStreamForFallback()
{
  if(!m_board)
    return false;
  if(m_stream)
  {
    VHD_CloseStreamHandle(m_stream);
    m_stream = nullptr;
  }
  if(VHD_OpenStreamHandle(
         m_board, VHD_ST_TX0, VHD_SDI_STPROC_DISJOINED_VIDEO, nullptr, &m_stream,
         nullptr)
         != VHDERR_NOERROR
     || !m_stream)
  {
    qWarning() << "Deltacast: could not reopen the TX stream after a failed "
                  "RDMA registration - playout disabled";
    m_stream = nullptr;
    return false;
  }
  return applyStreamProperties();
}

void DeltacastOutputBackend::quiesce()
{
  // Stop the board from reading any registered buffer while the strategy /
  // ring memory is still alive: VHD_StopStream releases the DMA pin on RDMA
  // application-buffer slots (destroying them), so DeltacastRdmaOutput can
  // then free its VMM VRAM safely. Runs before close(); close() skips the
  // second StopStream via m_started.
  if(m_stream && m_started)
  {
    VHD_StopStream(m_stream);
    m_started = false;
  }
  m_rdmaSlots.clear();
  m_rdmaMode = false;
  {
    std::lock_guard lock{m_busyMutex};
    m_busySlots.clear();
  }
}

void DeltacastOutputBackend::close()
{
  if(m_stream)
  {
    if(m_started)
      VHD_StopStream(m_stream);
    VHD_CloseStreamHandle(m_stream);
    m_stream = nullptr;
  }
  if(m_board)
  {
    VHD_CloseBoardHandle(m_board);
    m_board = nullptr;
  }
  // VHD_StopStream internally destroys any still-registered application-buffer
  // (RDMA) slots, so an explicit VHD_DestroySlot afterwards would target
  // already-freed handles. Just drop our map.
  m_rdmaSlots.clear();
  m_rdmaMode = false;
  m_started = false;
  m_open = false;
}

score::gfx::interop::VideoPixelFormat
DeltacastOutputBackend::wireFormat() const noexcept
{
  return neutralFromPacking(
      static_cast<VHD_BUFFERPACKING>(m_settings.bufferPacking));
}

bool DeltacastOutputBackend::prefersFloatRender() const noexcept
{
  return m_settings.bufferPacking == VHD_BUFPACK_VIDEO_YUV422_10;
}

QString DeltacastOutputBackend::colorConversion() const
{
  return score::gfx::colorMatrixOut(
      AVCOL_SPC_BT709, AVCOL_TRC_BT709, AVCOL_RANGE_MPEG, AVCOL_PRI_BT709);
}

std::vector<score::gfx::interop::HostStagedPlane>
DeltacastOutputBackend::planes() const
{
  return {{m_rowBytes, m_frameByteSize}}; // VHD wire packings are single-plane
}

score::gfx::interop::VendorDmaRegistrar DeltacastOutputBackend::registrar()
{
  // Host-staged v1: the SDK owns its slot buffers; CpuStagedVideoOutput's ring is
  // plain host memory that submitFrame() copies into a VHD slot. No page-lock.
  score::gfx::interop::VendorDmaRegistrar reg;
  reg.registerSlot = [](void*, std::uint32_t) { return true; };
  reg.releaseSlot = [](void*, std::uint32_t) {};
  return reg;
}

std::vector<
    std::function<std::unique_ptr<score::gfx::interop::VideoOutputStrategy>()>>
DeltacastOutputBackend::gpuDirectCandidates(QRhi* rhi, score::gfx::GraphicsApi)
{
  // RDMA GPU-direct playout (Seam B) is the Vulkan-only / CUDA-only fast path.
  // On any other backend (or when RDMA is disabled / no CUDA driver) return {}
  // so the node falls back to the host-staged path (unchanged).
  if(!m_settings.useRDMA || !rhi || rhi->backend() != QRhi::Vulkan
     || !cuda_p2p_available())
    return {};

  const auto neutral = neutralFromPacking(
      static_cast<VHD_BUFFERPACKING>(m_settings.bufferPacking));
  std::vector<
      std::function<std::unique_ptr<score::gfx::interop::VideoOutputStrategy>()>>
      out;
  out.emplace_back([this, neutral] {
    return std::make_unique<DeltacastRdmaOutput>(this, neutral);
  });
  return out;
}

bool DeltacastOutputBackend::registerRdmaOutputSlots(
    void* const* gpuVAs, std::size_t n, std::size_t slotCapacity)
{
  if(!m_stream || !gpuVAs || n == 0)
    return false;

  if(VHD_InitApplicationBuffers(m_stream) != VHDERR_NOERROR)
  {
    qWarning() << "Deltacast: VHD_InitApplicationBuffers (TX) failed";
    resetStreamForFallback();
    return false;
  }

  // The SDK owns the required buffer size ("The size of the buffer is given
  // by VHD_GetApplicationBuffersSize"); if it exceeds the GPU allocation the
  // card would DMA past the end of the slot.
  ULONG required = 0;
  if(VHD_GetApplicationBuffersSize(m_stream, VHD_SDI_BT_VIDEO, &required)
         == VHDERR_NOERROR
     && required > slotCapacity)
  {
    qWarning() << "Deltacast: application buffer size" << required
               << "exceeds the RDMA slot allocation" << qulonglong(slotCapacity)
               << "- falling back to host-staged playout";
    resetStreamForFallback();
    return false;
  }

  auto destroyPartial = [this] {
    for(auto& [va, slot] : m_rdmaSlots)
      VHD_DestroySlot(slot);
    m_rdmaSlots.clear();
  };

  m_rdmaSlots.clear();
  m_rdmaSlots.reserve(n);
  for(std::size_t i = 0; i < n; ++i)
  {
    VHD_APPLICATION_BUFFER_DESCRIPTOR desc[NB_VHD_SDI_BUFFERTYPE]{};
    desc[VHD_SDI_BT_VIDEO].Size = sizeof(VHD_APPLICATION_BUFFER_DESCRIPTOR);
    desc[VHD_SDI_BT_VIDEO].pBuffer = static_cast<UBYTE*>(gpuVAs[i]);
    desc[VHD_SDI_BT_VIDEO].RDMAEnabled = TRUE;
    desc[VHD_SDI_BT_VIDEO].pUserContext = nullptr;
    HANDLE slot = nullptr;
    if(VHD_CreateSlotEx(m_stream, desc, &slot) != VHDERR_NOERROR || !slot)
    {
      qWarning() << "Deltacast: VHD_CreateSlotEx(RDMA TX) failed at slot"
                 << int(i);
      destroyPartial();
      resetStreamForFallback();
      return false;
    }
    m_rdmaSlots.emplace_back(gpuVAs[i], slot);
  }

  if(VHD_StartStream(m_stream) != VHDERR_NOERROR)
  {
    qWarning() << "Deltacast: VHD_StartStream (RDMA TX) failed";
    destroyPartial();
    resetStreamForFallback();
    return false;
  }
  m_started = true;
  m_rdmaMode = true;
  return true;
}

bool DeltacastOutputBackend::rdmaSlotBusy(void* gpuVA)
{
  std::lock_guard lock{m_busyMutex};
  return std::find(m_busySlots.begin(), m_busySlots.end(), gpuVA)
         != m_busySlots.end();
}

score::gfx::interop::PacedFramePump::Hooks DeltacastOutputBackend::pacingHooks()
{
  score::gfx::interop::PacedFramePump::Hooks h;
  h.waitForTick = [] { return true; };
  h.canAccept = {}; // back-pressure is VHD_UnlockSlotHandle blocking in submit
  h.submit = [this](void* p) { return submitFrame(p); };
  return h;
}

bool DeltacastOutputBackend::submitFrame(void* framePtr)
{
  if(!m_stream || !framePtr)
    return false;

  if(m_rdmaMode)
  {
    // GPU-direct: framePtr is the gpuVA DeltacastRdmaOutput::prepareNextFrame()
    // returned (one of the registered slots). Map it back to its VHD slot,
    // queue it for sending, then block on VHD_WaitSlotSent for genlock pacing.
    HANDLE slot = nullptr;
    for(const auto& [va, h] : m_rdmaSlots)
    {
      if(va == framePtr)
      {
        slot = h;
        break;
      }
    }
    if(!slot)
      return false;

    // Mark the slot in flight before the board can touch it; the render
    // thread's prepareNextFrame() skips busy slots so the CUDA copy never
    // overwrites VRAM that is queued or being sent.
    {
      std::lock_guard lock{m_busyMutex};
      if(std::find(m_busySlots.begin(), m_busySlots.end(), framePtr)
         == m_busySlots.end())
        m_busySlots.push_back(framePtr);
    }
    if(VHD_QueueOutSlot(slot) != VHDERR_NOERROR)
    {
      std::lock_guard lock{m_busyMutex};
      std::erase(m_busySlots, framePtr);
      return false;
    }
    HANDLE sent = nullptr;
    if(VHD_WaitSlotSent(m_stream, &sent, 1000) == VHDERR_NOERROR && sent)
    {
      // The sent slot is not necessarily the one just queued (FIFO order);
      // resolve it back to its gpuVA before clearing the busy mark.
      for(const auto& [va, h] : m_rdmaSlots)
      {
        if(h == sent)
        {
          std::lock_guard lock{m_busyMutex};
          std::erase(m_busySlots, va);
          break;
        }
      }
      return true;
    }
    // Timeout/error: the queued slot stays marked busy until a later
    // WaitSlotSent drains it; report the failed transfer.
    return false;
  }

  // Host-staged path. If RDMA was requested but never engaged (no Vulkan /
  // strategy init failed → fallback), StartStream was deferred in open(); do it
  // now on the first frame.
  if(!m_started)
  {
    if(VHD_StartStream(m_stream) != VHDERR_NOERROR)
    {
      qWarning() << "Deltacast: VHD_StartStream (TX, lazy) failed";
      return false;
    }
    m_started = true;
  }

  HANDLE slot = nullptr;
  if(VHD_LockSlotHandle(m_stream, &slot) != VHDERR_NOERROR || !slot)
    return false;

  BYTE* buf = nullptr;
  ULONG size = 0;
  if(VHD_GetSlotBuffer(slot, VHD_SDI_BT_VIDEO, &buf, &size) == VHDERR_NOERROR
     && buf)
  {
    std::memcpy(
        buf, framePtr,
        std::min<ULONG>(size, static_cast<ULONG>(m_frameByteSize)));
  }
  // Blocks until the board consumes the slot -> paces the pump to genlock.
  VHD_UnlockSlotHandle(slot);
  return true;
}

} // namespace Gfx::Deltacast
