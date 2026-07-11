#include "AJAOutput.hpp"
#include "AJAOutputNode.hpp"

#include <Gfx/GfxApplicationPlugin.hpp>
#include <Gfx/GfxParameter.hpp>
#include <Gfx/Graph/NodeRenderer.hpp>

#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <score/serialization/MimeVisitor.hpp>

#include <ossia/network/generic/generic_device.hpp>
#include <ossia/network/generic/generic_node.hpp>
#include <ossia/network/generic/generic_parameter.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>

#include <ntv2card.h>
#include <ntv2devicescanner.h>
#include <ntv2utils.h>

#include <wobjectimpl.h>

W_OBJECT_IMPL(Gfx::AJA::AJADevice)

namespace Gfx::AJA
{

// =============================================================================
// Internal device class for ossia integration
// =============================================================================
//
// Mirror of the Spout/Sh4lt pattern: the device's root is a gfx_node_base
// (not a plain generic_node). gfx_node_base constructs a gfx_parameter_base
// that registers the AJANode with the gfx context AND exposes push_texture()
// so that score's UI cabling translates into edges on the AJANode's input
// port. Without this wrapper, the node is invisible to the cable wiring and
// `input[0]->edges` stays empty even though the GUI shows a connection.

class aja_device : public ossia::net::device_base
{
  gfx_node_base root;

public:
  aja_device(
      const AJAOutputSettings& settings,
      std::unique_ptr<gfx_protocol_base> proto,
      std::string name)
      : ossia::net::device_base{std::move(proto)}
      , root{
            *this,
            *static_cast<gfx_protocol_base*>(m_protocol.get()),
            new AJANode{settings},
            name}
  {
  }

  ~aja_device() override = default;

  const gfx_node_base& get_root_node() const override { return root; }
  gfx_node_base& get_root_node() override { return root; }
};

// =============================================================================
// AJAProtocolFactory
// =============================================================================

QString AJAProtocolFactory::prettyName() const noexcept
{
  return QObject::tr("AJA SDI Output");
}

QString AJAProtocolFactory::category() const noexcept
{
  return StandardCategories::video_out;
}

QUrl AJAProtocolFactory::manual() const noexcept
{
  return QUrl("https://ossia.io/score-docs/devices/aja-device.html");
}

Device::DeviceEnumerators
AJAProtocolFactory::getEnumerators(const score::DocumentContext& ctx) const
{
  return {{"Devices", new AJADeviceEnumerator}};
}

Device::DeviceInterface* AJAProtocolFactory::makeDevice(
    const Device::DeviceSettings& settings,
    const Explorer::DeviceDocumentPlugin& plugin,
    const score::DocumentContext& ctx)
{
  return new AJADevice{settings, ctx};
}

const Device::DeviceSettings& AJAProtocolFactory::defaultSettings() const noexcept
{
  static Device::DeviceSettings settings;
  if(settings.name.isEmpty())
  {
    settings.name = "AJA";
    AJAOutputSettings aja;
    aja.deviceName = "AJA";
    aja.width = 1920;
    aja.height = 1080;
    aja.rate = 59.94;
    aja.videoFormat = "1080p5994";
    aja.pixelFormat = "YCbCr10";
    aja.useRDMA = true;
    settings.deviceSpecificSettings = QVariant::fromValue(aja);
  }
  return settings;
}

Device::ProtocolSettingsWidget* AJAProtocolFactory::makeSettingsWidget()
{
  return new AJASettingsWidget;
}

Device::AddressDialog* AJAProtocolFactory::makeAddAddressDialog(
    const Device::DeviceInterface& dev, const score::DocumentContext& ctx,
    QWidget* parent)
{
  return nullptr;
}

Device::AddressDialog* AJAProtocolFactory::makeEditAddressDialog(
    const Device::AddressSettings& set, const Device::DeviceInterface& dev,
    const score::DocumentContext& ctx, QWidget* parent)
{
  return nullptr;
}

QVariant AJAProtocolFactory::makeProtocolSpecificSettings(const VisitorVariant& visitor) const
{
  return makeProtocolSpecificSettings_T<AJAOutputSettings>(visitor);
}

void AJAProtocolFactory::serializeProtocolSpecificSettings(
    const QVariant& data, const VisitorVariant& visitor) const
{
  serializeProtocolSpecificSettings_T<AJAOutputSettings>(data, visitor);
}

bool AJAProtocolFactory::checkCompatibility(
    const Device::DeviceSettings& a, const Device::DeviceSettings& b) const noexcept
{
  auto a_aja = a.deviceSpecificSettings.value<AJAOutputSettings>();
  auto b_aja = b.deviceSpecificSettings.value<AJAOutputSettings>();
  return a_aja.deviceIndex == b_aja.deviceIndex && a_aja.channelIndex == b_aja.channelIndex;
}

// =============================================================================
// AJADevice
// =============================================================================

AJADevice::~AJADevice() = default;

void AJADevice::disconnect()
{
  GfxOutputDevice::disconnect();
  auto prev = std::move(m_dev);
  m_dev = {};
  deviceChanged(prev.get(), nullptr);
}

bool AJADevice::reconnect()
{
  disconnect();

  try
  {
    auto plug = m_ctx.findPlugin<DocumentPlugin>();
    if(plug)
    {
      auto set = settings().deviceSpecificSettings.value<AJAOutputSettings>();
      m_protocol = new gfx_protocol_base{plug->exec};
      m_dev = std::make_unique<aja_device>(
          set, std::unique_ptr<gfx_protocol_base>(m_protocol),
          settings().name.toStdString());
      deviceChanged(nullptr, m_dev.get());
    }
  }
  catch(std::exception& e)
  {
    qDebug() << "AJA device error: " << e.what();
  }
  catch(...)
  {
    qDebug() << "AJA device error";
  }

  return connected();
}

// =============================================================================
// AJASettingsWidget
// =============================================================================

AJASettingsWidget::AJASettingsWidget(QWidget* parent)
    : Device::ProtocolSettingsWidget(parent)
{
  m_layout = new QFormLayout(this);

  m_deviceNameEdit = new QLineEdit(this);
  m_deviceNameEdit->setText("AJA");
  m_layout->addRow(tr("Name"), m_deviceNameEdit);
  this->checkForChanges(m_deviceNameEdit);

  m_deviceCombo = new QComboBox(this);
  m_layout->addRow(tr("Device"), m_deviceCombo);

  m_channelCombo = new QComboBox(this);
  m_layout->addRow(tr("SDI Channel"), m_channelCombo);

  m_formatCombo = new QComboBox(this);
  m_layout->addRow(tr("Video Format"), m_formatCombo);

  m_pixelFormatCombo = new QComboBox(this);
  m_pixelFormatCombo->addItem("YCbCr 10-bit", "YCbCr10");
  m_pixelFormatCombo->addItem("YCbCr 8-bit", "YCbCr8");
  m_pixelFormatCombo->addItem("RGB 8-bit", "RGB8");
  m_pixelFormatCombo->addItem("RGB 10-bit", "RGB10");
  m_pixelFormatCombo->addItem("RGB 12-bit", "RGB12");
  m_pixelFormatCombo->addItem("RGB 12-bit packed", "RGB12P");
  m_pixelFormatCombo->addItem("RGB 10-bit DPX", "RGB10DPX");
  m_pixelFormatCombo->addItem("RGB 24-bit", "RGB24");
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
  m_8kModeCombo->addItem(tr("Disabled (HD/4K)"), static_cast<int>(AJA8KMode::Disabled));
  m_8kModeCombo->addItem(tr("8K TSI (2 FrameStores)"), static_cast<int>(AJA8KMode::TSI));
  m_8kModeCombo->addItem(tr("8K Squares (4 FrameStores)"), static_cast<int>(AJA8KMode::Squares));
  m_8kModeCombo->setToolTip(tr("8K mode requires alternative firmware. TSI uses 2 FrameStores with TSI muxers, Squares uses 4 FrameStores in quadrant mode."));
  m_layout->addRow(tr("8K Mode"), m_8kModeCombo);

  m_hdrModeCombo = new QComboBox(this);
  m_hdrModeCombo->addItem(tr("SDR (BT.709 limited)"), static_cast<int>(AJAHDRMode::Off));
  m_hdrModeCombo->addItem(tr("HDR10 (BT.2020 + PQ)"), static_cast<int>(AJAHDRMode::HDR10));
  m_hdrModeCombo->addItem(tr("HLG (BT.2020 + ARIB STD-B67)"), static_cast<int>(AJAHDRMode::HLG));
  m_hdrModeCombo->setToolTip(tr(
      "HDR forces RGBA16F intermediate render and 10-bit YCbCr output. "
      "PQ/HLG metadata is programmed into AJA's HDR registers (HDMI) and "
      "inserted as ANC packets (SDI)."));
  m_layout->addRow(tr("HDR"), m_hdrModeCombo);

  m_outputRingDepth = new QSpinBox(this);
  // 0 == "Auto": the AutoCirculate ring holds up to 7 frames, but the pump
  // caps in-flight depth at 2 by default for lowest latency. Explicit 1..7
  // lets the user trade latency for drop-safety: each extra queued frame adds
  // one frame period of end-to-end latency (16.7 ms @ 60p, 40 ms @ 25p) but
  // absorbs that much more host-side jitter before the card underruns.
  m_outputRingDepth->setRange(0, 7);
  m_outputRingDepth->setValue(0);
  m_outputRingDepth->setSpecialValueText(tr("Auto (2 frames)"));
  m_outputRingDepth->setToolTip(tr(
      "Max in-flight output frames queued to the card's AutoCirculate ring.\n"
      "Auto (2) gives the lowest latency. Higher values add one frame period "
      "of latency each (16.7 ms at 60p, 40 ms at 25p) but tolerate more host "
      "jitter before dropping. Overridden by the SCORE_AJA_OUT_DEPTH env var."));
  m_layout->addRow(tr("Output Ring Depth"), m_outputRingDepth);

  m_rdmaCheckbox = new QCheckBox(tr("Enable GPUDirect RDMA"), this);
  m_rdmaCheckbox->setChecked(true);
  m_rdmaCheckbox->setToolTip(tr("Use GPU direct memory access for lower latency (requires NVIDIA GPU)"));
  m_layout->addRow(m_rdmaCheckbox);

  // Connect signals
  connect(
      m_deviceCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
      &AJASettingsWidget::updateChannelList);
  connect(
      m_deviceCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
      &AJASettingsWidget::updateFormatList);
  connect(
      m_deviceCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
      &AJASettingsWidget::update8KModeVisibility);
  connect(
      m_8kModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
      [this](int) { updateFormatList(m_deviceCombo->currentIndex()); });

  refreshDeviceList();
}

Device::DeviceSettings AJASettingsWidget::getSettings() const
{
  Device::DeviceSettings settings;
  settings.name = m_deviceNameEdit->text();
  settings.protocol = AJAProtocolFactory::static_concreteKey();

  AJAOutputSettings aja;
  aja.deviceName = m_deviceNameEdit->text();
  aja.deviceIndex = m_deviceCombo->currentData().toInt();
  aja.channelIndex = m_channelCombo->currentData().toInt();
  aja.videoFormat = m_formatCombo->currentData().toString();
  aja.pixelFormat = m_pixelFormatCombo->currentData().toString();
  aja.width = m_width->value();
  aja.height = m_height->value();
  aja.rate = m_rate->value();
  aja.useRDMA = m_rdmaCheckbox->isChecked();
  aja.outputRingDepth = m_outputRingDepth->value();
  aja.mode8K = static_cast<AJA8KMode>(m_8kModeCombo->currentData().toInt());
  aja.hdrMode = static_cast<AJAHDRMode>(m_hdrModeCombo->currentData().toInt());

  settings.deviceSpecificSettings = QVariant::fromValue(aja);
  return settings;
}

void AJASettingsWidget::setSettings(const Device::DeviceSettings& settings)
{
  m_deviceNameEdit->setText(settings.name);

  auto aja = settings.deviceSpecificSettings.value<AJAOutputSettings>();

  // Find and select device
  int deviceIdx = m_deviceCombo->findData(aja.deviceIndex);
  if(deviceIdx >= 0)
    m_deviceCombo->setCurrentIndex(deviceIdx);

  // Find and select channel
  int channelIdx = m_channelCombo->findData(aja.channelIndex);
  if(channelIdx >= 0)
    m_channelCombo->setCurrentIndex(channelIdx);

  // Find and select format
  int formatIdx = m_formatCombo->findData(aja.videoFormat);
  if(formatIdx >= 0)
    m_formatCombo->setCurrentIndex(formatIdx);

  // Find and select pixel format
  int pixelFormatIdx = m_pixelFormatCombo->findData(aja.pixelFormat);
  if(pixelFormatIdx >= 0)
    m_pixelFormatCombo->setCurrentIndex(pixelFormatIdx);

  m_width->setValue(aja.width);
  m_height->setValue(aja.height);
  m_rate->setValue(static_cast<int>(aja.rate));
  m_outputRingDepth->setValue(aja.outputRingDepth);
  m_rdmaCheckbox->setChecked(aja.useRDMA);

  // Find and select 8K mode
  int mode8KIdx = m_8kModeCombo->findData(static_cast<int>(aja.mode8K));
  if(mode8KIdx >= 0)
    m_8kModeCombo->setCurrentIndex(mode8KIdx);

  // Find and select HDR mode
  int hdrIdx = m_hdrModeCombo->findData(static_cast<int>(aja.hdrMode));
  if(hdrIdx >= 0)
    m_hdrModeCombo->setCurrentIndex(hdrIdx);
}

void AJASettingsWidget::refreshDeviceList()
{
  m_deviceCombo->clear();

  CNTV2DeviceScanner scanner;
  const int numDevices = static_cast<int>(scanner.GetNumDevices());

  if(numDevices == 0)
  {
    m_deviceCombo->addItem(tr("No AJA devices found"), -1);
    m_deviceCombo->setEnabled(false);
    return;
  }

  m_deviceCombo->setEnabled(true);

  for(int i = 0; i < numDevices; ++i)
  {
    NTV2DeviceInfo info;
    if(scanner.GetDeviceInfo(i, info))
    {
      QString name = QString::fromStdString(info.deviceIdentifier);
      m_deviceCombo->addItem(name, i);
    }
  }

  // Update dependent lists
  if(m_deviceCombo->count() > 0)
  {
    updateChannelList(0);
    updateFormatList(0);
    update8KModeVisibility(0);
  }
}

void AJASettingsWidget::updateChannelList(int deviceIndex)
{
  m_channelCombo->clear();

  if(deviceIndex < 0)
    return;

  int devIdx = m_deviceCombo->itemData(deviceIndex).toInt();
  if(devIdx < 0)
    return;

  CNTV2Card card(static_cast<UWord>(devIdx));
  if(!card.IsOpen())
    return;

  // Query number of output channels
  const ULWord numOutputs = card.features().GetNumVideoOutputs();

  for(ULWord i = 0; i < numOutputs; ++i)
  {
    m_channelCombo->addItem(QString("SDI Out %1").arg(i + 1), static_cast<int>(i));
  }
}

void AJASettingsWidget::updateFormatList(int deviceIndex)
{
  m_formatCombo->clear();

  if(deviceIndex < 0)
    return;

  // Check if 8K mode is selected
  AJA8KMode mode8K = static_cast<AJA8KMode>(m_8kModeCombo->currentData().toInt());

  if(mode8K != AJA8KMode::Disabled)
  {
    // 8K/UHD2 formats (7680x4320)
    m_formatCombo->addItem("UHD2 23.98 (7680x4320)", "UHD2_2398");
    m_formatCombo->addItem("UHD2 24 (7680x4320)", "UHD2_24");
    m_formatCombo->addItem("UHD2 25 (7680x4320)", "UHD2_25");
    m_formatCombo->addItem("UHD2 29.97 (7680x4320)", "UHD2_2997");
    m_formatCombo->addItem("UHD2 30 (7680x4320)", "UHD2_30");
    m_formatCombo->addItem("UHD2 50 (7680x4320)", "UHD2_50");
    m_formatCombo->addItem("UHD2 59.94 (7680x4320)", "UHD2_5994");
    m_formatCombo->addItem("UHD2 60 (7680x4320)", "UHD2_60");

    // 8K DCI formats (8192x4320)
    m_formatCombo->addItem("8K DCI 23.98 (8192x4320)", "8K_2398");
    m_formatCombo->addItem("8K DCI 24 (8192x4320)", "8K_24");
    m_formatCombo->addItem("8K DCI 25 (8192x4320)", "8K_25");
    m_formatCombo->addItem("8K DCI 29.97 (8192x4320)", "8K_2997");
    m_formatCombo->addItem("8K DCI 30 (8192x4320)", "8K_30");
    m_formatCombo->addItem("8K DCI 47.95 (8192x4320)", "8K_4795");
    m_formatCombo->addItem("8K DCI 48 (8192x4320)", "8K_48");
    m_formatCombo->addItem("8K DCI 50 (8192x4320)", "8K_50");
    m_formatCombo->addItem("8K DCI 59.94 (8192x4320)", "8K_5994");
    m_formatCombo->addItem("8K DCI 60 (8192x4320)", "8K_60");

    // Default to UHD2 @ 59.94
    int idx = m_formatCombo->findData("UHD2_5994");
    if(idx >= 0)
      m_formatCombo->setCurrentIndex(idx);
  }
  else
  {
    // Standard HD/4K formats
    m_formatCombo->addItem("1080p 23.98", "1080p2398");
    m_formatCombo->addItem("1080p 24", "1080p24");
    m_formatCombo->addItem("1080p 25", "1080p25");
    m_formatCombo->addItem("1080p 29.97", "1080p2997");
    m_formatCombo->addItem("1080p 30", "1080p30");
    m_formatCombo->addItem("1080p 50", "1080p50");
    m_formatCombo->addItem("1080p 59.94", "1080p5994");
    m_formatCombo->addItem("1080p 60", "1080p60");
    m_formatCombo->addItem("1080i 50", "1080i50");
    m_formatCombo->addItem("1080i 59.94", "1080i5994");
    m_formatCombo->addItem("720p 50", "720p50");
    m_formatCombo->addItem("720p 59.94", "720p5994");
    m_formatCombo->addItem("720p 60", "720p60");
    m_formatCombo->addItem("UHD 23.98", "UHD2398");
    m_formatCombo->addItem("UHD 24", "UHD24");
    m_formatCombo->addItem("UHD 25", "UHD25");
    m_formatCombo->addItem("UHD 29.97", "UHD2997");
    m_formatCombo->addItem("UHD 50", "UHD50");
    m_formatCombo->addItem("UHD 59.94", "UHD5994");
    m_formatCombo->addItem("UHD 60", "UHD60");

    // Default to 1080p 59.94
    int idx = m_formatCombo->findData("1080p5994");
    if(idx >= 0)
      m_formatCombo->setCurrentIndex(idx);
  }
}

void AJASettingsWidget::update8KModeVisibility(int deviceIndex)
{
  if(deviceIndex < 0)
  {
    m_8kModeCombo->setEnabled(false);
    return;
  }

  int devIdx = m_deviceCombo->itemData(deviceIndex).toInt();
  if(devIdx < 0)
  {
    m_8kModeCombo->setEnabled(false);
    return;
  }

  CNTV2Card card(static_cast<UWord>(devIdx));
  if(!card.IsOpen())
  {
    m_8kModeCombo->setEnabled(false);
    return;
  }

  // Check if device supports 8K
  bool can8K = card.features().CanDo8KVideo();
  m_8kModeCombo->setEnabled(can8K);

  if(!can8K)
  {
    m_8kModeCombo->setCurrentIndex(0); // Reset to Disabled
  }
}

// =============================================================================
// AJADeviceEnumerator
// =============================================================================

void AJADeviceEnumerator::enumerate(
    std::function<void(const QString&, const Device::DeviceSettings&)> func) const
{
  CNTV2DeviceScanner scanner;
  const size_t numDevices = scanner.GetNumDevices();

  for(size_t i = 0; i < numDevices; ++i)
  {
    NTV2DeviceInfo info;
    if(scanner.GetDeviceInfo(static_cast<UWord>(i), info))
    {
      Device::DeviceSettings settings;
      settings.name = QString::fromStdString(info.deviceIdentifier);
      settings.protocol = AJAProtocolFactory::static_concreteKey();

      AJAOutputSettings aja;
      aja.deviceName = settings.name;
      aja.deviceIndex = static_cast<int>(i);
      aja.channelIndex = 0;
      aja.width = 1920;
      aja.height = 1080;
      aja.rate = 59.94;
      aja.videoFormat = "1080p5994";
      aja.pixelFormat = "YCbCr10";
      aja.useRDMA = true;

      settings.deviceSpecificSettings = QVariant::fromValue(aja);

      func(settings.name, settings);
    }
  }
}

} // namespace Gfx::AJA

// =============================================================================
// Serialization
// =============================================================================

template <>
void DataStreamReader::read(const Gfx::AJA::AJAOutputSettings& n)
{
  m_stream << n.deviceName << n.deviceIndex << n.channelIndex << n.width << n.height
           << n.rate << n.videoFormat << n.pixelFormat << n.useRDMA
           << static_cast<int>(n.mode8K) << static_cast<int>(n.hdrMode)
           << n.outputRingDepth;
  insertDelimiter();
}

template <>
void DataStreamWriter::write(Gfx::AJA::AJAOutputSettings& n)
{
  int mode8K = 0;
  int hdrMode = 0;
  m_stream >> n.deviceName >> n.deviceIndex >> n.channelIndex >> n.width >> n.height
      >> n.rate >> n.videoFormat >> n.pixelFormat >> n.useRDMA >> mode8K >> hdrMode
      >> n.outputRingDepth;
  n.mode8K = static_cast<Gfx::AJA::AJA8KMode>(mode8K);
  n.hdrMode = static_cast<Gfx::AJA::AJAHDRMode>(hdrMode);
  checkDelimiter();
}

template <>
void JSONReader::read(const Gfx::AJA::AJAOutputSettings& n)
{
  obj["DeviceName"] = n.deviceName;
  obj["DeviceIndex"] = n.deviceIndex;
  obj["ChannelIndex"] = n.channelIndex;
  obj["Width"] = n.width;
  obj["Height"] = n.height;
  obj["Rate"] = n.rate;
  obj["VideoFormat"] = n.videoFormat;
  obj["PixelFormat"] = n.pixelFormat;
  obj["UseRDMA"] = n.useRDMA;
  obj["Mode8K"] = static_cast<int>(n.mode8K);
  obj["HDRMode"] = static_cast<int>(n.hdrMode);
  obj["OutputRingDepth"] = n.outputRingDepth;
}

template <>
void JSONWriter::write(Gfx::AJA::AJAOutputSettings& n)
{
  n.deviceName = obj["DeviceName"].toString();
  n.deviceIndex = obj["DeviceIndex"].toInt();
  n.channelIndex = obj["ChannelIndex"].toInt();
  n.width = obj["Width"].toInt();
  n.height = obj["Height"].toInt();
  n.rate = obj["Rate"].toDouble();
  n.videoFormat = obj["VideoFormat"].toString();
  n.pixelFormat = obj["PixelFormat"].toString();
  n.useRDMA = obj["UseRDMA"].toBool();
  n.mode8K = static_cast<Gfx::AJA::AJA8KMode>(obj["Mode8K"].toInt());
  n.hdrMode = static_cast<Gfx::AJA::AJAHDRMode>(obj["HDRMode"].toInt());
  // Absent in projects saved before this field existed => 0 (auto), preserving
  // the prior fixed depth-2 behavior.
  if(auto it = obj.tryGet("OutputRingDepth"))
    n.outputRingDepth = it->toInt();
  else
    n.outputRingDepth = 0;
}

SCORE_SERALIZE_DATASTREAM_DEFINE(Gfx::AJA::AJAOutputSettings);
