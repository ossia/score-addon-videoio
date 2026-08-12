#include "ArgusInputBackend.hpp"

#include <Gfx/Graph/decoders/WireDecoderFactory.hpp>
#include <Gfx/Graph/interop/BorrowedHostImportCapture.hpp>
#include <Gfx/Graph/interop/CpuStagedCapture.hpp>
#include <Gfx/Graph/interop/VideoPixelFormat.hpp>

#if defined(__linux__)
#include <Gfx/Graph/interop/DmaBufImportCapture.hpp>
#endif

#include <QDebug>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace Gfx::Argus
{

/// Host-staged fallback. The Argus loop leaves the frame in its NvBufSurface
/// and the render thread copies out of the host mapping; on Vulkan the shared
/// implementation still imports the slot pages once, so the per-frame staging
/// copy disappears even here.
struct ArgusCpuPolicy : score::gfx::interop::CpuStagedNoLockPolicy
{
  static constexpr const char* fixed_name = "Argus-CPU";
};
using ArgusCpuCapture = score::gfx::interop::CpuStagedCapture<ArgusCpuPolicy>;


ArgusInputBackend::ArgusInputBackend(
    ArgusSettings settings, score::gfx::interop::VideoCaptureSlotRing& ring)
    : m_settings{std::move(settings)}
    , m_ring{ring}
{
}

ArgusInputBackend::~ArgusInputBackend()
{
  stop();
}

bool ArgusInputBackend::open()
{
  return m_session.open(m_settings);
}

int ArgusInputBackend::width() const noexcept
{
  return int(m_session.width());
}
int ArgusInputBackend::height() const noexcept
{
  return int(m_session.height());
}
std::uint32_t ArgusInputBackend::frameByteSize() const noexcept
{
  return m_session.frameByteSize();
}
std::int32_t ArgusInputBackend::resolvedSensorMode() const noexcept
{
  return m_session.resolvedSensorMode();
}

Video::ImageFormat ArgusInputBackend::imageFormat() const
{
  Video::ImageFormat f;
  f.width = int(m_session.width());
  f.height = int(m_session.height());
  // Argus emits NV12 and nothing else.
  f.pixel_format = AV_PIX_FMT_NV12;
  // The ISP outputs BT.709 limited range. Argus can report the colour space
  // per capture through CaptureMetadata, but the stream-level answer does not
  // change mid-run, so the decoder is built from the constant.
  f.color_space = AVCOL_SPC_BT709;
  f.color_primaries = AVCOL_PRI_BT709;
  f.color_trc = AVCOL_TRC_BT709;
  f.color_range = AVCOL_RANGE_MPEG;
  return f;
}

std::unique_ptr<score::gfx::GPUVideoDecoder>
ArgusInputBackend::makeDecoder(Video::VideoMetadata& meta)
{
  return score::gfx::makeWireDecoder(
      score::gfx::interop::VideoPixelFormat::NV12, meta);
}

std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
ArgusInputBackend::makeDmaBufRung()
{
#if defined(__linux__)
  const auto& slots = m_session.slots();
  if(slots.empty())
    return {};

  std::vector<score::gfx::interop::DmaBufSlotDesc> descs;
  descs.reserve(slots.size());
  for(const auto& s : slots)
  {
    if(s.dmabufFd < 0)
      return {};
    // Plane 0's offset and pitch; the importer derives the chroma plane from
    // the decoder's plane textures, and the session already warned if
    // NvBufSurface disagreed with that derivation.
    descs.push_back(
        {s.dmabufFd, /*modifier, pitch-linear*/ 0ull, s.offset[0], s.pitch[0]});
  }

  auto strat = std::make_unique<score::gfx::interop::DmaBufImportCapture>(
      "Argus", std::move(descs));
  m_dmabuf = strat.get();
  return strat;
#else
  return {};
#endif
}

std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
ArgusInputBackend::makeBorrowedRung()
{
  // Needs the surfaces host-mapped; that mapping is not free on Tegra, so it
  // is only done when this rung is actually being tried.
  if(!m_session.mapHost())
    return {};

  const auto& slots = m_session.slots();
  std::vector<score::gfx::interop::BorrowedHostBuffer> bufs;
  bufs.reserve(slots.size());
  for(const auto& s : slots)
  {
    if(!s.host)
      return {};
    bufs.push_back({s.host, std::size_t(s.totalBytes)});
  }

  auto strat = std::make_unique<score::gfx::interop::BorrowedHostImportCapture>(
      "Argus", std::move(bufs));
  m_borrowed = strat.get();
  return strat;
}

std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
ArgusInputBackend::pickStrategy(
    QRhi::Implementation, const score::gfx::interop::GpuCapabilities&)
{
  // pickStrategies is the real entry point; this exists because the base class
  // requires it, and returning the best single guess keeps a caller that only
  // knows the old interface working.
  m_dmabuf = nullptr;
  m_borrowed = nullptr;
  return makeDmaBufRung();
}

std::vector<std::function<std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>()>>
ArgusInputBackend::pickStrategies(
    QRhi::Implementation, const score::gfx::interop::GpuCapabilities&)
{
  m_dmabuf = nullptr;
  m_borrowed = nullptr;

  std::vector<std::function<std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>()>> v;
  // Best first. Whether either works is only knowable by initialising it: the
  // Tegra driver may refuse an NV12 dma-buf import that it happily host-maps,
  // and the reverse happens on desktop NVIDIA.
  v.emplace_back([this] { return makeDmaBufRung(); });
  v.emplace_back([this] { return makeBorrowedRung(); });
  return v;
}

std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
ArgusInputBackend::makeCpuStrategy()
{
  // The universal fallback copies out of the slot, so the surfaces must be
  // readable from the host.
  m_session.mapHost();
  return std::make_unique<ArgusCpuCapture>();
}

void ArgusInputBackend::setStrategy(
    score::gfx::interop::VideoCaptureStrategy* s) noexcept
{
  m_strategy = s;
  // Only the two borrowed rungs report which slots the renderer has finished
  // with; if neither is engaged, m_dmabuf/m_borrowed stay null and the release
  // loop hands every slot straight back.
  if(s != m_dmabuf)
    m_dmabuf = nullptr;
  if(s != m_borrowed)
    m_borrowed = nullptr;
}

void ArgusInputBackend::start()
{
  if(!m_session.isOpen())
    return;

  const auto slotCount = m_session.slots().size();

  m_session.start(
      // Capture thread: publish the slot the ISP just filled.
      [this, slotCount](std::size_t slot) {
        if(slot >= slotCount)
          return;
        if(m_strategy)
        {
          // A borrowed rung takes ownership here; if it declines the frame
          // (its ring is full) the slot is left for the release drain below,
          // which is what stops a stalled renderer from starving the ISP.
          if(!m_strategy->ingestFrame(slot))
            return;
        }
        m_ring.latestSlot.store(slot, std::memory_order_release);
        m_ring.latestFrameId.fetch_add(1, std::memory_order_release);
      },
      // Capture thread: which slots may go back to the ISP.
      [this, slotCount]() -> std::uint32_t {
        if(m_dmabuf)
          return m_dmabuf->takeReturnedSlots();
        if(m_borrowed)
          return m_borrowed->takeReturnedSlots();
        // The CPU rung copies the frame out during acquireForRender, so the
        // slot is free again as soon as it has been published.
        return slotCount >= 32u ? 0xFFFFFFFFu
                                : ((1u << slotCount) - 1u);
      });
}

void ArgusInputBackend::stop()
{
  m_session.stop();
}

}
