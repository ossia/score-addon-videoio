#include <bluefish/BluefishOutputBackend.hpp>

#include <bluefish/BluefishFormats.hpp>

#include <Gfx/Graph/encoders/ColorSpaceOut.hpp>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <QDebug>

#include <algorithm>
#include <cstring>

namespace Gfx::Bluefish
{

BluefishOutputBackend::BluefishOutputBackend(BluefishOutputSettings settings)
    : m_settings{settings}
{
}

BluefishOutputBackend::~BluefishOutputBackend()
{
  close();
}

bool BluefishOutputBackend::open(score::gfx::GraphicsApi)
{
  m_bvc = bfcFactory();
  if(!m_bvc)
    return false;

  const BLUE_S32 deviceId = std::max(1, m_settings.deviceIndex);
  if(BLUE_FAIL(bfcAttach(m_bvc, deviceId)))
  {
    qWarning() << "Bluefish: bfcAttach failed for device" << deviceId;
    close();
    return false;
  }

  const auto vidFmt = static_cast<EVideoModeExt>(m_settings.videoModeExt);
  const auto memFmt = static_cast<EMemoryFormat>(m_settings.memoryFormat);

  // Default output setup for the requested mode, then override the knobs we care
  // about (FrameStore engine, frame update, pixel format) and validate/apply.
  blue_setup_info setup = bfcUtilsGetDefaultSetupInfoOutput(
      BLUE_VIDEO_OUTPUT_CHANNEL_1, vidFmt);
  setup.DeviceId = deviceId;
  setup.MemoryFormat = memFmt;
  setup.UpdateMethod = UPD_FMT_FRAME;
  setup.VideoEngine = VIDEO_ENGINE_FRAMESTORE;

  if(BLUE_FAIL(bfcUtilsValidateSetupInfo(&setup)))
  {
    qWarning() << "Bluefish: bfcUtilsValidateSetupInfo (TX) failed";
    close();
    return false;
  }
  if(BLUE_FAIL(bfcUtilsSetupOutput(m_bvc, &setup)))
  {
    qWarning() << "Bluefish: bfcUtilsSetupOutput failed";
    close();
    return false;
  }

  BLUE_U32 w = 0, h = 0, bpl = 0, bpf = 0, golden = 0;
  bfcGetVideoInfo(setup.VideoModeExt, setup.UpdateMethod, setup.MemoryFormat,
                  &w, &h, &bpl, &bpf, &golden);
  m_width = static_cast<int>(w);
  m_height = static_cast<int>(h);
  m_rowBytes = static_cast<int>(bpl);
  m_frameByteSize = bpf;
  m_goldenBytes = golden;

  const BLUE_S32 fps = bfcUtilsGetFpsForVideoMode(setup.VideoModeExt);
  m_frameRate = fps > 0 ? static_cast<double>(fps) : 60.0;
  if(bfcUtilsIsVideoMode1001Framerate(setup.VideoModeExt))
    m_frameRate /= 1.001; // 60 -> 59.94, 30 -> 29.97, ...

  // 4 page-aligned golden host buffers cycled by the FrameStore engine.
  for(auto& b : m_buffers)
  {
    b = bfAlloc(m_goldenBytes);
    if(!b)
    {
      qWarning() << "Bluefish: bfAlloc failed";
      close();
      return false;
    }
    std::memset(b, 0, m_goldenBytes);
  }
  m_bufId = 0;

  // Normal image orientation + turn the black generator off so DMA'd frames show.
  bfcSetCardProperty32(m_bvc, VIDEO_IMAGE_ORIENTATION, ImageOrientation_Normal);
  bfcSetCardProperty32(m_bvc, VIDEO_BLACKGENERATOR, ENUM_BLACKGENERATOR_OFF);

  m_open = true;
  return true;
}

void BluefishOutputBackend::close()
{
  for(auto& b : m_buffers)
  {
    if(b)
    {
      bfFree(m_goldenBytes, b);
      b = nullptr;
    }
  }
  if(m_bvc)
  {
    bfcDetach(m_bvc);
    bfcDestroy(m_bvc);
    m_bvc = nullptr;
  }
  m_bufId = 0;
  m_open = false;
}

score::gfx::interop::VideoPixelFormat
BluefishOutputBackend::wireFormat() const noexcept
{
  return neutralFromMemFmt(m_settings.memoryFormat);
}

bool BluefishOutputBackend::prefersFloatRender() const noexcept
{
  return m_settings.memoryFormat == MEM_FMT_V210; // 10-bit wire
}

QString BluefishOutputBackend::colorConversion() const
{
  // SDR BT.709 limited range. HDR / wide-gamut is a later pass.
  return score::gfx::colorMatrixOut(
      AVCOL_SPC_BT709, AVCOL_TRC_BT709, AVCOL_RANGE_MPEG, AVCOL_PRI_BT709);
}

std::vector<score::gfx::interop::HostStagedPlane>
BluefishOutputBackend::planes() const
{
  return {{m_rowBytes, m_frameByteSize}}; // Bluefish wire packings are single-plane
}

score::gfx::interop::VendorDmaRegistrar BluefishOutputBackend::registrar()
{
  // Host-staged v1: the app owns the bfAlloc golden buffers; CpuStagedVideoOutput's
  // ring is plain host memory that submitFrame() memcpies into the current
  // bfAlloc buffer before DMA. No page-lock of the ring is required (bfAlloc
  // already returns DMA-suitable pages for the staging buffers themselves).
  score::gfx::interop::VendorDmaRegistrar reg;
  reg.registerSlot = [](void*, std::uint32_t) { return true; };
  reg.releaseSlot = [](void*, std::uint32_t) {};
  return reg;
}

std::vector<
    std::function<std::unique_ptr<score::gfx::interop::VideoOutputStrategy>()>>
BluefishOutputBackend::gpuDirectCandidates(QRhi*, score::gfx::GraphicsApi)
{
  // BlueGpuDirect (GPU-direct playout) is out of scope for this host-staged
  // backend; return {} so the node stays on the host-staged path.
  return {};
}

score::gfx::interop::PacedFramePump::Hooks BluefishOutputBackend::pacingHooks()
{
  score::gfx::interop::PacedFramePump::Hooks h;
  h.waitForTick = [this] { return waitForTick(); };
  h.canAccept = {}; // pacing is the output-sync wait; submit does blocking DMA
  h.submit = [this](void* p) { return submitFrame(p); };
  return h;
}

bool BluefishOutputBackend::waitForTick()
{
  if(!m_bvc)
    return false;
  unsigned long fieldCount = 0;
  // Blocks until the next output interrupt -> paces the pump to the SDI clock.
  return BLUE_OK(bfcWaitVideoOutputSync(m_bvc, UPD_FMT_FRAME, &fieldCount));
}

bool BluefishOutputBackend::submitFrame(void* framePtr)
{
  if(!m_bvc || !framePtr)
    return false;

  void* buf = m_buffers[m_bufId];
  if(!buf)
    return false;

  // Copy the host-staged wire frame into the current golden buffer (rest of the
  // page-padded golden region stays zero), then DMA it and schedule display.
  std::memcpy(buf, framePtr, std::min<uint32_t>(m_goldenBytes, m_frameByteSize));

  // No SyncInfo => the DMA is blocking (returns bytes copied, < 0 on error).
  const BLUE_S32 copied = bfcDmaWriteToCardAsync(
      m_bvc, static_cast<BLUE_U8*>(buf), m_goldenBytes, nullptr,
      BlueImage_VBI_DMABuffer(static_cast<unsigned long>(m_bufId),
                              BLUE_DMA_DATA_TYPE_IMAGE),
      0);
  if(copied < 0)
  {
    qWarning() << "Bluefish: bfcDmaWriteToCardAsync failed" << copied;
    return false;
  }

  // Tell the card to present this buffer (image + VBI) at the next interrupt.
  bfcRenderBufferUpdate(
      m_bvc, BlueBuffer_Image_VBI(static_cast<unsigned long>(m_bufId)));

  m_bufId = (m_bufId + 1) % kBufferCount;
  return true;
}

} // namespace Gfx::Bluefish
