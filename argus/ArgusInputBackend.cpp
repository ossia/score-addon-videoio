#include "ArgusInputBackend.hpp"

#include <Gfx/Graph/decoders/NV12ExternalOES.hpp>
#include <Gfx/Graph/decoders/WireDecoderFactory.hpp>
#include <Gfx/Graph/interop/BorrowedHostImportCapture.hpp>
#include <Gfx/Graph/interop/CpuStagedCapture.hpp>
#include <Gfx/Graph/interop/VideoPixelFormat.hpp>

#if defined(__linux__)
#include <Gfx/Graph/interop/DmaBufImportCapture.hpp>
#endif

#include <QDebug>

#include <algorithm>

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
  if(m_settings.syncRig.empty())
  {
    m_session = &m_ownSession;
    return m_ownSession.open(m_settings);
  }

  m_rig = acquireArgusRig(
      m_settings.syncRig, std::max<std::size_t>(m_settings.sensorIds.size(), 1));
  if(!m_rig || !m_rig->open(m_settings))
    return false;

  auto* session = m_rig->sessionFor(m_settings.syncMember);
  if(!session)
  {
    qWarning() << "Argus: no session for rig member" << m_settings.syncMember;
    return false;
  }

  m_stream = m_rig->streamFor(m_settings.syncMember);
  // Refused rather than clamped: a member reading a stream that is not its own
  // renders the wrong sensor, and two members both landing on stream 0 look
  // perfectly synchronised because they are the same eye twice.
  if(m_stream >= session->streamCount())
  {
    qWarning() << "Argus: rig member" << m_settings.syncMember
               << "has no stream of its own in a session of"
               << int(session->streamCount()) << "-- refusing to render a "
                                                 "sensor that is not this one";
    return false;
  }

  m_session = session;
  return true;
}

const std::vector<ArgusSlot>& ArgusInputBackend::mySlots() const noexcept
{
  static const std::vector<ArgusSlot> none;
  return m_session ? m_session->slots(m_stream) : none;
}

int ArgusInputBackend::width() const noexcept
{
  return m_session ? int(m_session->width()) : 0;
}
int ArgusInputBackend::height() const noexcept
{
  return m_session ? int(m_session->height()) : 0;
}
std::uint32_t ArgusInputBackend::frameByteSize() const noexcept
{
  return m_session ? m_session->frameByteSize() : 0u;
}
std::int32_t ArgusInputBackend::resolvedSensorMode() const noexcept
{
  return m_session ? m_session->resolvedSensorMode() : -1;
}

score::gfx::DMACaptureBackend::SyncMembership
ArgusInputBackend::syncGroup() noexcept
{
  if(!m_rig)
    return {};
  return {&m_rig->group(), m_settings.syncMember};
}

Video::ImageFormat ArgusInputBackend::imageFormat() const
{
  Video::ImageFormat f;
  f.width = width();
  f.height = height();
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
  // pickStrategies() runs before this (DMACaptureInputNode::init probes caps
  // and builds the ladder first), so by now we know whether the external-image
  // rung is on the table. It matters: an external sampler returns RGB already
  // converted, so pairing it with the ordinary NV12 decoder would convert
  // twice, and pairing the ordinary rungs with this decoder would show raw
  // luma. The two must be chosen together.
  if(m_wantExternal)
    return std::make_unique<score::gfx::NV12ExternalOESDecoder>(meta);

  return score::gfx::makeWireDecoder(
      score::gfx::interop::VideoPixelFormat::NV12, meta);
}

std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
ArgusInputBackend::makeDmaBufRung()
{
#if defined(__linux__)
  const auto& slots = mySlots();
  if(slots.empty())
    return {};

  std::vector<score::gfx::interop::DmaBufSlotDesc> descs;
  descs.reserve(slots.size());
  for(const auto& s : slots)
  {
    if(s.dmabufFd < 0)
      return {};
    // State the layout rather than let it be derived: NvBufSurface aligns each
    // plane offset to 64 KB even with row padding disabled, so a derived chroma
    // offset lands in the wrong place and the rung declines.
    score::gfx::interop::DmaBufSlotDesc d{
        s.dmabufFd, /*modifier, pitch-linear*/ 0ull, s.offset[0], s.pitch[0]};
    d.planeCount = s.planeCount;
    for(std::uint32_t p = 0; p < s.planeCount && p < 3; ++p)
      d.planes[p] = {s.offset[p], s.pitch[p]};
    descs.push_back(d);
  }

  auto strat = std::make_unique<score::gfx::interop::DmaBufImportCapture>(
      "Argus", std::move(descs),
      score::gfx::interop::DmaBufOrigin::GpuAllocated);
  if(m_wantExternal)
  {
    // DRM_FORMAT_NV12, spelled out to avoid a drm_fourcc.h dependency.
    constexpr std::uint32_t DRM_NV12 = 0x3231564eu;
    strat->requestExternalImage(DRM_NV12, height());
  }
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
  if(!m_session || !m_session->mapHost())
    return {};

  const auto& slots = mySlots();
  std::vector<score::gfx::interop::BorrowedHostBuffer> bufs;
  bufs.reserve(slots.size());
  for(const auto& s : slots)
  {
    if(!s.host)
      return {};
    score::gfx::interop::BorrowedHostBuffer b{s.host, std::size_t(s.totalBytes)};
    b.planeCount = s.planeCount;
    for(std::uint32_t p = 0; p < s.planeCount && p < 3; ++p)
      b.planes[p] = {s.offset[p], s.pitch[p]};
    bufs.push_back(b);
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
    QRhi::Implementation backend, const score::gfx::interop::GpuCapabilities&)
{
  m_dmabuf = nullptr;
  m_borrowed = nullptr;
  // Probed here because makeDecoder runs after this and must pick the matching
  // decoder; by then the QRhi backend and GL context are both known.
  m_wantExternal = score::gfx::nv12ExternalOesUsable(backend);
  if(m_wantExternal)
    qDebug() << "Argus: external-image NV12 import available";

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
  if(m_session)
    m_session->mapHost();
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

void ArgusInputBackend::publishUngrouped(std::size_t slot)
{
  if(slot >= mySlots().size())
    return;
  if(m_strategy)
  {
    // A borrowed rung takes ownership here; if it declines the frame (its ring
    // is full) the slot is left for the release drain, which is what stops a
    // stalled renderer from starving the ISP.
    if(!m_strategy->ingestFrame(slot))
      return;
  }
  m_ring.latestSlot.store(slot, std::memory_order_release);
  m_ring.latestFrameId.fetch_add(1, std::memory_order_release);
}

std::uint32_t ArgusInputBackend::takeReturnedUngrouped()
{
  if(m_dmabuf)
    return m_dmabuf->takeReturnedSlots();
  if(m_borrowed)
    return m_borrowed->takeReturnedSlots();
  // The CPU rung copies the frame out during acquireForRender, so the slot is
  // free again as soon as it has been published.
  const auto slotCount = mySlots().size();
  return slotCount >= 32u ? 0xFFFFFFFFu : ((1u << slotCount) - 1u);
}

void ArgusInputBackend::start()
{
  if(m_rig)
  {
    // The session belongs to the rig and is already running, or starts with
    // this member; either way the rig decides, because one session feeds every
    // sensor and cannot be started once per node.
    m_rig->arm(m_settings.syncMember, this);
    return;
  }

  if(!m_ownSession.isOpen())
    return;

  m_ownSession.start(
      // Capture thread: publish the slot the ISP just filled.
      [this](const std::size_t* slots, const std::uint64_t*, std::size_t n) {
        if(n == 1)
          publishUngrouped(slots[0]);
      },
      // Capture thread: which slots may go back to the ISP.
      [this](std::size_t) { return takeReturnedUngrouped(); });
}

void ArgusInputBackend::stop()
{
  if(m_rig)
  {
    m_rig->disarm(m_settings.syncMember);
    return;
  }
  m_ownSession.stop();
}

}
