#include "VideoInput.hpp"

#if defined(SCORE_HAS_V4L2)
#include <linux/videodev2.h>
#endif

#include <Gfx/GfxApplicationPlugin.hpp>
#include <Gfx/GfxParameter.hpp>

#include <Video/ExternalInput.hpp>

#include <score/serialization/MimeVisitor.hpp>

#include <VideoIOCaps.hpp>

#include <ossia/network/generic/generic_device.hpp>

#include <QCheckBox>
#include <QComboBox>

#include <algorithm>
#include <QDebug>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>

#include <wobjectimpl.h>

#include <memory>

#if defined(SCORE_HAS_AJA)
#include <AJA/AJAInput.hpp>
#include <AJA/AJAInputNode.hpp>

#include <ntv2card.h>
#include <ntv2devicescanner.h>
#endif

#if defined(SCORE_HAS_DECKLINK)
#include <decklink/DeckLinkCaptureNode.hpp>
#include <decklink/DeckLinkDevices.hpp>
#include <decklink/DeckLinkModeMap.hpp>
#endif

#if defined(SCORE_HAS_DELTACAST)
#include <deltacast/DeltacastCaptureNode.hpp>
#include <deltacast/DeltacastDevices.hpp>
#include <deltacast/DeltacastFormats.hpp>
#endif

#if defined(SCORE_HAS_BLUEFISH)
#include <bluefish/BluefishCaptureNode.hpp>
#include <bluefish/BluefishDevices.hpp>
#include <bluefish/BluefishSettings.hpp>
#endif

#if defined(SCORE_HAS_V4L2)
#include <v4l2/V4L2CaptureNode.hpp>
#include <v4l2/V4L2Session.hpp>
#endif

#if defined(SCORE_HAS_ARGUS)
#include <argus/ArgusCaptureNode.hpp>
#include <argus/ArgusRuntime.hpp>
#endif

#if defined(SCORE_HAS_MAGEWELL)
#include <magewell/MagewellCaptureNode.hpp>
#include <magewell/MagewellDevices.hpp>
#include <magewell/MagewellFormats.hpp>
#endif

W_OBJECT_IMPL(Gfx::VideoIO::VideoInputDevice)

namespace Gfx::VideoIO
{
namespace
{

#if defined(SCORE_HAS_AJA)
Gfx::AJA::AJAInputPixelFormat ajaInputPixel(const QString& p) noexcept
{
  if(p == "YCbCr10") return Gfx::AJA::AJAInputPixelFormat::YCbCr10;
  if(p == "ARGB")    return Gfx::AJA::AJAInputPixelFormat::ARGB;
  if(p == "RGBA")    return Gfx::AJA::AJAInputPixelFormat::RGBA;
  return Gfx::AJA::AJAInputPixelFormat::YCbCr8;
}

Gfx::AJA::AJAInputSettings toAjaInput(const VideoInputSettings& s)
{
  Gfx::AJA::AJAInputSettings a;
  a.deviceName = s.deviceName;
  a.deviceIndex = s.deviceIndex;
  a.channelIndex = s.channelIndex;
  a.videoFormat = s.videoFormat;
  a.pixelFormat = ajaInputPixel(s.pixelFormat);
  a.resolutionMode = static_cast<Gfx::AJA::AJAInputResolutionMode>(s.resolutionMode);
  a.routingMode = static_cast<Gfx::AJA::AJAInputRoutingMode>(s.routingMode);
  a.useRDMA = s.useRDMA;
  return a;
}
#endif

#if defined(SCORE_HAS_DECKLINK)
Gfx::DeckLink::DeckLinkInputSettings toDeckLinkInput(const VideoInputSettings& s)
{
  Gfx::DeckLink::DeckLinkInputSettings d;
  d.deviceIndex = s.deviceIndex;
  d.autoDetect = (s.videoFormat == "Auto");
  d.displayMode = Gfx::DeckLink::bmdModeFromToken(s.videoFormat);
  d.pixelFormat = Gfx::DeckLink::bmdPixelFromToken(s.pixelFormat);
  return d;
}
#endif

#if defined(SCORE_HAS_DELTACAST)
Gfx::Deltacast::DeltacastInputSettings toDeltacastInput(const VideoInputSettings& s)
{
  Gfx::Deltacast::DeltacastInputSettings d;
  d.deviceIndex = s.deviceIndex;
  // 0 = auto-detect the incoming standard (the RX signal drives the geometry);
  // the widget's "expected format" is only a UI hint for capture.
  d.videoStandard = 0;
  d.bufferPacking = Gfx::Deltacast::vhdPackingFromToken(s.pixelFormat);
  d.useRDMA = s.useRDMA; // user-visible switch to force the host-staged path
  return d;
}
#endif

#if defined(SCORE_HAS_BLUEFISH)
Gfx::Bluefish::BluefishInputSettings toBluefishInput(const VideoInputSettings& s)
{
  Gfx::Bluefish::BluefishInputSettings b;
  b.deviceIndex = std::max(1, s.deviceIndex); // Bluefish ids are 1-based
  // The incoming video mode is auto-detected (bfcUtilsGetRecommendedSetupInfoInput);
  // the widget's "expected format" is only a UI hint.
  b.memoryFormat = Gfx::Bluefish::memFmtFromToken(s.pixelFormat);
  return b;
}
#endif

#if defined(SCORE_HAS_MAGEWELL)
Gfx::Magewell::MagewellInputSettings toMagewellInput(const VideoInputSettings& s)
{
  Gfx::Magewell::MagewellInputSettings m;
  m.deviceIndex = s.deviceIndex;
  // Magewell auto-detects the incoming signal (geometry comes from
  // MWGetVideoSignalStatus), so videoFormat is ignored; only the FOURCC matters.
  m.fourcc = Gfx::Magewell::fourccFromToken(s.pixelFormat);
  return m;
}
#endif

} // namespace

// =============================================================================
// VideoInputProtocolFactory
// =============================================================================

QString VideoInputProtocolFactory::prettyName() const noexcept
{
  return QObject::tr("Direct Video Input");
}

QString VideoInputProtocolFactory::category() const noexcept
{
  return StandardCategories::video_in;
}

QUrl VideoInputProtocolFactory::manual() const noexcept
{
  return QUrl("https://ossia.io/score-docs/devices/aja-device.html");
}

Device::DeviceInterface* VideoInputProtocolFactory::makeDevice(
    const Device::DeviceSettings& settings, const Explorer::DeviceDocumentPlugin&,
    const score::DocumentContext& ctx)
{
  return new VideoInputDevice{settings, ctx};
}

const Device::DeviceSettings&
VideoInputProtocolFactory::defaultSettings() const noexcept
{
  static Device::DeviceSettings settings;
  if(settings.name.isEmpty())
  {
    settings.name = "Direct Video In";
    VideoInputSettings set;
    set.deviceName = "Direct Video In";
    settings.deviceSpecificSettings = QVariant::fromValue(set);
  }
  return settings;
}

Device::ProtocolSettingsWidget* VideoInputProtocolFactory::makeSettingsWidget()
{
  return new VideoInputSettingsWidget;
}

Device::AddressDialog* VideoInputProtocolFactory::makeAddAddressDialog(
    const Device::DeviceInterface&, const score::DocumentContext&, QWidget*)
{
  return nullptr;
}

Device::AddressDialog* VideoInputProtocolFactory::makeEditAddressDialog(
    const Device::AddressSettings&, const Device::DeviceInterface&,
    const score::DocumentContext&, QWidget*)
{
  return nullptr;
}

QVariant VideoInputProtocolFactory::makeProtocolSpecificSettings(
    const VisitorVariant& visitor) const
{
  return makeProtocolSpecificSettings_T<VideoInputSettings>(visitor);
}

void VideoInputProtocolFactory::serializeProtocolSpecificSettings(
    const QVariant& data, const VisitorVariant& visitor) const
{
  serializeProtocolSpecificSettings_T<VideoInputSettings>(data, visitor);
}

bool VideoInputProtocolFactory::checkCompatibility(
    const Device::DeviceSettings& a, const Device::DeviceSettings& b) const noexcept
{
  // "Can a and b coexist?" — the device explorer also probes against an empty
  // DeviceSettings{} (null protocol), which must always be compatible.
  if(a.protocol != b.protocol)
    return true;
  auto sa = a.deviceSpecificSettings.value<VideoInputSettings>();
  auto sb = b.deviceSpecificSettings.value<VideoInputSettings>();
  // Only one input device may own a given physical card at a time (the
  // backends acquire the whole board, e.g. AcquireStreamForApplication).
  return !(sa.vendor == sb.vendor && sa.deviceIndex == sb.deviceIndex);
}

// =============================================================================
// VideoInputDevice
// =============================================================================

VideoInputDevice::~VideoInputDevice() = default;

void VideoInputDevice::disconnect()
{
  Gfx::GfxInputDevice::disconnect();
  auto prev = std::move(m_dev);
  m_dev = {};
  deviceChanged(prev.get(), nullptr);
}

bool VideoInputDevice::reconnect()
{
  disconnect();

  try
  {
    auto set = settings().deviceSpecificSettings.value<VideoInputSettings>();
    auto plug = m_ctx.findPlugin<Gfx::DocumentPlugin>();
    if(!plug)
      return connected();

    // GPU-direct path: vendor node + simple_texture_input_device (the Spout/
    // Syphon plumbing for a foreign-owned QRhi texture).
    auto registerGpuDirect = [&](score::gfx::Node* node) {
      m_dev = std::make_unique<Gfx::simple_texture_input_device>(
          node, &plug->exec,
          std::make_unique<Gfx::simple_texture_input_protocol>(),
          settings().name.toStdString());
      m_protocol = nullptr;
      deviceChanged(nullptr, m_dev.get());
    };

    switch(set.vendor)
    {
      case Vendor::AJA:
      {
#if defined(SCORE_HAS_AJA)
        if(set.useRDMA)
        {
          registerGpuDirect(new Gfx::AJA::AJAInputNode{toAjaInput(set)});
          qDebug() << "Direct Video Input: AJA GPU-direct node";
          return connected();
        }
        // CPU-staging fallback: AJA -> AVFrame -> GPUVideoDecoder upload.
        if(set.pixelFormat == "YCbCr10")
        {
          qWarning() << "Direct Video Input: v210 (10-bit YCbCr) needs the "
                        "GPU-direct path; enable RDMA or pick another format.";
          return false;
        }
        auto dec = Gfx::AJA::makeAJACapture(toAjaInput(set));
        if(!dec)
        {
          qWarning() << "Direct Video Input: AJA open() failed";
          return false;
        }
        m_protocol
            = new Gfx::video_texture_input_protocol{std::move(dec), plug->exec};
        m_dev = std::make_unique<Gfx::video_texture_input_device>(
            std::unique_ptr<ossia::net::protocol_base>(m_protocol),
            settings().name.toStdString());
        deviceChanged(nullptr, m_dev.get());
        qDebug() << "Direct Video Input: AJA CPU-staging path";
#else
        qDebug() << "Direct Video Input: AJA not compiled in";
        return false;
#endif
        break;
      }
      case Vendor::DeckLink:
      {
#if defined(SCORE_HAS_DECKLINK)
        registerGpuDirect(new Gfx::DeckLink::DeckLinkCaptureNode{toDeckLinkInput(set)});
        qDebug() << "Direct Video Input: DeckLink host-staged node";
        return connected();
#else
        qDebug() << "Direct Video Input: DeckLink not compiled in";
        return false;
#endif
        break;
      }
      case Vendor::Deltacast:
      {
#if defined(SCORE_HAS_DELTACAST)
        registerGpuDirect(
            new Gfx::Deltacast::DeltacastCaptureNode{toDeltacastInput(set)});
        qDebug() << "Direct Video Input: DELTACAST host-staged node";
        return connected();
#else
        qDebug() << "Direct Video Input: DELTACAST not compiled in";
        return false;
#endif
        break;
      }
      case Vendor::Bluefish:
      {
#if defined(SCORE_HAS_BLUEFISH)
        registerGpuDirect(
            new Gfx::Bluefish::BluefishCaptureNode{toBluefishInput(set)});
        qDebug() << "Direct Video Input: Bluefish444 host-staged node";
        return connected();
#else
        qDebug() << "Direct Video Input: Bluefish444 not compiled in";
        return false;
#endif
        break;
      }
      case Vendor::V4L2:
      {
#if defined(SCORE_HAS_V4L2)
        Gfx::V4L2::V4L2InputSettings v4l2;
        // devicePath is the node; deviceName is what the user called this
        // device in score and has never been a path. Opening the latter meant
        // open("vi-output, imx676 9-001a (/dev/video0)") -> ENOENT, and the
        // capture silently never started.
        v4l2.device = set.devicePath.isEmpty()
                          ? ("/dev/video" + std::to_string(set.deviceIndex))
                          : set.devicePath.toStdString();

        // Geometry and pixel format were being dropped here, so the node always
        // took whatever the device happened to be configured for and the two
        // settings fields did nothing on this vendor. A left-empty field still
        // means "keep the driver's current setting" -- V4L2Session only writes
        // the members that are non-zero.
        //
        // VideoFormat is a "<width>x<height>" string for V4L2 (the SDI vendors'
        // mode names have no meaning for a webcam), and PixelFormat is a fourcc
        // spelled out, e.g. "NV12".
        if(const auto geom = set.videoFormat.split('x'); geom.size() == 2)
        {
          bool okW = false, okH = false;
          const auto w = geom[0].trimmed().toUInt(&okW);
          const auto h = geom[1].trimmed().toUInt(&okH);
          if(okW && okH)
          {
            v4l2.width = w;
            v4l2.height = h;
          }
        }
        if(const auto pf = set.pixelFormat.trimmed().toLatin1(); pf.size() == 4)
        {
          v4l2.fourcc = v4l2_fourcc(
              std::uint8_t(pf[0]), std::uint8_t(pf[1]), std::uint8_t(pf[2]),
              std::uint8_t(pf[3]));
        }
        registerGpuDirect(new Gfx::V4L2::V4L2CaptureNode{v4l2});
        qDebug() << "Direct Video Input: V4L2 host-staged node";
        return connected();
#else
        qDebug() << "Direct Video Input: V4L2 not compiled in";
        return false;
#endif
        break;
      }
      case Vendor::Argus:
      {
#if defined(SCORE_HAS_ARGUS)
        Gfx::Argus::ArgusSettings a;
        a.sensorId = std::uint32_t(std::max(0, set.deviceIndex));
        // sensorMode -1 = "best match", the same default the GStreamer element
        // uses; ResolutionMode carries it so no new settings field is needed.
        a.sensorMode = set.resolutionMode > 0 ? (set.resolutionMode - 1) : -1;
        if(const auto geom = set.videoFormat.split('x'); geom.size() == 2)
        {
          bool okW = false, okH = false;
          const auto w = geom[0].trimmed().toUInt(&okW);
          const auto h = geom[1].trimmed().toUInt(&okH);
          if(okW && okH)
          {
            a.width = w;
            a.height = h;
          }
        }
        registerGpuDirect(new Gfx::Argus::ArgusCaptureNode{a});
        qDebug() << "Direct Video Input: Argus camera node";
        return connected();
#else
        qDebug() << "Direct Video Input: Argus not compiled in";
        return false;
#endif
      }
      case Vendor::Magewell:
      {
#if defined(SCORE_HAS_MAGEWELL)
        registerGpuDirect(
            new Gfx::Magewell::MagewellCaptureNode{toMagewellInput(set)});
        qDebug() << "Direct Video Input: Magewell host-staged node";
        return connected();
#else
        qDebug() << "Direct Video Input: Magewell not compiled in";
        return false;
#endif
        break;
      }
    }
  }
  catch(std::exception& e)
  {
    qDebug() << "Direct Video Input: reconnect failed:" << e.what();
  }
  catch(...)
  {
  }
  return connected();
}

// =============================================================================
// VideoInputSettingsWidget
// =============================================================================

VideoInputSettingsWidget::VideoInputSettingsWidget(QWidget* parent)
    : Device::ProtocolSettingsWidget{parent}
{
  m_layout = new QFormLayout{this};

  m_deviceNameEdit = new QLineEdit{this};
  m_deviceNameEdit->setText("Direct Video In");
  m_layout->addRow(tr("Name"), m_deviceNameEdit);
  this->checkForChanges(m_deviceNameEdit);

  m_vendorCombo = new QComboBox{this};
#if defined(SCORE_HAS_AJA)
  m_vendorCombo->addItem("AJA", static_cast<int>(Vendor::AJA));
#endif
#if defined(SCORE_HAS_DECKLINK)
  m_vendorCombo->addItem("Blackmagic DeckLink", static_cast<int>(Vendor::DeckLink));
#endif
#if defined(SCORE_HAS_DELTACAST)
  m_vendorCombo->addItem("DELTACAST", static_cast<int>(Vendor::Deltacast));
#endif
#if defined(SCORE_HAS_BLUEFISH)
  m_vendorCombo->addItem("Bluefish444", static_cast<int>(Vendor::Bluefish));
#endif
#if defined(SCORE_HAS_V4L2)
  m_vendorCombo->addItem("V4L2 (Linux)", static_cast<int>(Vendor::V4L2));
#endif
#if defined(SCORE_HAS_MAGEWELL)
  m_vendorCombo->addItem("Magewell", static_cast<int>(Vendor::Magewell));
#endif
  m_layout->addRow(tr("Vendor"), m_vendorCombo);

  m_deviceCombo = new QComboBox{this};
  m_layout->addRow(tr("Device"), m_deviceCombo);

  m_channelCombo = new QComboBox{this};
  for(int i = 1; i <= 8; ++i)
    m_channelCombo->addItem(QString("Channel %1").arg(i), i - 1);
  m_layout->addRow(tr("Channel"), m_channelCombo);

  m_formatCombo = new QComboBox{this};
  m_layout->addRow(tr("Expected format"), m_formatCombo);

  m_pixelFormatCombo = new QComboBox{this};
  m_layout->addRow(tr("Pixel format"), m_pixelFormatCombo);

  m_resolutionModeCombo = new QComboBox{this};
  m_resolutionModeCombo->addItem(tr("Auto (single-link / 4K / 8K detected)"), 0);
  m_resolutionModeCombo->addItem(tr("Single-link (1080p / 4K via 12G)"), 1);
  m_resolutionModeCombo->addItem(tr("Quad-link 4K (4 SDI inputs)"), 2);
  m_resolutionModeCombo->addItem(tr("Quad-link 8K (4 SDI inputs)"), 3);
  m_layout->addRow(tr("Resolution mode"), m_resolutionModeCombo);

  m_routingModeCombo = new QComboBox{this};
  m_routingModeCombo->addItem(tr("SQD — Square Division"), 0);
  m_routingModeCombo->addItem(tr("TSI — Two-Sample Interleave"), 1);
  m_layout->addRow(tr("Quad routing"), m_routingModeCombo);

  m_rdmaCheckbox = new QCheckBox{tr("GPU-direct (RDMA / DVP) when available"), this};
  m_rdmaCheckbox->setChecked(true);
  m_layout->addRow(m_rdmaCheckbox);

  connect(
      m_vendorCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
      [this](int) { onVendorChanged(); });
  connect(
      m_deviceCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
      [this](int) { updateFormatList(); });

  onVendorChanged();
}

Vendor VideoInputSettingsWidget::currentVendor() const
{
  return static_cast<Vendor>(m_vendorCombo->currentData().toInt());
}

void VideoInputSettingsWidget::onVendorChanged()
{
  const bool isAja = currentVendor() == Vendor::AJA;

  auto setRow = [this](QWidget* w, bool vis) {
    if(auto* l = m_layout->labelForField(w))
      l->setVisible(vis);
    w->setVisible(vis);
  };
  setRow(m_resolutionModeCombo, isAja);
  setRow(m_routingModeCombo, isAja);
  // Only AJA plumbs channelIndex through to its backend; on the other vendors
  // the selector would silently do nothing (their input settings carry no
  // channel), so hide it like the output widget does.
  setRow(m_channelCombo, isAja);
  // GPU-direct capture exists for AJA (RDMA/DVP) and Deltacast (RDMA).
  m_rdmaCheckbox->setVisible(isAja || currentVendor() == Vendor::Deltacast);

  const bool isMagewell = currentVendor() == Vendor::Magewell;

  m_pixelFormatCombo->clear();
  if(isAja)
  {
    m_pixelFormatCombo->addItem("YCbCr 8-bit (UYVY)", "YCbCr8");
    m_pixelFormatCombo->addItem("YCbCr 10-bit (v210, GPU-direct only)", "YCbCr10");
    m_pixelFormatCombo->addItem("ARGB (32-bit BGRA)", "ARGB");
    m_pixelFormatCombo->addItem("RGBA (32-bit RGBA)", "RGBA");
  }
  else if(isMagewell)
  {
    // Magewell FOURCC tokens (translated to MWFOURCC_* in toMagewellInput).
    m_pixelFormatCombo->addItem("YCbCr 8-bit (UYVY)", "UYVY");
    m_pixelFormatCombo->addItem("YCbCr 10-bit (v210)", "V210");
    m_pixelFormatCombo->addItem("BGRA 8-bit", "BGRA");
    m_pixelFormatCombo->addItem("RGBA 8-bit", "RGBA");
  }
#if defined(SCORE_HAS_V4L2)
  else if(currentVendor() == Vendor::V4L2)
  {
    // The node's own fourccs. The consumer takes a literal four-character code,
    // so the SDI tokens below ("YCbCr8" and friends) are the wrong length and
    // were silently discarded, leaving whatever the driver was set to.
    m_pixelFormatCombo->addItem(tr("Auto"), QString{});
    const auto path = m_deviceCombo->currentData().toString().toStdString();
    std::vector<QString> seen;
    for(const auto& m : Gfx::V4L2::enumerateModes(path))
    {
      const char cc[5]
          = {char(m.fourcc & 0xff), char((m.fourcc >> 8) & 0xff),
             char((m.fourcc >> 16) & 0xff), char((m.fourcc >> 24) & 0xff), 0};
      const auto tok = QString::fromLatin1(cc);
      if(tok.size() != 4 || std::find(seen.begin(), seen.end(), tok) != seen.end())
        continue;
      seen.push_back(tok);
      m_pixelFormatCombo->addItem(tok, tok);
    }
  }
#endif
  else
  {
    m_pixelFormatCombo->addItem("YCbCr 8-bit", "YCbCr8");
    m_pixelFormatCombo->addItem("YCbCr 10-bit", "YCbCr10");
    m_pixelFormatCombo->addItem("BGRA 8-bit", "RGB8");
    // "RGB 10-bit" only exists in the DeckLink map; on Deltacast/Bluefish it
    // would silently degrade to 8-bit YCbCr.
    if(currentVendor() == Vendor::DeckLink)
      m_pixelFormatCombo->addItem("RGB 10-bit", "RGB10");
  }

  refreshDeviceList();
  updateFormatList();
}

void VideoInputSettingsWidget::refreshDeviceList()
{
  m_deviceCombo->clear();
  const Vendor vendor = currentVendor();
  if(vendor == Vendor::AJA)
  {
#if defined(SCORE_HAS_AJA)
    CNTV2DeviceScanner scanner;
    for(unsigned i = 0; i < scanner.GetNumDevices(); ++i)
    {
      NTV2DeviceInfo info;
      if(scanner.GetDeviceInfo(i, info))
        m_deviceCombo->addItem(
            QString::fromStdString(info.deviceIdentifier), static_cast<int>(i));
    }
#endif
  }
#if defined(SCORE_HAS_DECKLINK)
  else if(vendor == Vendor::DeckLink)
  {
    Gfx::DeckLink::ensureComInit();
    for(const auto& dev : Gfx::DeckLink::enumerateDevices())
      if(dev.canInput)
        m_deviceCombo->addItem(
            QString::fromStdString(dev.displayName), dev.index);
  }
#endif
#if defined(SCORE_HAS_DELTACAST)
  else if(vendor == Vendor::Deltacast)
  {
    Gfx::Deltacast::ensureVhdInit();
    for(const auto& dev : Gfx::Deltacast::enumerateDevices())
      if(dev.canInput)
        m_deviceCombo->addItem(
            QString::fromStdString(dev.displayName), dev.index);
  }
#endif
#if defined(SCORE_HAS_V4L2)
  else if(vendor == Vendor::V4L2)
  {
    // The node path is the identity here, not an index: a single USB camera
    // exposes several /dev/videoN and only some of them capture.
    for(const auto& dev : Gfx::V4L2::enumerateDevices())
      if(dev.canCapture)
        m_deviceCombo->addItem(
            QString::fromStdString(dev.card + " (" + dev.path + ")"),
            QString::fromStdString(dev.path));
  }
#endif
#if defined(SCORE_HAS_BLUEFISH)
  else if(vendor == Vendor::Bluefish)
  {
    Gfx::Bluefish::ensureBlueInit();
    for(const auto& dev : Gfx::Bluefish::enumerateDevices())
      if(dev.canInput)
        m_deviceCombo->addItem(
            QString::fromStdString(dev.displayName), dev.index);
  }
#endif
#if defined(SCORE_HAS_MAGEWELL)
  else if(vendor == Vendor::Magewell)
  {
    Gfx::Magewell::ensureMwInit();
    for(const auto& dev : Gfx::Magewell::enumerateDevices())
      if(dev.canInput)
        m_deviceCombo->addItem(
            QString::fromStdString(dev.displayName), dev.index);
  }
#endif
  if(m_deviceCombo->count() == 0)
    m_deviceCombo->addItem(tr("(no device detected)"), -1);
}

void VideoInputSettingsWidget::updateFormatList()
{
  // Items carry the token as their text (getSettings reads currentText).
  const QString prev = m_formatCombo->currentText();
  m_formatCombo->clear();

  // V4L2 is not an SDI card: its modes are whatever the node advertises, and
  // the consumer parses "<width>x<height>". Offering the fixed SDI mode names
  // here meant nothing ever parsed and the geometry silently stayed at
  // whatever the driver was last left at -- which on the 12.6 MP sensors is
  // full resolution, far past what the host-staged rung can carry.
#if defined(SCORE_HAS_V4L2)
  if(currentVendor() == Vendor::V4L2)
  {
    const auto path = m_deviceCombo->currentData().toString().toStdString();
    std::vector<QString> seen;
    for(const auto& m : Gfx::V4L2::enumerateModes(path))
    {
      if(m.width == 0 || m.height == 0)
        continue;
      const auto tok = QStringLiteral("%1x%2").arg(m.width).arg(m.height);
      if(std::find(seen.begin(), seen.end(), tok) != seen.end())
        continue;
      seen.push_back(tok);
      m_formatCombo->addItem(tok);
    }
    // "Auto" keeps the driver's current format, which is the right default for
    // a webcam and the only option when the node advertises nothing.
    m_formatCombo->insertItem(0, QStringLiteral("Auto"));
    const int vidx = m_formatCombo->findText(prev.isEmpty() ? "Auto" : prev);
    m_formatCombo->setCurrentIndex(vidx >= 0 ? vidx : 0);
    return;
  }
#endif

  caps::FormatList master;
  for(const char* tok : {"720p5994",  "720p60",    "1080p2398", "1080p24",
                         "1080p25",   "1080p2997", "1080p30",   "1080p50",
                         "1080p5994", "1080p60",   "1080i50",   "1080i5994",
                         "1080i60",   "2160p25",   "2160p2997", "2160p30",
                         "2160p50",   "2160p5994", "2160p60"})
    master.push_back({QString::fromLatin1(tok), QString::fromLatin1(tok)});

  // Keep only what the selected card supports (fail-open to the full list).
  const int devIdx = m_deviceCombo->currentData().toInt();
  for(const auto& e :
      caps::filterVideoFormats(currentVendor(), devIdx, /*forOutput=*/false, master))
    m_formatCombo->addItem(e.second);

  // Follow the wire: open at whatever resolution the input currently carries and
  // adopt live changes. DeckLink reads it from IDeckLinkStatus; AJA/Magewell/
  // Deltacast already auto-detect their incoming signal.
  m_formatCombo->insertItem(0, QStringLiteral("Auto"));

  const QString def = "1080p5994";
  int idx = m_formatCombo->findText(prev.isEmpty() ? def : prev);
  if(idx < 0)
    idx = m_formatCombo->findText(def);
  if(idx >= 0)
    m_formatCombo->setCurrentIndex(idx);
}

Device::DeviceSettings VideoInputSettingsWidget::getSettings() const
{
  Device::DeviceSettings s;
  s.name = m_deviceNameEdit->text();
  s.protocol = VideoInputProtocolFactory::static_concreteKey();
  VideoInputSettings set;
  set.vendor = currentVendor();
  set.deviceName = s.name;
  // V4L2 identifies a device by node path, every other vendor by index, so the
  // combo's data is a QString there and an int elsewhere. Reading it as an int
  // turned "/dev/video2" into 0 and always opened the first node.
  const auto devData = m_deviceCombo->currentData();
  if(set.vendor == Vendor::V4L2)
  {
    set.devicePath = devData.toString();
    set.deviceIndex = 0;
  }
  else
  {
    set.devicePath.clear();
    set.deviceIndex = std::max(0, devData.toInt());
  }
  set.channelIndex = m_channelCombo->currentData().toInt();
  set.videoFormat = m_formatCombo->currentText();
  set.pixelFormat = m_pixelFormatCombo->currentData().toString();
  set.resolutionMode = m_resolutionModeCombo->currentData().toInt();
  set.routingMode = m_routingModeCombo->currentData().toInt();
  set.useRDMA = m_rdmaCheckbox->isChecked();
  s.deviceSpecificSettings = QVariant::fromValue(set);
  return s;
}

void VideoInputSettingsWidget::setSettings(const Device::DeviceSettings& s)
{
  m_deviceNameEdit->setText(s.name);
  auto set = s.deviceSpecificSettings.value<VideoInputSettings>();

  int vIdx = m_vendorCombo->findData(static_cast<int>(set.vendor));
  if(vIdx >= 0)
    m_vendorCombo->setCurrentIndex(vIdx);
  onVendorChanged();

  const int deviceIdx = set.vendor == Vendor::V4L2
                            ? m_deviceCombo->findData(set.devicePath)
                            : m_deviceCombo->findData(set.deviceIndex);
  if(deviceIdx >= 0)
    m_deviceCombo->setCurrentIndex(deviceIdx);
  for(int i = 0; i < m_channelCombo->count(); ++i)
    if(m_channelCombo->itemData(i).toInt() == set.channelIndex)
      m_channelCombo->setCurrentIndex(i);
  m_formatCombo->setCurrentText(set.videoFormat);
  int pfIdx = m_pixelFormatCombo->findData(set.pixelFormat);
  if(pfIdx >= 0)
    m_pixelFormatCombo->setCurrentIndex(pfIdx);
  for(int i = 0; i < m_resolutionModeCombo->count(); ++i)
    if(m_resolutionModeCombo->itemData(i).toInt() == set.resolutionMode)
      m_resolutionModeCombo->setCurrentIndex(i);
  for(int i = 0; i < m_routingModeCombo->count(); ++i)
    if(m_routingModeCombo->itemData(i).toInt() == set.routingMode)
      m_routingModeCombo->setCurrentIndex(i);
  m_rdmaCheckbox->setChecked(set.useRDMA);
}

// =============================================================================
// Per-vendor enumerators
// =============================================================================

namespace
{
#if defined(SCORE_HAS_AJA)
class AjaVideoInputEnumerator final : public Device::DeviceEnumerator
{
public:
  void enumerate(
      std::function<void(const QString&, const Device::DeviceSettings&)> func)
      const override
  {
    CNTV2DeviceScanner scanner;
    for(unsigned i = 0; i < scanner.GetNumDevices(); ++i)
    {
      NTV2DeviceInfo info;
      if(!scanner.GetDeviceInfo(i, info))
        continue;
      Device::DeviceSettings s;
      s.name = QString::fromStdString(info.deviceIdentifier);
      s.protocol = VideoInputProtocolFactory::static_concreteKey();
      VideoInputSettings set;
      set.vendor = Vendor::AJA;
      set.deviceName = s.name;
      set.deviceIndex = static_cast<int>(i);
      s.deviceSpecificSettings = QVariant::fromValue(set);
      func(s.name, s);
    }
  }
};
#endif

#if defined(SCORE_HAS_DECKLINK)
class DeckLinkVideoInputEnumerator final : public Device::DeviceEnumerator
{
public:
  void enumerate(
      std::function<void(const QString&, const Device::DeviceSettings&)> func)
      const override
  {
    Gfx::DeckLink::ensureComInit();
    for(const auto& dev : Gfx::DeckLink::enumerateDevices())
    {
      if(!dev.canInput)
        continue;
      Device::DeviceSettings s;
      s.name = QString::fromStdString(dev.displayName);
      s.protocol = VideoInputProtocolFactory::static_concreteKey();
      VideoInputSettings set;
      set.vendor = Vendor::DeckLink;
      set.deviceName = s.name;
      set.deviceIndex = dev.index;
      s.deviceSpecificSettings = QVariant::fromValue(set);
      func(s.name, s);
    }
  }
};
#endif

#if defined(SCORE_HAS_DELTACAST)
class DeltacastVideoInputEnumerator final : public Device::DeviceEnumerator
{
public:
  void enumerate(
      std::function<void(const QString&, const Device::DeviceSettings&)> func)
      const override
  {
    Gfx::Deltacast::ensureVhdInit();
    for(const auto& dev : Gfx::Deltacast::enumerateDevices())
    {
      if(!dev.canInput)
        continue;
      Device::DeviceSettings s;
      s.name = QString::fromStdString(dev.displayName);
      s.protocol = VideoInputProtocolFactory::static_concreteKey();
      VideoInputSettings set;
      set.vendor = Vendor::Deltacast;
      set.deviceName = s.name;
      set.deviceIndex = dev.index;
      s.deviceSpecificSettings = QVariant::fromValue(set);
      func(s.name, s);
    }
  }
};
#endif

#if defined(SCORE_HAS_BLUEFISH)
class BluefishVideoInputEnumerator final : public Device::DeviceEnumerator
{
public:
  void enumerate(
      std::function<void(const QString&, const Device::DeviceSettings&)> func)
      const override
  {
    Gfx::Bluefish::ensureBlueInit();
    for(const auto& dev : Gfx::Bluefish::enumerateDevices())
    {
      if(!dev.canInput)
        continue;
      Device::DeviceSettings s;
      s.name = QString::fromStdString(dev.displayName);
      s.protocol = VideoInputProtocolFactory::static_concreteKey();
      VideoInputSettings set;
      set.vendor = Vendor::Bluefish;
      set.deviceName = s.name;
      set.deviceIndex = dev.index;
      s.deviceSpecificSettings = QVariant::fromValue(set);
      func(s.name, s);
    }
  }
};
#endif

#if defined(SCORE_HAS_V4L2)
class V4L2VideoInputEnumerator final : public Device::DeviceEnumerator
{
public:
  void enumerate(
      std::function<void(const QString&, const Device::DeviceSettings&)> func)
      const override
  {
    for(const auto& dev : Gfx::V4L2::enumerateDevices())
    {
      if(!dev.canCapture)
        continue;
      Device::DeviceSettings s;
      // The card name alone is ambiguous (a UVC camera exposes several nodes),
      // so the path is part of the label.
      s.name = QString::fromStdString(dev.card + " (" + dev.path + ")");
      s.protocol = VideoInputProtocolFactory::static_concreteKey();
      VideoInputSettings set;
      set.vendor = Vendor::V4L2;
      set.deviceName = QString::fromStdString(dev.path);
      s.deviceSpecificSettings = QVariant::fromValue(set);
      func(s.name, s);
    }
  }
};
#endif

#if defined(SCORE_HAS_MAGEWELL)
class MagewellVideoInputEnumerator final : public Device::DeviceEnumerator
{
public:
  void enumerate(
      std::function<void(const QString&, const Device::DeviceSettings&)> func)
      const override
  {
    Gfx::Magewell::ensureMwInit();
    for(const auto& dev : Gfx::Magewell::enumerateDevices())
    {
      if(!dev.canInput)
        continue;
      Device::DeviceSettings s;
      s.name = QString::fromStdString(dev.displayName);
      s.protocol = VideoInputProtocolFactory::static_concreteKey();
      VideoInputSettings set;
      set.vendor = Vendor::Magewell;
      set.deviceName = s.name;
      set.deviceIndex = dev.index;
      s.deviceSpecificSettings = QVariant::fromValue(set);
      func(s.name, s);
    }
  }
};
#endif
} // namespace

Device::DeviceEnumerators
VideoInputProtocolFactory::getEnumerators(const score::DocumentContext&) const
{
  Device::DeviceEnumerators e;
#if defined(SCORE_HAS_AJA)
  e.push_back({"AJA", new AjaVideoInputEnumerator});
#endif
#if defined(SCORE_HAS_DECKLINK)
  e.push_back({"DeckLink", new DeckLinkVideoInputEnumerator});
#endif
#if defined(SCORE_HAS_DELTACAST)
  e.push_back({"DELTACAST", new DeltacastVideoInputEnumerator});
#endif
#if defined(SCORE_HAS_BLUEFISH)
  e.push_back({"Bluefish444", new BluefishVideoInputEnumerator});
#endif
#if defined(SCORE_HAS_MAGEWELL)
  e.push_back({"Magewell", new MagewellVideoInputEnumerator});
#endif
#if defined(SCORE_HAS_V4L2)
  e.push_back({"V4L2", new V4L2VideoInputEnumerator});
#endif
  return e;
}

} // namespace Gfx::VideoIO
