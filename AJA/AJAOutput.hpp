#pragma once
#include <Device/Protocol/DeviceInterface.hpp>
#include <Device/Protocol/DeviceSettings.hpp>
#include <Device/Protocol/ProtocolFactoryInterface.hpp>
#include <Device/Protocol/ProtocolSettingsWidget.hpp>

#include <Gfx/GfxDevice.hpp>

#include <ntv2enums.h>

#include <verdigris>

class QComboBox;
class QCheckBox;
class QSpinBox;
class QFormLayout;

namespace Gfx::AJA
{

// 8K routing mode
enum class AJA8KMode
{
  Disabled, // Standard HD/4K mode
  TSI,      // Two-Sample Interleave (2 FrameStores + TSI muxers)
  Squares   // Quadrant mode (4 FrameStores in 2x2 grid)
};

// HDR transfer / colorimetry. SDR uses BT.709 limited range; HDR10 and HLG
// switch the encoder color matrix to BT.2020 + PQ / HLG OETF and (where
// supported) program the AJA card's HDR metadata registers / SDI ANC.
enum class AJAHDRMode
{
  Off = 0, // BT.709 limited (SDR)
  HDR10,   // BT.2020 + SMPTE ST 2084 (PQ)
  HLG      // BT.2020 + ARIB STD-B67 (Hybrid Log-Gamma)
};

// AJA-specific output settings
struct AJAOutputSettings
{
  QString deviceName;
  int deviceIndex{0};
  int channelIndex{0};
  int width{1920};
  int height{1080};
  double rate{59.94};
  QString videoFormat{"1080p5994"};
  // When set (!= UNKNOWN), drives the format directly and bypasses the
  // videoFormat-string parse — lets callers select ANY NTV2VideoFormat the
  // firmware supports without needing a matching string in parseVideoFormat.
  NTV2VideoFormat videoFormatEnum{NTV2_FORMAT_UNKNOWN};
  QString pixelFormat{"YCbCr10"};
  bool useRDMA{true};
  AJA8KMode mode8K{AJA8KMode::Disabled};
  AJAHDRMode hdrMode{AJAHDRMode::Off};
  // Max in-flight AutoCirculate output frames the pump keeps queued.
  // 0 = auto (engine default of 2, lowest safe latency). Higher values trade
  // one frame period of end-to-end latency per unit (16.7 ms @ 60p, 40 ms @
  // 25p) for more drop headroom under host jitter. Capped to the AC ring size.
  // The SCORE_AJA_OUT_DEPTH env var overrides this for debugging.
  int outputRingDepth{0};
};

// Protocol factory for AJA SDI output
class AJAProtocolFactory final : public Device::ProtocolFactory
{
  SCORE_CONCRETE("3205ff94-15eb-4d31-82fa-9939cd07821c")

public:
  QString prettyName() const noexcept override;
  QString category() const noexcept override;
  QUrl manual() const noexcept override;

  Device::DeviceEnumerators getEnumerators(const score::DocumentContext& ctx) const override;

  Device::DeviceInterface* makeDevice(
      const Device::DeviceSettings& settings,
      const Explorer::DeviceDocumentPlugin& doc,
      const score::DocumentContext& ctx) override;

  const Device::DeviceSettings& defaultSettings() const noexcept override;

  Device::ProtocolSettingsWidget* makeSettingsWidget() override;

  Device::AddressDialog* makeAddAddressDialog(
      const Device::DeviceInterface& dev, const score::DocumentContext& ctx,
      QWidget* parent) override;
  Device::AddressDialog* makeEditAddressDialog(
      const Device::AddressSettings&, const Device::DeviceInterface& dev,
      const score::DocumentContext& ctx, QWidget*) override;

  QVariant makeProtocolSpecificSettings(const VisitorVariant& visitor) const override;

  void serializeProtocolSpecificSettings(
      const QVariant& data, const VisitorVariant& visitor) const override;

  bool checkCompatibility(
      const Device::DeviceSettings& a,
      const Device::DeviceSettings& b) const noexcept override;
};

// AJA output device
class AJADevice final : public GfxOutputDevice
{
  W_OBJECT(AJADevice)

public:
  using GfxOutputDevice::GfxOutputDevice;
  ~AJADevice();

private:
  void disconnect() override;
  bool reconnect() override;
  ossia::net::device_base* getDevice() const override { return m_dev.get(); }

  gfx_protocol_base* m_protocol{};
  mutable std::unique_ptr<ossia::net::device_base> m_dev;
};

// Settings widget for AJA output configuration
class AJASettingsWidget final : public Device::ProtocolSettingsWidget
{
public:
  AJASettingsWidget(QWidget* parent = nullptr);

  Device::DeviceSettings getSettings() const override;
  void setSettings(const Device::DeviceSettings& settings) override;

private:
  void refreshDeviceList();
  void updateChannelList(int deviceIndex);
  void updateFormatList(int deviceIndex);
  void update8KModeVisibility(int deviceIndex);

  QFormLayout* m_layout{};
  QLineEdit* m_deviceNameEdit{};
  QComboBox* m_deviceCombo{};
  QComboBox* m_channelCombo{};
  QComboBox* m_formatCombo{};
  QComboBox* m_pixelFormatCombo{};
  QComboBox* m_8kModeCombo{};
  QComboBox* m_hdrModeCombo{};
  QSpinBox* m_width{};
  QSpinBox* m_height{};
  QSpinBox* m_rate{};
  QSpinBox* m_outputRingDepth{};
  QCheckBox* m_rdmaCheckbox{};
};

// Device enumerator for auto-discovery
class AJADeviceEnumerator final : public Device::DeviceEnumerator
{
public:
  void enumerate(
      std::function<void(const QString&, const Device::DeviceSettings&)> func) const override;
};

} // namespace Gfx::AJA

// Serialization declarations
SCORE_SERIALIZE_DATASTREAM_DECLARE(, Gfx::AJA::AJAOutputSettings);
Q_DECLARE_METATYPE(Gfx::AJA::AJAOutputSettings)
W_REGISTER_ARGTYPE(Gfx::AJA::AJAOutputSettings)
