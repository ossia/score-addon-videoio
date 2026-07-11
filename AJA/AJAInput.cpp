#include "AJAInput.hpp"

#include "AJACaptureSession.hpp"
#include "AJACpuCapture.hpp"
#include "AJAInputNode.hpp"

#include <Gfx/GfxApplicationPlugin.hpp>
#include <Gfx/GfxParameter.hpp>
#include <Gfx/Graph/VideoNode.hpp>

#include <Video/ExternalInput.hpp>
#include <Video/FrameQueue.hpp>

extern "C" {
#include <libavutil/buffer.h>
}

#include <array>

#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <score/serialization/MimeVisitor.hpp>

#include <ossia/network/generic/generic_device.hpp>
#include <ossia/network/generic/generic_node.hpp>

#include <QComboBox>
#include <QDebug>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>

#include <ntv2card.h>
#include <ntv2devicescanner.h>
#include <ntv2publicinterface.h>
#include <ntv2utils.h>

#include <wobjectimpl.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <atomic>
#include <thread>

W_OBJECT_IMPL(Gfx::AJA::AJAInputDevice)

namespace Gfx::AJA
{
namespace
{

/**
 * @brief AJA SDI capture worker for the CPU-staging path.
 *
 * Composes a `CaptureSession` for all the AJA card bookkeeping
 * (device open, AutoCirculate-channel setup, signal-change reroute,
 * VPID HDR detection, ANC + timecode metadata extraction) and adds
 * an AutoCirculate ring + AVFrame producer thread on top.
 *
 * Per-VBI loop:
 *   1. WaitForInputVerticalInterrupt
 *   2. detectAndApplyFormatChange every ~30 VBIs (cable swap / mode
 *      change). On change, AC is reinit'd in place.
 *   3. AutoCirculateGetStatus -> HasAvailableInputFrame?
 *   4. AutoCirculateTransfer DMAs SDI -> a fresh AVFrame from
 *      Video::FrameQueue::newFrame(). Buffer is page-locked at the
 *      driver level so 4K/8K bandwidths don't stall first-use.
 *   5. session.readPerFrameMetadata: extracts RP188 + ANC payloads,
 *      polls SDI link status periodically.
 *   6. Stamps the AVFrame with the session's current ImageFormat
 *      (color_space / primaries / trc / range) and enqueues.
 */
class aja_input_capture
{
public:
  explicit aja_input_capture(const AJAInputSettings& settings)
      : m_session{settings}
  {
  }

  ~aja_input_capture()
  {
    stop();
    if(m_acStarted)
    {
      if(auto* card = m_session.card())
        card->AutoCirculateStop(m_session.masterChannel());
      m_acStarted = false;
    }
    // Drop the pool's refs before close() unlocks the DMA regions. Any frame
    // still held by the renderer keeps its buffer alive via its own ref.
    releaseBufferPool();
    m_session.close();
  }

  bool open()
  {
    if(!m_session.open())
      return false;
    if(!m_session.setupChannel())
    {
      m_session.close();
      return false;
    }
    if(!initAutoCirculate())
    {
      m_session.close();
      return false;
    }
    return true;
  }

  // Accessors used by aja_input_decoder.
  int width() const noexcept { return m_session.width(); }
  int height() const noexcept { return m_session.height(); }
  AVPixelFormat pixelFormat() const noexcept
  {
    return m_session.avPixelFormat();
  }
  double fps() const noexcept { return m_session.fps(); }
  ::Video::FrameQueue& queue() noexcept { return m_queue; }

  /// Capture session — the device tree exposes a few of its atomic
  /// status fields (signalLocked, crcErrorCount, lastTimecode) as
  /// read-only parameters.
  CaptureSession& session() noexcept { return m_session; }

  void start()
  {
    if(m_running.exchange(true))
      return;
    m_thread = std::thread{[this] { runLoop(); }};
  }

  void stop()
  {
    if(!m_running.exchange(false))
      return;
    if(m_thread.joinable())
      m_thread.join();
    m_queue.drain();
  }

private:
  // Pool of page-locked DMA buffers, allocated and pinned once per format.
  // Avoids re-doing, every captured frame, a full-frame av_buffer_alloc/free
  // (>128 KB => glibc mmap/munmap churn) AND a DMABufferLock (a get_user_pages
  // pin over the whole frame) — the latter also leaked a stale locked region
  // per frame, since re-locking a fresh address never matched the previous one
  // and nothing unpinned until close(). Buffers are handed to frames by
  // refcount (av_buffer_ref), so av_frame_unref on recycle just drops the ref;
  // the pool keeps the last ref and never reallocs.
  bool ensureBufferPool(uint32_t frameSize)
  {
    if(m_bufPoolSize == frameSize && m_bufPool[0])
      return true;
    releaseBufferPool();
    auto* card = m_session.card();
    for(auto& b : m_bufPool)
    {
      b = av_buffer_alloc(frameSize);
      if(!b)
      {
        releaseBufferPool();
        return false;
      }
      if(card)
        card->DMABufferLock(
            reinterpret_cast<const ULWord*>(b->data),
            static_cast<ULWord>(frameSize), /*inMap=*/true, /*inRDMA=*/false);
    }
    m_bufPoolSize = frameSize;
    m_bufPoolNext = 0;
    return true;
  }

  void releaseBufferPool()
  {
    for(auto& b : m_bufPool)
      if(b)
        av_buffer_unref(&b); // drops the pool's ref; sets b = nullptr
    m_bufPoolSize = 0;
  }

  // Attach a free pinned buffer to the frame; null if every buffer is still
  // in flight (caller falls back to a one-off allocation). A buffer with
  // ref_count == 1 is held only by the pool, so it is safe to DMA into.
  uint8_t* attachPinnedBuffer(AVFrame& f, uint32_t frameSize)
  {
    if(!ensureBufferPool(frameSize))
      return nullptr;
    for(int k = 0; k < kBufPool; ++k)
    {
      AVBufferRef*& b = m_bufPool[(m_bufPoolNext + k) % kBufPool];
      if(av_buffer_get_ref_count(b) == 1)
      {
        m_bufPoolNext = (m_bufPoolNext + k + 1) % kBufPool;
        f.buf[0] = av_buffer_ref(b);
        f.data[0] = b->data;
        return b->data;
      }
    }
    return nullptr;
  }

  bool initAutoCirculate()
  {
    auto* card = m_session.card();
    if(!card)
      return false;
    auto ch = m_session.masterChannel();
    constexpr uint16_t kRingDepth = 7;
    card->AutoCirculateStop(ch);
    if(!card->AutoCirculateInitForInput(
           ch, kRingDepth, NTV2_AUDIOSYSTEM_INVALID,
           CaptureSession::kSuggestedAcOptions))
    {
      qWarning() << "AJA input: AutoCirculateInitForInput failed";
      return false;
    }
    if(!card->AutoCirculateStart(ch))
    {
      qWarning() << "AJA input: AutoCirculateStart failed";
      return false;
    }
    m_acStarted = true;
    return true;
  }

  void runLoop()
  {
    auto* card = m_session.card();
    auto ch = m_session.masterChannel();
    // Number of VBIs between signal-format polls. At 60fps this is
    // ~0.5s — fast enough to react to a cable swap, slow enough to
    // add ~zero per-frame overhead.
    constexpr int kFormatPollPeriod = 30;
    int sinceLastPoll = 0;

    while(m_running.load(std::memory_order_acquire))
    {
      card->WaitForInputVerticalInterrupt(ch);

      // Periodic signal-change check. On change, the session has
      // already done the rerouting; we re-init AutoCirculate against
      // the (possibly new) channel set and drain the queue so the
      // consumer doesn't keep stale-geometry frames.
      if(++sinceLastPoll >= kFormatPollPeriod)
      {
        sinceLastPoll = 0;
        if(m_session.detectAndApplyFormatChange())
        {
          if(m_acStarted)
          {
            card->AutoCirculateStop(ch);
            m_acStarted = false;
          }
          if(initAutoCirculate())
            m_queue.drain();
          continue;
        }
      }

      // Drain every complete frame this VBI slot. One transfer per VBI
      // cannot recover once a transfer overruns a frame period (UHD DMA +
      // enqueue > 16.7 ms occasionally): the ring level climbs until AC
      // overwrites, delivery caps near half rate with index gaps. Bounded
      // so a stall never spins here forever.
      for(int drained = 0; drained < 8; ++drained)
      {
      AUTOCIRCULATE_STATUS status;
      if(!card->AutoCirculateGetStatus(ch, status))
        break;
      if(!status.IsRunning() || !status.HasAvailableInputFrame())
        break;

      // Backpressure: if the renderer has fallen behind, drop the
      // oldest before enqueueing.
      if(m_queue.size() >= kMaxQueueDepth)
      {
        if(AVFrame* stale = m_queue.dequeue_one())
          m_queue.release(stale);
        m_drops.fetch_add(1, std::memory_order_relaxed);
      }

      auto avFrame = m_queue.newFrame();
      const uint32_t frameSize = m_session.frameSize();
      // Hand the frame a pre-allocated, pre-pinned pool buffer (no per-frame
      // alloc or page-pin). Falls back to a one-off UNPINNED buffer if the
      // pool is momentarily exhausted (renderer holding every buffer):
      // AutoCirculateTransfer works on unpinned memory, just slower for this
      // frame. Deliberately NOT DMABufferLock'ed — the buffer is freed with
      // the AVFrame while the driver pin would persist until close()'s
      // DMABufferUnlockAll, growing the kernel pin list with stale VAs on
      // every pool miss (the exact leak the pool was built to avoid).
      uint8_t* storage = attachPinnedBuffer(*avFrame, frameSize);
      if(!storage)
      {
        storage = ::Video::initFrameBuffer(*avFrame, frameSize);
        if(!storage)
          continue;
      }

      AUTOCIRCULATE_TRANSFER xfer;
      xfer.SetVideoBuffer(reinterpret_cast<ULWord*>(storage), frameSize);
      m_session.attachAncToTransfer(xfer);

      if(!card->AutoCirculateTransfer(ch, xfer))
      {
        m_xferFails.fetch_add(1, std::memory_order_relaxed);
        continue;
      }

      // Pull RP188, ANC HDR static metadata, SDI lock + CRC counters
      // off the freshly-DMA'd frame; updates session.lastTimecode and
      // session.signalLocked / session.crcErrorCount.
      m_session.readPerFrameMetadata(xfer);

      // Stamp the AVFrame with whatever color metadata the session
      // currently has — VPID-derived at setup, possibly refined by
      // ANC HDR detection on this very frame.
      const auto fmt = m_session.imageFormat();
      avFrame->format = m_session.avPixelFormat();
      avFrame->width = m_session.width();
      avFrame->height = m_session.height();
      avFrame->linesize[0]
          = (m_session.height() > 0)
                ? static_cast<int>(frameSize / m_session.height())
                : 0;
      avFrame->color_range = fmt.color_range;
      avFrame->color_primaries = fmt.color_primaries;
      avFrame->color_trc = fmt.color_trc;
      avFrame->colorspace = fmt.color_space;
      avFrame->best_effort_timestamp = 0;

      m_queue.enqueue(avFrame.release());
      m_goodXfers.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  CaptureSession m_session;

  ::Video::FrameQueue m_queue;
  std::thread m_thread;
  std::atomic<bool> m_running{false};
  bool m_acStarted{false};

  std::atomic<uint64_t> m_goodXfers{0};
  std::atomic<uint64_t> m_drops{0};
  std::atomic<uint64_t> m_xferFails{0};

  // Pinned DMA buffer pool. Size must exceed kMaxQueueDepth plus the frames
  // the renderer may hold, so a free buffer is essentially always available.
  static constexpr int kBufPool = 8;
  std::array<AVBufferRef*, kBufPool> m_bufPool{};
  uint32_t m_bufPoolSize{0};
  int m_bufPoolNext{0};

  // Bound on in-flight AVFrames. The AJA kernel ring already absorbs
  // capture-side back-pressure; the host queue only needs depth to
  // smooth out short renderer hiccups.
  static constexpr std::size_t kMaxQueueDepth = 4;
};

/**
 * @brief Video::ExternalInput wrapper: lets score's CameraNode
 *        pipeline consume the AJA capture's FrameQueue without
 *        knowing anything about AJA.
 */
class aja_input_decoder : public ::Video::ExternalInput
{
  std::shared_ptr<aja_input_capture> m_capture;

public:
  explicit aja_input_decoder(std::shared_ptr<aja_input_capture> cap)
      : m_capture{std::move(cap)}
  {
    this->width = m_capture->width();
    this->height = m_capture->height();
    this->pixel_format = m_capture->pixelFormat();
    this->fps = m_capture->fps();
    this->realTime = true;
    this->dts_per_flicks = 0;
    this->flicks_per_dts = 0;
  }

  ~aja_input_decoder() override = default;

  bool start() noexcept override
  {
    m_capture->start();
    return true;
  }
  void stop() noexcept override { m_capture->stop(); }

  AVFrame* dequeue_frame() noexcept override
  {
    return m_capture->queue().dequeue();
  }
  void release_frame(AVFrame* f) noexcept override
  {
    m_capture->queue().release(f);
  }
};

} // namespace

std::shared_ptr<::Video::ExternalInput> makeAJACapture(const AJAInputSettings& s)
{
  auto cap = std::make_shared<aja_input_capture>(s);
  if(!cap->open())
    return nullptr;
  return std::make_shared<aja_input_decoder>(std::move(cap));
}

// =============================================================================
// AJAInputProtocolFactory
// =============================================================================

QString AJAInputProtocolFactory::prettyName() const noexcept
{
  return QObject::tr("AJA SDI Input");
}

QString AJAInputProtocolFactory::category() const noexcept
{
  return StandardCategories::video_in;
}

QUrl AJAInputProtocolFactory::manual() const noexcept
{
  return QUrl("https://ossia.io/score-docs/devices/aja-device.html");
}

Device::DeviceEnumerators AJAInputProtocolFactory::getEnumerators(
    const score::DocumentContext&) const
{
  return {{"Devices", new AJAInputDeviceEnumerator}};
}

Device::DeviceInterface* AJAInputProtocolFactory::makeDevice(
    const Device::DeviceSettings& settings,
    const Explorer::DeviceDocumentPlugin&,
    const score::DocumentContext& ctx)
{
  return new AJAInputDevice{settings, ctx};
}

const Device::DeviceSettings&
AJAInputProtocolFactory::defaultSettings() const noexcept
{
  static Device::DeviceSettings settings;
  if(settings.name.isEmpty())
  {
    settings.name = "AJA Input";
    AJAInputSettings aja;
    aja.deviceName = "AJA Input";
    aja.videoFormat = "1080p5994";
    aja.pixelFormat = AJAInputPixelFormat::YCbCr8;
    settings.deviceSpecificSettings = QVariant::fromValue(aja);
  }
  return settings;
}

Device::ProtocolSettingsWidget* AJAInputProtocolFactory::makeSettingsWidget()
{
  return new AJAInputSettingsWidget;
}

Device::AddressDialog* AJAInputProtocolFactory::makeAddAddressDialog(
    const Device::DeviceInterface&, const score::DocumentContext&, QWidget*)
{
  return nullptr;
}

Device::AddressDialog* AJAInputProtocolFactory::makeEditAddressDialog(
    const Device::AddressSettings&, const Device::DeviceInterface&,
    const score::DocumentContext&, QWidget*)
{
  return nullptr;
}

QVariant AJAInputProtocolFactory::makeProtocolSpecificSettings(
    const VisitorVariant& visitor) const
{
  return makeProtocolSpecificSettings_T<AJAInputSettings>(visitor);
}

void AJAInputProtocolFactory::serializeProtocolSpecificSettings(
    const QVariant& data, const VisitorVariant& visitor) const
{
  serializeProtocolSpecificSettings_T<AJAInputSettings>(data, visitor);
}

bool AJAInputProtocolFactory::checkCompatibility(
    const Device::DeviceSettings& a, const Device::DeviceSettings& b) const noexcept
{
  auto a_aja = a.deviceSpecificSettings.value<AJAInputSettings>();
  auto b_aja = b.deviceSpecificSettings.value<AJAInputSettings>();
  return a_aja.deviceIndex == b_aja.deviceIndex
         && a_aja.channelIndex == b_aja.channelIndex;
}

// =============================================================================
// AJAInputDevice
// =============================================================================

AJAInputDevice::~AJAInputDevice() = default;

void AJAInputDevice::disconnect()
{
  Gfx::GfxInputDevice::disconnect();
  auto prev = std::move(m_dev);
  m_dev = {};
  deviceChanged(prev.get(), nullptr);
}

bool AJAInputDevice::reconnect()
{
  disconnect();

  try
  {
    auto set = settings().deviceSpecificSettings.value<AJAInputSettings>();
    auto plug = m_ctx.findPlugin<Gfx::DocumentPlugin>();
    if(plug)
    {
      // GPU-direct path: AJA -> sysmem -> DVP DMA -> QRhi texture.
      // Uses score's simple_texture_input_device — same plumbing
      // Spout/Syphon use to expose a foreign-owned QRhi texture as a
      // gfx graph source. Renderer opens the AJA card; if the strategy
      // can't init the user sees a warning + black texture and can
      // flip useRDMA off in settings to force AVFrame staging.
      if(set.useRDMA)
      {
        auto* node = new AJAInputNode{set};
        m_dev = std::make_unique<Gfx::simple_texture_input_device>(
            node, &plug->exec,
            std::make_unique<Gfx::simple_texture_input_protocol>(),
            settings().name.toStdString());
        m_protocol = nullptr;
        deviceChanged(nullptr, m_dev.get());
        qDebug() << "AJA input: registered GPU-direct AJAInputNode";
        return connected();
      }

      // CPU-staging fallback path: AJA -> AVFrame -> GPUVideoDecoder
      // upload. Card is opened up front to fail fast on missing devices.
      if(set.pixelFormat == AJAInputPixelFormat::YCbCr10)
      {
        qWarning() << "AJA input: v210 (10-bit YCbCr) is only supported via "
                      "the GPU-direct path; enable useRDMA or pick another "
                      "pixel format.";
        return false;
      }
      auto cap = std::make_shared<aja_input_capture>(set);
      if(!cap->open())
      {
        qWarning() << "AJA input: open() failed - device unavailable";
        return false;
      }
      std::shared_ptr<::Video::ExternalInput> dec
          = std::make_shared<aja_input_decoder>(std::move(cap));
      m_protocol
          = new Gfx::video_texture_input_protocol{std::move(dec), plug->exec};
      m_dev = std::make_unique<Gfx::video_texture_input_device>(
          std::unique_ptr<ossia::net::protocol_base>(m_protocol),
          settings().name.toStdString());
      deviceChanged(nullptr, m_dev.get());
      qDebug() << "AJA input: registered AVFrame CPU-staging path";
    }
  }
  catch(std::exception& e)
  {
    qDebug() << "AJA input: reconnect failed:" << e.what();
  }
  catch(...)
  {
  }
  return connected();
}

// =============================================================================
// AJAInputSettingsWidget
// =============================================================================

AJAInputSettingsWidget::AJAInputSettingsWidget(QWidget* parent)
    : Device::ProtocolSettingsWidget{parent}
{
  m_layout = new QFormLayout{this};

  m_deviceNameEdit = new State::AddressFragmentLineEdit{this};
  m_deviceNameEdit->setText("AJA Input");
  m_layout->addRow(tr("Name"), m_deviceNameEdit);

  m_deviceCombo = new QComboBox{this};
  m_layout->addRow(tr("Device"), m_deviceCombo);

  m_channelCombo = new QComboBox{this};
  for(int i = 1; i <= 8; ++i)
    m_channelCombo->addItem(QString("Channel %1").arg(i), i - 1);
  m_layout->addRow(tr("Channel"), m_channelCombo);

  m_formatCombo = new QComboBox{this};
  for(const auto& f : {"720p5994",   "720p60",    "1080p2398", "1080p24",
                       "1080p25",    "1080p2997", "1080p30",   "1080p50",
                       "1080p5994",  "1080p60",   "1080i50",   "1080i5994",
                       "1080i60",    "2160p25",   "2160p2997", "2160p30",
                       "2160p50",    "2160p5994", "2160p60"})
    m_formatCombo->addItem(QString::fromLatin1(f));
  m_formatCombo->setCurrentText("1080p5994");
  m_layout->addRow(tr("Expected format"), m_formatCombo);

  m_pixelFormatCombo = new QComboBox{this};
  m_pixelFormatCombo->addItem(
      "YCbCr 8-bit (UYVY)", int(AJAInputPixelFormat::YCbCr8));
  m_pixelFormatCombo->addItem(
      "YCbCr 10-bit (v210, GPU-direct only)",
      int(AJAInputPixelFormat::YCbCr10));
  m_pixelFormatCombo->addItem(
      "ARGB (32-bit BGRA)", int(AJAInputPixelFormat::ARGB));
  m_pixelFormatCombo->addItem(
      "RGBA (32-bit RGBA)", int(AJAInputPixelFormat::RGBA));
  m_layout->addRow(tr("Pixel format"), m_pixelFormatCombo);

  m_resolutionModeCombo = new QComboBox{this};
  m_resolutionModeCombo->addItem(
      tr("Auto (single-link / 4K / 8K detected)"),
      int(AJAInputResolutionMode::Auto));
  m_resolutionModeCombo->addItem(
      tr("Single-link (1080p / 4K via 12G)"),
      int(AJAInputResolutionMode::SingleLink));
  m_resolutionModeCombo->addItem(
      tr("Quad-link 4K (4 SDI inputs)"),
      int(AJAInputResolutionMode::Quad4K));
  m_resolutionModeCombo->addItem(
      tr("Quad-link 8K (4 SDI inputs)"),
      int(AJAInputResolutionMode::Quad8K));
  m_layout->addRow(tr("Resolution mode"), m_resolutionModeCombo);

  m_routingModeCombo = new QComboBox{this};
  m_routingModeCombo->addItem(
      tr("SQD — Square Division (one quadrant per frame store)"),
      int(AJAInputRoutingMode::SQD));
  m_routingModeCombo->addItem(
      tr("TSI — Two-Sample Interleave (sample-interleaved across muxers)"),
      int(AJAInputRoutingMode::TSI));
  m_layout->addRow(tr("Quad routing"), m_routingModeCombo);

  refreshDeviceList();
}

void AJAInputSettingsWidget::refreshDeviceList()
{
  m_deviceCombo->clear();
  CNTV2DeviceScanner scanner;
  for(unsigned i = 0; i < scanner.GetNumDevices(); ++i)
  {
    NTV2DeviceInfo info;
    if(scanner.GetDeviceInfo(i, info))
    {
      m_deviceCombo->addItem(
          QString::fromStdString(info.deviceIdentifier),
          static_cast<int>(i));
    }
  }
  if(m_deviceCombo->count() == 0)
    m_deviceCombo->addItem(tr("(no AJA device detected)"), -1);
}

void AJAInputSettingsWidget::updateChannelList(int) { }

Device::DeviceSettings AJAInputSettingsWidget::getSettings() const
{
  Device::DeviceSettings s;
  s.name = m_deviceNameEdit->text();
  s.protocol = AJAInputProtocolFactory::static_concreteKey();
  AJAInputSettings aja;
  aja.deviceName = s.name;
  aja.deviceIndex = std::max(0, m_deviceCombo->currentData().toInt());
  aja.channelIndex = m_channelCombo->currentData().toInt();
  aja.videoFormat = m_formatCombo->currentText();
  aja.pixelFormat = static_cast<AJAInputPixelFormat>(
      m_pixelFormatCombo->currentData().toInt());
  aja.resolutionMode = static_cast<AJAInputResolutionMode>(
      m_resolutionModeCombo->currentData().toInt());
  aja.routingMode = static_cast<AJAInputRoutingMode>(
      m_routingModeCombo->currentData().toInt());
  s.deviceSpecificSettings = QVariant::fromValue(aja);
  return s;
}

void AJAInputSettingsWidget::setSettings(const Device::DeviceSettings& s)
{
  m_deviceNameEdit->setText(s.name);
  AJAInputSettings aja = s.deviceSpecificSettings.value<AJAInputSettings>();
  if(aja.deviceIndex >= 0 && aja.deviceIndex < m_deviceCombo->count())
    m_deviceCombo->setCurrentIndex(aja.deviceIndex);
  for(int i = 0; i < m_channelCombo->count(); ++i)
    if(m_channelCombo->itemData(i).toInt() == aja.channelIndex)
      m_channelCombo->setCurrentIndex(i);
  m_formatCombo->setCurrentText(aja.videoFormat);
  for(int i = 0; i < m_pixelFormatCombo->count(); ++i)
    if(m_pixelFormatCombo->itemData(i).toInt() == int(aja.pixelFormat))
      m_pixelFormatCombo->setCurrentIndex(i);
  for(int i = 0; i < m_resolutionModeCombo->count(); ++i)
    if(m_resolutionModeCombo->itemData(i).toInt() == int(aja.resolutionMode))
      m_resolutionModeCombo->setCurrentIndex(i);
  for(int i = 0; i < m_routingModeCombo->count(); ++i)
    if(m_routingModeCombo->itemData(i).toInt() == int(aja.routingMode))
      m_routingModeCombo->setCurrentIndex(i);
}

// =============================================================================
// AJAInputDeviceEnumerator
// =============================================================================

void AJAInputDeviceEnumerator::enumerate(
    std::function<void(const QString&, const Device::DeviceSettings&)> func) const
{
  CNTV2DeviceScanner scanner;
  for(unsigned i = 0; i < scanner.GetNumDevices(); ++i)
  {
    NTV2DeviceInfo info;
    if(!scanner.GetDeviceInfo(i, info))
      continue;
    Device::DeviceSettings s;
    s.name = QString::fromStdString(info.deviceIdentifier);
    s.protocol = AJAInputProtocolFactory::static_concreteKey();
    AJAInputSettings aja;
    aja.deviceName = s.name;
    aja.deviceIndex = static_cast<int>(i);
    aja.channelIndex = 0;
    aja.videoFormat = "1080p5994";
    aja.pixelFormat = AJAInputPixelFormat::YCbCr8;
    s.deviceSpecificSettings = QVariant::fromValue(aja);
    func(s.name, s);
  }
}

} // namespace Gfx::AJA

// =============================================================================
// Serialization
// =============================================================================

template <>
void DataStreamReader::read(const Gfx::AJA::AJAInputSettings& n)
{
  m_stream << n.deviceName << n.deviceIndex << n.channelIndex << n.videoFormat
           << static_cast<int>(n.pixelFormat)
           << static_cast<int>(n.resolutionMode)
           << static_cast<int>(n.routingMode) << n.useRDMA;
  insertDelimiter();
}

template <>
void DataStreamWriter::write(Gfx::AJA::AJAInputSettings& n)
{
  int pf = 0, rm = 0, ro = 0;
  m_stream >> n.deviceName >> n.deviceIndex >> n.channelIndex >> n.videoFormat
      >> pf >> rm >> ro >> n.useRDMA;
  n.pixelFormat = static_cast<Gfx::AJA::AJAInputPixelFormat>(pf);
  n.resolutionMode = static_cast<Gfx::AJA::AJAInputResolutionMode>(rm);
  n.routingMode = static_cast<Gfx::AJA::AJAInputRoutingMode>(ro);
  checkDelimiter();
}

template <>
void JSONReader::read(const Gfx::AJA::AJAInputSettings& n)
{
  obj["DeviceName"] = n.deviceName;
  obj["DeviceIndex"] = n.deviceIndex;
  obj["ChannelIndex"] = n.channelIndex;
  obj["VideoFormat"] = n.videoFormat;
  obj["PixelFormat"] = static_cast<int>(n.pixelFormat);
  obj["ResolutionMode"] = static_cast<int>(n.resolutionMode);
  obj["RoutingMode"] = static_cast<int>(n.routingMode);
  obj["UseRDMA"] = n.useRDMA;
}

template <>
void JSONWriter::write(Gfx::AJA::AJAInputSettings& n)
{
  n.deviceName = obj["DeviceName"].toString();
  n.deviceIndex = obj["DeviceIndex"].toInt();
  n.channelIndex = obj["ChannelIndex"].toInt();
  n.videoFormat = obj["VideoFormat"].toString();
  n.pixelFormat
      = static_cast<Gfx::AJA::AJAInputPixelFormat>(obj["PixelFormat"].toInt());
  n.resolutionMode = static_cast<Gfx::AJA::AJAInputResolutionMode>(
      obj["ResolutionMode"].toInt());
  n.routingMode
      = static_cast<Gfx::AJA::AJAInputRoutingMode>(obj["RoutingMode"].toInt());
  n.useRDMA = obj["UseRDMA"].toBool();
}

SCORE_SERALIZE_DATASTREAM_DEFINE(Gfx::AJA::AJAInputSettings);
