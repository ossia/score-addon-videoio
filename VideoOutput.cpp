#include "VideoOutput.hpp"

#include <Gfx/GfxApplicationPlugin.hpp>
#include <Gfx/GfxParameter.hpp>
#include <Gfx/Graph/NodeRenderer.hpp>

#include <score/serialization/MimeVisitor.hpp>

#include <VideoIOCaps.hpp>

#include <ossia/network/generic/generic_device.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>

#include <wobjectimpl.h>

#if defined(SCORE_HAS_AJA)
#include <AJA/AJAOutputNode.hpp>

#include <ntv2card.h>
#include <ntv2devicescanner.h>
#endif

#if defined(SCORE_HAS_DECKLINK)
#include <decklink/DeckLinkDevices.hpp>
#include <decklink/DeckLinkModeMap.hpp>
#include <decklink/DeckLinkNode.hpp>
#endif

#if defined(SCORE_HAS_DELTACAST)
#include <deltacast/DeltacastDevices.hpp>
#include <deltacast/DeltacastFormats.hpp>
#include <deltacast/DeltacastNode.hpp>
#endif

#if defined(SCORE_HAS_BLUEFISH)
#include <bluefish/BluefishDevices.hpp>
#include <bluefish/BluefishNode.hpp>
#include <bluefish/BluefishSettings.hpp>
#endif

W_OBJECT_IMPL(Gfx::VideoIO::VideoOutputDevice)

namespace Gfx::VideoIO
{
namespace
{

// Generic gfx device wrapper: holds a pre-built vendor output node (AJANode /
// DeckLinkNode — both score::gfx::OutputNode) exactly the way the old aja_device
// did, so cable wiring reaches the node's input port.
class direct_video_output_device : public ossia::net::device_base
{
  gfx_node_base root;

public:
  direct_video_output_device(
      score::gfx::Node* node, std::unique_ptr<gfx_protocol_base> proto,
      std::string name)
      : ossia::net::device_base{std::move(proto)}
      , root{
            *this, *static_cast<gfx_protocol_base*>(m_protocol.get()), node,
            std::move(name)}
  {
  }

  const gfx_node_base& get_root_node() const override { return root; }
  gfx_node_base& get_root_node() override { return root; }
};

#if defined(SCORE_HAS_AJA)
Gfx::AJA::AJAOutputSettings toAja(const VideoOutputSettings& s)
{
  Gfx::AJA::AJAOutputSettings a;
  a.deviceName = s.deviceName;
  a.deviceIndex = s.deviceIndex;
  a.channelIndex = s.channelIndex;
  a.width = s.width;
  a.height = s.height;
  a.rate = s.rate;
  a.videoFormat = s.videoFormat;
  a.pixelFormat = s.pixelFormat;
  a.useRDMA = s.useRDMA;
  a.mode8K = static_cast<Gfx::AJA::AJA8KMode>(s.mode8K);
  a.hdrMode = static_cast<Gfx::AJA::AJAHDRMode>(s.hdrMode);
  // videoFormatEnum left UNKNOWN: the string drives the format.
  return a;
}
#endif

#if defined(SCORE_HAS_DECKLINK)
Gfx::DeckLink::DeckLinkOutputSettings toDeckLink(const VideoOutputSettings& s)
{
  Gfx::DeckLink::DeckLinkOutputSettings d;
  d.deviceIndex = s.deviceIndex;
  d.displayMode = Gfx::DeckLink::bmdModeFromToken(s.videoFormat);
  d.pixelFormat = Gfx::DeckLink::bmdPixelFromToken(s.pixelFormat);
  return d;
}
#endif

#if defined(SCORE_HAS_DELTACAST)
Gfx::Deltacast::DeltacastOutputSettings toDeltacast(const VideoOutputSettings& s)
{
  Gfx::Deltacast::DeltacastOutputSettings d;
  d.deviceIndex = s.deviceIndex;
  d.videoStandard = Gfx::Deltacast::vhdStandardFromToken(s.videoFormat);
  d.bufferPacking = Gfx::Deltacast::vhdPackingFromToken(s.pixelFormat);
  d.fractionalClock = Gfx::Deltacast::vhdIsFractionalRate(s.videoFormat);
  return d;
}
#endif

#if defined(SCORE_HAS_BLUEFISH)
Gfx::Bluefish::BluefishOutputSettings toBluefish(const VideoOutputSettings& s)
{
  Gfx::Bluefish::BluefishOutputSettings b;
  b.deviceIndex = std::max(1, s.deviceIndex); // Bluefish ids are 1-based
  b.videoModeExt = Gfx::Bluefish::videoModeExtFromToken(s.videoFormat);
  b.memoryFormat = Gfx::Bluefish::memFmtFromToken(s.pixelFormat);
  return b; // videoModeExt/memoryFormat carried as uint32_t (EVideoModeExt/EMemoryFormat)
}
#endif

} // namespace

// =============================================================================
// VideoOutputProtocolFactory
// =============================================================================

QString VideoOutputProtocolFactory::prettyName() const noexcept
{
  return QObject::tr("Direct Video Output");
}

QString VideoOutputProtocolFactory::category() const noexcept
{
  return StandardCategories::video_out;
}

QUrl VideoOutputProtocolFactory::manual() const noexcept
{
  return QUrl("https://ossia.io/score-docs/devices/aja-device.html");
}

Device::DeviceInterface* VideoOutputProtocolFactory::makeDevice(
    const Device::DeviceSettings& settings, const Explorer::DeviceDocumentPlugin&,
    const score::DocumentContext& ctx)
{
  return new VideoOutputDevice{settings, ctx};
}

const Device::DeviceSettings&
VideoOutputProtocolFactory::defaultSettings() const noexcept
{
  static Device::DeviceSettings settings;
  if(settings.name.isEmpty())
  {
    settings.name = "Direct Video Out";
    VideoOutputSettings set;
    set.deviceName = "Direct Video Out";
    settings.deviceSpecificSettings = QVariant::fromValue(set);
  }
  return settings;
}

Device::ProtocolSettingsWidget* VideoOutputProtocolFactory::makeSettingsWidget()
{
  return new VideoOutputSettingsWidget;
}

Device::AddressDialog* VideoOutputProtocolFactory::makeAddAddressDialog(
    const Device::DeviceInterface&, const score::DocumentContext&, QWidget*)
{
  return nullptr;
}

Device::AddressDialog* VideoOutputProtocolFactory::makeEditAddressDialog(
    const Device::AddressSettings&, const Device::DeviceInterface&,
    const score::DocumentContext&, QWidget*)
{
  return nullptr;
}

QVariant VideoOutputProtocolFactory::makeProtocolSpecificSettings(
    const VisitorVariant& visitor) const
{
  return makeProtocolSpecificSettings_T<VideoOutputSettings>(visitor);
}

void VideoOutputProtocolFactory::serializeProtocolSpecificSettings(
    const QVariant& data, const VisitorVariant& visitor) const
{
  serializeProtocolSpecificSettings_T<VideoOutputSettings>(data, visitor);
}

bool VideoOutputProtocolFactory::checkCompatibility(
    const Device::DeviceSettings& a, const Device::DeviceSettings& b) const noexcept
{
  // "Can a and b coexist?" — the device explorer also probes against an empty
  // DeviceSettings{} (null protocol), which must always be compatible.
  if(a.protocol != b.protocol)
    return true;
  auto sa = a.deviceSpecificSettings.value<VideoOutputSettings>();
  auto sb = b.deviceSpecificSettings.value<VideoOutputSettings>();
  // Only one output device may own a given physical card at a time (the
  // backends acquire the whole board, e.g. AcquireStreamForApplication).
  return !(sa.vendor == sb.vendor && sa.deviceIndex == sb.deviceIndex);
}

// =============================================================================
// VideoOutputDevice
// =============================================================================

VideoOutputDevice::~VideoOutputDevice() = default;

void VideoOutputDevice::disconnect()
{
  GfxOutputDevice::disconnect();
  auto prev = std::move(m_dev);
  m_dev = {};
  deviceChanged(prev.get(), nullptr);
}

bool VideoOutputDevice::reconnect()
{
  disconnect();

  try
  {
    auto plug = m_ctx.findPlugin<DocumentPlugin>();
    if(plug)
    {
      auto set = settings().deviceSpecificSettings.value<VideoOutputSettings>();

      score::gfx::Node* node = nullptr;
      switch(set.vendor)
      {
        case Vendor::AJA:
#if defined(SCORE_HAS_AJA)
          node = new Gfx::AJA::AJANode{toAja(set)};
#endif
          break;
        case Vendor::DeckLink:
#if defined(SCORE_HAS_DECKLINK)
          node = new Gfx::DeckLink::DeckLinkNode{toDeckLink(set)};
#endif
          break;
        case Vendor::Deltacast:
#if defined(SCORE_HAS_DELTACAST)
          node = new Gfx::Deltacast::DeltacastNode{toDeltacast(set)};
#endif
          break;
        case Vendor::Bluefish:
#if defined(SCORE_HAS_BLUEFISH)
          node = new Gfx::Bluefish::BluefishNode{toBluefish(set)};
#endif
          break;
        case Vendor::Magewell:
          // Magewell is capture-only — no playout. It's never offered by the
          // output enumerator/widget; node stays null -> graceful no-op below.
          break;
      }

      if(!node)
      {
        qDebug() << "Direct Video Output: vendor not compiled in";
        return false;
      }

      m_protocol = new gfx_protocol_base{plug->exec};
      m_dev = std::make_unique<direct_video_output_device>(
          node, std::unique_ptr<gfx_protocol_base>(m_protocol),
          settings().name.toStdString());
      deviceChanged(nullptr, m_dev.get());
    }
  }
  catch(std::exception& e)
  {
    qDebug() << "Direct Video Output error: " << e.what();
  }
  catch(...)
  {
    qDebug() << "Direct Video Output error";
  }

  return connected();
}

// =============================================================================
// VideoOutputSettingsWidget
// =============================================================================

VideoOutputSettingsWidget::VideoOutputSettingsWidget(QWidget* parent)
    : Device::ProtocolSettingsWidget(parent)
{
  m_layout = new QFormLayout(this);

  m_deviceNameEdit = new QLineEdit(this);
  m_deviceNameEdit->setText("Direct Video Out");
  m_layout->addRow(tr("Name"), m_deviceNameEdit);
  this->checkForChanges(m_deviceNameEdit);

  m_vendorCombo = new QComboBox(this);
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
  m_layout->addRow(tr("Vendor"), m_vendorCombo);

  m_deviceCombo = new QComboBox(this);
  m_layout->addRow(tr("Device"), m_deviceCombo);

  m_channelCombo = new QComboBox(this);
  m_layout->addRow(tr("SDI Channel"), m_channelCombo);

  m_formatCombo = new QComboBox(this);
  m_layout->addRow(tr("Video Format"), m_formatCombo);

  m_pixelFormatCombo = new QComboBox(this);
  m_layout->addRow(tr("Pixel Format"), m_pixelFormatCombo);

  m_width = new QSpinBox(this);
  m_width->setRange(720, 8192);
  m_width->setValue(1920);
  m_layout->addRow(tr("Width"), m_width);

  m_height = new QSpinBox(this);
  m_height->setRange(480, 4320);
  m_height->setValue(1080);
  m_layout->addRow(tr("Height"), m_height);

  m_rate = new QSpinBox(this);
  m_rate->setRange(24, 120);
  m_rate->setValue(60);
  m_layout->addRow(tr("Frame Rate"), m_rate);

  m_8kModeCombo = new QComboBox(this);
  m_8kModeCombo->addItem(tr("Disabled (HD/4K)"), 0);
  m_8kModeCombo->addItem(tr("8K TSI (2 FrameStores)"), 1);
  m_8kModeCombo->addItem(tr("8K Squares (4 FrameStores)"), 2);
  m_layout->addRow(tr("8K Mode"), m_8kModeCombo);

  m_hdrModeCombo = new QComboBox(this);
  m_hdrModeCombo->addItem(tr("SDR (BT.709 limited)"), 0);
  m_hdrModeCombo->addItem(tr("HDR10 (BT.2020 + PQ)"), 1);
  m_hdrModeCombo->addItem(tr("HLG (BT.2020 + ARIB STD-B67)"), 2);
  m_layout->addRow(tr("HDR"), m_hdrModeCombo);

  m_rdmaCheckbox = new QCheckBox(tr("Enable GPUDirect RDMA"), this);
  m_rdmaCheckbox->setChecked(true);
  m_layout->addRow(m_rdmaCheckbox);

  connect(
      m_vendorCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
      [this](int) { onVendorChanged(); });
  connect(
      m_deviceCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
      [this](int i) {
        updateChannelList(i);
        updateFormatList();
        updatePixelFormatList();
      });
  connect(
      m_8kModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
      [this](int) { updateFormatList(); });

  onVendorChanged();
}

Vendor VideoOutputSettingsWidget::currentVendor() const
{
  return static_cast<Vendor>(m_vendorCombo->currentData().toInt());
}

void VideoOutputSettingsWidget::onVendorChanged()
{
  const bool isAja = currentVendor() == Vendor::AJA;

  auto setRow = [this](QWidget* w, bool vis) {
    if(auto* l = m_layout->labelForField(w))
      l->setVisible(vis);
    w->setVisible(vis);
  };
  // AJA-only controls.
  setRow(m_channelCombo, isAja);
  setRow(m_8kModeCombo, isAja);
  setRow(m_hdrModeCombo, isAja);
  m_rdmaCheckbox->setVisible(isAja);

  updatePixelFormatList();
  refreshDeviceList();
  updateFormatList();
}

void VideoOutputSettingsWidget::refreshDeviceList()
{
  m_deviceCombo->clear();
  const Vendor vendor = currentVendor();

  if(vendor == Vendor::AJA)
  {
#if defined(SCORE_HAS_AJA)
    CNTV2DeviceScanner scanner;
    const int n = static_cast<int>(scanner.GetNumDevices());
    for(int i = 0; i < n; ++i)
    {
      NTV2DeviceInfo info;
      if(scanner.GetDeviceInfo(i, info))
        m_deviceCombo->addItem(
            QString::fromStdString(info.deviceIdentifier), i);
    }
#endif
  }
#if defined(SCORE_HAS_DECKLINK)
  else if(vendor == Vendor::DeckLink)
  {
    Gfx::DeckLink::ensureComInit();
    for(const auto& dev : Gfx::DeckLink::enumerateDevices())
    {
      if(dev.canOutput)
        m_deviceCombo->addItem(
            QString::fromStdString(dev.displayName), dev.index);
    }
  }
#endif
#if defined(SCORE_HAS_DELTACAST)
  else if(vendor == Vendor::Deltacast)
  {
    Gfx::Deltacast::ensureVhdInit();
    for(const auto& dev : Gfx::Deltacast::enumerateDevices())
    {
      if(dev.canOutput)
        m_deviceCombo->addItem(
            QString::fromStdString(dev.displayName), dev.index);
    }
  }
#endif
#if defined(SCORE_HAS_BLUEFISH)
  else if(vendor == Vendor::Bluefish)
  {
    Gfx::Bluefish::ensureBlueInit();
    for(const auto& dev : Gfx::Bluefish::enumerateDevices())
    {
      if(dev.canOutput)
        m_deviceCombo->addItem(
            QString::fromStdString(dev.displayName), dev.index);
    }
  }
#endif

  if(m_deviceCombo->count() == 0)
  {
    m_deviceCombo->addItem(tr("(no device detected)"), -1);
    m_deviceCombo->setEnabled(false);
  }
  else
  {
    m_deviceCombo->setEnabled(true);
    updateChannelList(0);
  }
}

void VideoOutputSettingsWidget::updateChannelList(int)
{
  if(currentVendor() != Vendor::AJA)
    return;
#if defined(SCORE_HAS_AJA)
  m_channelCombo->clear();
  int devIdx = m_deviceCombo->currentData().toInt();
  if(devIdx < 0)
    return;
  CNTV2Card card(static_cast<UWord>(devIdx));
  if(!card.IsOpen())
    return;
  const ULWord numOutputs = card.features().GetNumVideoOutputs();
  for(ULWord i = 0; i < numOutputs; ++i)
    m_channelCombo->addItem(QString("SDI Out %1").arg(i + 1), static_cast<int>(i));
#endif
}

void VideoOutputSettingsWidget::updateFormatList()
{
  const QString prev = m_formatCombo->currentData().toString();
  m_formatCombo->clear();
  const bool isAja = currentVendor() == Vendor::AJA;
  const bool eightK = isAja && m_8kModeCombo->currentData().toInt() != 0;

  caps::FormatList master;
  if(eightK)
  {
    master = {
        {tr("UHD2 50 (7680x4320)"), "UHD2_50"},
        {tr("UHD2 59.94 (7680x4320)"), "UHD2_5994"},
        {tr("UHD2 60 (7680x4320)"), "UHD2_60"},
        {tr("8K DCI 59.94 (8192x4320)"), "8K_5994"},
        {tr("8K DCI 60 (8192x4320)"), "8K_60"}};
  }
  else
  {
    master = {
        {tr("1080p 23.98"), "1080p2398"}, {tr("1080p 24"), "1080p24"},
        {tr("1080p 25"), "1080p25"},      {tr("1080p 29.97"), "1080p2997"},
        {tr("1080p 30"), "1080p30"},      {tr("1080p 50"), "1080p50"},
        {tr("1080p 59.94"), "1080p5994"}, {tr("1080p 60"), "1080p60"},
        {tr("1080i 50"), "1080i50"},      {tr("1080i 59.94"), "1080i5994"},
        {tr("720p 50"), "720p50"},        {tr("720p 59.94"), "720p5994"},
        {tr("720p 60"), "720p60"},        {tr("UHD 25"), "2160p25"},
        {tr("UHD 29.97"), "2160p2997"},   {tr("UHD 30"), "2160p30"},
        {tr("UHD 50"), "2160p50"},        {tr("UHD 59.94"), "2160p5994"},
        {tr("UHD 60"), "2160p60"}};
  }

  // Keep only what the selected card supports (fail-open to the full list).
  const int devIdx = m_deviceCombo->currentData().toInt();
  for(const auto& e : caps::filterVideoFormats(currentVendor(), devIdx, true, master))
    m_formatCombo->addItem(e.first, e.second);

  const QString def = eightK ? "UHD2_5994" : "1080p5994";
  int idx = m_formatCombo->findData(prev);
  if(idx < 0)
    idx = m_formatCombo->findData(def);
  if(idx >= 0)
    m_formatCombo->setCurrentIndex(idx);
}

void VideoOutputSettingsWidget::updatePixelFormatList()
{
  const QString prev = m_pixelFormatCombo->currentData().toString();
  m_pixelFormatCombo->clear();
  const bool isAja = currentVendor() == Vendor::AJA;

  caps::FormatList master;
  if(isAja)
  {
    master = {
        {tr("YCbCr 10-bit"), "YCbCr10"},    {tr("YCbCr 8-bit"), "YCbCr8"},
        {tr("RGB 8-bit"), "RGB8"},          {tr("RGB 10-bit"), "RGB10"},
        {tr("RGB 12-bit"), "RGB12"},        {tr("RGB 12-bit packed"), "RGB12P"},
        {tr("RGB 10-bit DPX"), "RGB10DPX"}, {tr("RGB 24-bit"), "RGB24"}};
  }
  else
  {
    master = {
        {tr("YCbCr 8-bit"), "YCbCr8"}, {tr("YCbCr 10-bit"), "YCbCr10"},
        {tr("BGRA 8-bit"), "RGB8"},    {tr("RGB 10-bit"), "RGB10"}};
  }

  const int devIdx = m_deviceCombo->currentData().toInt();
  for(const auto& e : caps::filterPixelFormats(currentVendor(), devIdx, master))
    m_pixelFormatCombo->addItem(e.first, e.second);

  int idx = m_pixelFormatCombo->findData(prev);
  if(idx >= 0)
    m_pixelFormatCombo->setCurrentIndex(idx);
}

Device::DeviceSettings VideoOutputSettingsWidget::getSettings() const
{
  Device::DeviceSettings settings;
  settings.name = m_deviceNameEdit->text();
  settings.protocol = VideoOutputProtocolFactory::static_concreteKey();

  VideoOutputSettings set;
  set.vendor = currentVendor();
  set.deviceName = m_deviceNameEdit->text();
  set.deviceIndex = std::max(0, m_deviceCombo->currentData().toInt());
  set.channelIndex = m_channelCombo->currentData().toInt();
  set.videoFormat = m_formatCombo->currentData().toString();
  set.pixelFormat = m_pixelFormatCombo->currentData().toString();
  set.width = m_width->value();
  set.height = m_height->value();
  set.rate = m_rate->value();
  set.useRDMA = m_rdmaCheckbox->isChecked();
  set.mode8K = m_8kModeCombo->currentData().toInt();
  set.hdrMode = m_hdrModeCombo->currentData().toInt();

  settings.deviceSpecificSettings = QVariant::fromValue(set);
  return settings;
}

void VideoOutputSettingsWidget::setSettings(const Device::DeviceSettings& settings)
{
  m_deviceNameEdit->setText(settings.name);
  auto set = settings.deviceSpecificSettings.value<VideoOutputSettings>();

  int vIdx = m_vendorCombo->findData(static_cast<int>(set.vendor));
  if(vIdx >= 0)
    m_vendorCombo->setCurrentIndex(vIdx);
  onVendorChanged();

  int deviceIdx = m_deviceCombo->findData(set.deviceIndex);
  if(deviceIdx >= 0)
    m_deviceCombo->setCurrentIndex(deviceIdx);
  int channelIdx = m_channelCombo->findData(set.channelIndex);
  if(channelIdx >= 0)
    m_channelCombo->setCurrentIndex(channelIdx);
  int formatIdx = m_formatCombo->findData(set.videoFormat);
  if(formatIdx >= 0)
    m_formatCombo->setCurrentIndex(formatIdx);
  int pixelFormatIdx = m_pixelFormatCombo->findData(set.pixelFormat);
  if(pixelFormatIdx >= 0)
    m_pixelFormatCombo->setCurrentIndex(pixelFormatIdx);

  m_width->setValue(set.width);
  m_height->setValue(set.height);
  m_rate->setValue(static_cast<int>(set.rate));
  m_rdmaCheckbox->setChecked(set.useRDMA);
  int mode8KIdx = m_8kModeCombo->findData(set.mode8K);
  if(mode8KIdx >= 0)
    m_8kModeCombo->setCurrentIndex(mode8KIdx);
  int hdrIdx = m_hdrModeCombo->findData(set.hdrMode);
  if(hdrIdx >= 0)
    m_hdrModeCombo->setCurrentIndex(hdrIdx);
}

// =============================================================================
// Per-vendor enumerators
// =============================================================================

namespace
{
#if defined(SCORE_HAS_AJA)
class AjaVideoOutputEnumerator final : public Device::DeviceEnumerator
{
public:
  void enumerate(
      std::function<void(const QString&, const Device::DeviceSettings&)> func)
      const override
  {
    CNTV2DeviceScanner scanner;
    const size_t n = scanner.GetNumDevices();
    for(size_t i = 0; i < n; ++i)
    {
      NTV2DeviceInfo info;
      if(!scanner.GetDeviceInfo(static_cast<UWord>(i), info))
        continue;
      Device::DeviceSettings s;
      s.name = QString::fromStdString(info.deviceIdentifier);
      s.protocol = VideoOutputProtocolFactory::static_concreteKey();
      VideoOutputSettings set;
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
class DeckLinkVideoOutputEnumerator final : public Device::DeviceEnumerator
{
public:
  void enumerate(
      std::function<void(const QString&, const Device::DeviceSettings&)> func)
      const override
  {
    Gfx::DeckLink::ensureComInit();
    for(const auto& dev : Gfx::DeckLink::enumerateDevices())
    {
      if(!dev.canOutput)
        continue;
      Device::DeviceSettings s;
      s.name = QString::fromStdString(dev.displayName);
      s.protocol = VideoOutputProtocolFactory::static_concreteKey();
      VideoOutputSettings set;
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
class DeltacastVideoOutputEnumerator final : public Device::DeviceEnumerator
{
public:
  void enumerate(
      std::function<void(const QString&, const Device::DeviceSettings&)> func)
      const override
  {
    Gfx::Deltacast::ensureVhdInit();
    for(const auto& dev : Gfx::Deltacast::enumerateDevices())
    {
      if(!dev.canOutput)
        continue;
      Device::DeviceSettings s;
      s.name = QString::fromStdString(dev.displayName);
      s.protocol = VideoOutputProtocolFactory::static_concreteKey();
      VideoOutputSettings set;
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
class BluefishVideoOutputEnumerator final : public Device::DeviceEnumerator
{
public:
  void enumerate(
      std::function<void(const QString&, const Device::DeviceSettings&)> func)
      const override
  {
    Gfx::Bluefish::ensureBlueInit();
    for(const auto& dev : Gfx::Bluefish::enumerateDevices())
    {
      if(!dev.canOutput)
        continue;
      Device::DeviceSettings s;
      s.name = QString::fromStdString(dev.displayName);
      s.protocol = VideoOutputProtocolFactory::static_concreteKey();
      VideoOutputSettings set;
      set.vendor = Vendor::Bluefish;
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
VideoOutputProtocolFactory::getEnumerators(const score::DocumentContext&) const
{
  Device::DeviceEnumerators e;
#if defined(SCORE_HAS_AJA)
  e.push_back({"AJA", new AjaVideoOutputEnumerator});
#endif
#if defined(SCORE_HAS_DECKLINK)
  e.push_back({"DeckLink", new DeckLinkVideoOutputEnumerator});
#endif
#if defined(SCORE_HAS_DELTACAST)
  e.push_back({"DELTACAST", new DeltacastVideoOutputEnumerator});
#endif
#if defined(SCORE_HAS_BLUEFISH)
  e.push_back({"Bluefish444", new BluefishVideoOutputEnumerator});
#endif
  return e;
}

} // namespace Gfx::VideoIO
