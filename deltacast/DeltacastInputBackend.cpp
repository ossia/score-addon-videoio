#include <deltacast/DeltacastInputBackend.hpp>

#include <deltacast/DeltacastCpuCapture.hpp>
#include <deltacast/DeltacastFormats.hpp>
#include <deltacast/DeltacastRdmaCapture.hpp>

#include <Gfx/Graph/decoders/WireDecoderFactory.hpp>
#include <Gfx/Graph/interop/CudaInterop.h>
#include <Gfx/Graph/interop/VideoCaptureStrategy.hpp>
#include <Gfx/Graph/interop/VideoPixelFormatAV.hpp>

#include <Video/VideoInterface.hpp>

#include <VideoMasterHD_ApplicationBuffers.h>

#include <QDebug>

#include <algorithm>
#include <cstring>

namespace Gfx::Deltacast
{

DeltacastInputBackend::DeltacastInputBackend(
    DeltacastInputSettings settings,
    score::gfx::interop::VideoCaptureSlotRing& ring)
    : m_settings{settings}, m_ring{ring}
{
}

DeltacastInputBackend::~DeltacastInputBackend()
{
  stop();
}

bool DeltacastInputBackend::open()
{
  if(VHD_OpenBoardHandle(
         static_cast<ULONG>(m_settings.deviceIndex), &m_board, nullptr, 0)
         != VHDERR_NOERROR
     || !m_board)
    return false;

  if(VHD_OpenStreamHandle(
         m_board, VHD_ST_RX0, VHD_SDI_STPROC_DISJOINED_VIDEO, nullptr, &m_stream,
         nullptr)
         != VHDERR_NOERROR
     || !m_stream)
  {
    if(m_board) { VHD_CloseBoardHandle(m_board); m_board = nullptr; }
    return false;
  }

  // Auto-detect the incoming standard unless one was pinned in the settings.
  ULONG vstd = m_settings.videoStandard;
  if(vstd == 0)
  {
    if(VHD_GetStreamProperty(m_stream, VHD_SDI_SP_VIDEO_STANDARD, &vstd)
           != VHDERR_NOERROR
       || vstd == NB_VHD_VIDEOSTANDARDS)
    {
      qWarning() << "Deltacast input: no incoming video standard detected";
      VHD_CloseStreamHandle(m_stream); m_stream = nullptr;
      VHD_CloseBoardHandle(m_board); m_board = nullptr;
      return false;
    }
  }
  m_settings.videoStandard = vstd;

  const auto pack = static_cast<VHD_BUFFERPACKING>(m_settings.bufferPacking);
  ULONG w = 0, h = 0, fr = 0;
  BOOL32 interlaced = FALSE;
  VHD_GetVideoCharacteristics(
      static_cast<VHD_VIDEOSTANDARD>(vstd), &w, &h, &interlaced, &fr);
  m_width = static_cast<int>(w);
  m_height = static_cast<int>(h);
  m_frameByteSize = vhdBytesPerLine(pack, m_width) * static_cast<uint32_t>(m_height);

  VHD_SetStreamProperty(m_stream, VHD_SDI_SP_VIDEO_STANDARD, vstd);
  VHD_SetStreamProperty(m_stream, VHD_CORE_SP_TRANSFER_SCHEME, VHD_TRANSFER_SLAVED);
  VHD_SetStreamProperty(m_stream, VHD_CORE_SP_BUFFER_PACKING, m_settings.bufferPacking);

  // RDMA "application buffers" must be registered (VHD_CreateSlotEx) BEFORE
  // VHD_StartStream, but the slot GPU buffers only exist after the strategy's
  // init() — which the node runs after open(). So in RDMA mode we DEFER
  // StartStream to start(), once the strategy is bound and its slots are valid.
  // The host-staged path keeps StartStream here, exactly as before.
  if(!m_settings.useRDMA)
  {
    if(VHD_StartStream(m_stream) != VHDERR_NOERROR)
    {
      qWarning() << "Deltacast input: VHD_StartStream (RX) failed";
      VHD_CloseStreamHandle(m_stream); m_stream = nullptr;
      VHD_CloseBoardHandle(m_board); m_board = nullptr;
      return false;
    }
    m_streamStarted = true;
  }
  return true;
}

Video::ImageFormat DeltacastInputBackend::imageFormat() const
{
  Video::ImageFormat f;
  f.width = m_width;
  f.height = m_height;
  f.pixel_format = score::gfx::interop::toAVPixelFormat(
      neutralFromPacking(static_cast<VHD_BUFFERPACKING>(m_settings.bufferPacking)));
  f.color_space = AVCOL_SPC_BT709;
  f.color_primaries = AVCOL_PRI_BT709;
  f.color_trc = AVCOL_TRC_BT709;
  f.color_range = AVCOL_RANGE_MPEG;
  return f;
}

std::unique_ptr<score::gfx::GPUVideoDecoder>
DeltacastInputBackend::makeDecoder(Video::VideoMetadata& meta)
{
  return score::gfx::makeWireDecoder(
      neutralFromPacking(static_cast<VHD_BUFFERPACKING>(m_settings.bufferPacking)),
      meta);
}

std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
DeltacastInputBackend::pickStrategy(QRhi::Implementation impl)
{
  // RDMA GPU-direct (Seam B) is the Vulkan-only / CUDA-only fast path. On any
  // other backend (or when RDMA is disabled / no CUDA driver) return {} so the
  // node falls back to makeCpuStrategy() (host-staged, unchanged).
  if(!m_settings.useRDMA || impl != QRhi::Vulkan || !cuda_interop_available())
    return {};
  auto strat = std::make_unique<DeltacastRdmaCapture>();
  m_rdmaStrategy = strat.get();
  return strat;
}

std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
DeltacastInputBackend::makeCpuStrategy()
{
  return std::make_unique<DeltacastCpuCapture>();
}

void DeltacastInputBackend::setStrategy(
    score::gfx::interop::VideoCaptureStrategy* s) noexcept
{
  m_strategy = s;
  // If the node bound a strategy other than the RDMA one we created (i.e. its
  // init() failed and the node swapped in the CPU-staging fallback), drop the
  // RDMA pointer so start() takes the host-staged path.
  if(s != static_cast<score::gfx::interop::VideoCaptureStrategy*>(
             m_rdmaStrategy))
    m_rdmaStrategy = nullptr;
}

bool DeltacastInputBackend::rdmaActive() const noexcept
{
  return m_rdmaStrategy != nullptr
         && m_strategy
                == static_cast<score::gfx::interop::VideoCaptureStrategy*>(
                       m_rdmaStrategy);
}

void DeltacastInputBackend::start()
{
  if(m_started || !m_stream)
    return;

  if(rdmaActive())
  {
    // Register the strategy's RDMA GPU buffers as Deltacast "application
    // buffers" (RDMAEnabled=TRUE), prime the FIFO, then StartStream — the
    // deferred-from-open() sequence. The card will DMA each received frame
    // straight into slot GPU VRAM. See Sample_RX_ApplicationBuffers_RDMA.
    auto destroyPartial = [this] {
      for(auto& slot : m_vhdSlots)
      {
        if(slot)
          VHD_DestroySlot(slot);
        slot = nullptr;
      }
    };
    if(VHD_InitApplicationBuffers(m_stream) != VHDERR_NOERROR)
    {
      qWarning() << "Deltacast input: VHD_InitApplicationBuffers failed";
      return;
    }
    // The SDK dictates the required buffer size; the strategy allocated
    // m_frameByteSize per slot, so a larger requirement would let the card
    // DMA past the end of the GPU allocation.
    ULONG required = 0;
    if(VHD_GetApplicationBuffersSize(m_stream, VHD_SDI_BT_VIDEO, &required)
           == VHDERR_NOERROR
       && required > m_frameByteSize)
    {
      qWarning() << "Deltacast input: application buffer size" << required
                 << "exceeds the RDMA slot allocation" << m_frameByteSize;
      return;
    }
    for(std::size_t i = 0; i < kRdmaSlotCount; ++i)
    {
      VHD_APPLICATION_BUFFER_DESCRIPTOR desc[NB_VHD_SDI_BUFFERTYPE]{};
      desc[VHD_SDI_BT_VIDEO].Size
          = sizeof(VHD_APPLICATION_BUFFER_DESCRIPTOR);
      desc[VHD_SDI_BT_VIDEO].pBuffer
          = static_cast<UBYTE*>(m_rdmaStrategy->slotBuffer(i));
      desc[VHD_SDI_BT_VIDEO].RDMAEnabled = TRUE;
      desc[VHD_SDI_BT_VIDEO].pUserContext = nullptr;
      if(VHD_CreateSlotEx(m_stream, desc, &m_vhdSlots[i]) != VHDERR_NOERROR
         || !m_vhdSlots[i])
      {
        qWarning() << "Deltacast input: VHD_CreateSlotEx(RDMA) failed at slot"
                   << int(i);
        destroyPartial();
        return;
      }
    }
    // Prime the FIFO BEFORE StartStream (vendor sample order): the card must
    // have DMA targets from the very first frame or the capture starts with
    // guaranteed drops / WaitSlotFilled errors.
    for(std::size_t i = 0; i < kRdmaSlotCount; ++i)
      VHD_QueueInSlot(m_vhdSlots[i]);
    if(VHD_StartStream(m_stream) != VHDERR_NOERROR)
    {
      qWarning() << "Deltacast input: VHD_StartStream (RDMA) failed";
      destroyPartial();
      return;
    }
    m_streamStarted = true;
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread{[this] { runLoopRdma(); }};
    m_started = true;
    return;
  }

  // Host-staged path. If RDMA was requested but not engaged (no Vulkan / init
  // failed → CPU fallback), StartStream was deferred in open(); do it now.
  if(!m_streamStarted)
  {
    if(VHD_StartStream(m_stream) != VHDERR_NOERROR)
    {
      qWarning() << "Deltacast input: VHD_StartStream (RX) failed";
      return;
    }
    m_streamStarted = true;
  }
  m_running.store(true, std::memory_order_release);
  m_thread = std::thread{[this] { runLoop(); }};
  m_started = true;
}

void DeltacastInputBackend::stop()
{
  m_running.store(false, std::memory_order_release);
  if(m_thread.joinable())
    m_thread.join();
  if(m_stream)
  {
    // VHD_StopStream internally destroys any still-registered application-buffer
    // slots (RDMA mode), so an explicit VHD_DestroySlot afterwards would target
    // already-freed handles. Just drop our handles.
    VHD_StopStream(m_stream);
    VHD_CloseStreamHandle(m_stream);
    m_stream = nullptr;
  }
  if(m_board)
  {
    VHD_CloseBoardHandle(m_board);
    m_board = nullptr;
  }
  m_vhdSlots = {};
  m_streamStarted = false;
  m_started = false;
}

void DeltacastInputBackend::runLoop()
{
  std::size_t writeIdx = 0;
  while(m_running.load(std::memory_order_acquire))
  {
    auto* strat = m_strategy;
    if(!strat)
      continue;
    const std::size_t slots = strat->slotCount();
    if(slots == 0)
      continue;

    HANDLE slot = nullptr;
    const ULONG r = VHD_LockSlotHandle(m_stream, &slot);
    if(r != VHDERR_NOERROR)
    {
      if(r == VHDERR_TIMEOUT)
        continue; // no signal yet; keep polling
      break;
    }

    BYTE* buf = nullptr;
    ULONG size = 0;
    if(VHD_GetSlotBuffer(slot, VHD_SDI_BT_VIDEO, &buf, &size) == VHDERR_NOERROR
       && buf)
    {
      if(void* dst = strat->slotBuffer(writeIdx))
      {
        std::memcpy(
            dst, buf,
            std::min<ULONG>(size, static_cast<ULONG>(m_frameByteSize)));
        strat->ingestFrame(writeIdx);
        m_ring.latestSlot.store(writeIdx, std::memory_order_release);
        m_ring.latestFrameId.fetch_add(1, std::memory_order_release);
        writeIdx = (writeIdx + 1) % slots;
      }
    }
    VHD_UnlockSlotHandle(slot);
  }
}

void DeltacastInputBackend::runLoopRdma()
{
  auto* strat = m_rdmaStrategy;
  if(!strat)
    return;

  // The FIFO was primed (every slot queued) before StartStream in start().

  while(m_running.load(std::memory_order_acquire))
  {
    HANDLE filled = nullptr;
    const ULONG r = VHD_WaitSlotFilled(m_stream, &filled, 100);
    if(r != VHDERR_NOERROR)
    {
      if(r == VHDERR_TIMEOUT)
        continue; // no signal yet; keep polling
      break;
    }

    // Map the filled slot handle back to its index: the card RDMA'd the frame
    // straight into slot i's GPU VRAM, so ingestFrame(i) just does the CUDA
    // copy buffer->texture (no host transit) and publishes.
    std::size_t idx = kRdmaSlotCount;
    for(std::size_t i = 0; i < kRdmaSlotCount; ++i)
    {
      if(m_vhdSlots[i] == filled)
      {
        idx = i;
        break;
      }
    }
    if(idx < kRdmaSlotCount)
    {
      strat->ingestFrame(idx);
      m_ring.latestSlot.store(idx, std::memory_order_release);
      m_ring.latestFrameId.fetch_add(1, std::memory_order_release);
    }

    // Recycle the slot back into the FIFO for the next frame.
    VHD_QueueInSlot(filled);
  }
}

} // namespace Gfx::Deltacast
