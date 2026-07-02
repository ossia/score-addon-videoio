#pragma once
#include <Device/Protocol/DeviceInterface.hpp>
#include <Device/Protocol/DeviceSettings.hpp>
#include <Device/Protocol/ProtocolFactoryInterface.hpp>
#include <Device/Protocol/ProtocolSettingsWidget.hpp>

#include <Gfx/GfxDevice.hpp>

#include <VideoIOSettings.hpp>

#include <verdigris>

class QComboBox;
class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;
class QLineEdit;
class QFormLayout;

namespace Gfx::VideoIO
{

/**
 * @brief Unified "Direct Video I/O" playout protocol.
 *
 * One device that branches on the detected card: getEnumerators() returns one
 * enumerator per compiled-in vendor (AJA, DeckLink, ...); makeDevice/the widget
 * dispatch on VideoOutputSettings::vendor. No registry — hardcoded #if-gated
 * vendor set.
 */
class VideoOutputProtocolFactory final : public Device::ProtocolFactory
{
  SCORE_CONCRETE("d7e1c4a2-3b56-4f08-9c1d-6a8e2f0b71d4")

public:
  QString prettyName() const noexcept override;
  QString category() const noexcept override;
  QUrl manual() const noexcept override;

  Device::DeviceEnumerators
  getEnumerators(const score::DocumentContext& ctx) const override;

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

  QVariant
  makeProtocolSpecificSettings(const VisitorVariant& visitor) const override;

  void serializeProtocolSpecificSettings(
      const QVariant& data, const VisitorVariant& visitor) const override;

  bool checkCompatibility(
      const Device::DeviceSettings& a,
      const Device::DeviceSettings& b) const noexcept override;
};

class VideoOutputDevice final : public GfxOutputDevice
{
  W_OBJECT(VideoOutputDevice)

public:
  using GfxOutputDevice::GfxOutputDevice;
  ~VideoOutputDevice();

private:
  void disconnect() override;
  bool reconnect() override;
  ossia::net::device_base* getDevice() const override { return m_dev.get(); }

  gfx_protocol_base* m_protocol{};
  mutable std::unique_ptr<ossia::net::device_base> m_dev;
};

class VideoOutputSettingsWidget final : public Device::ProtocolSettingsWidget
{
public:
  VideoOutputSettingsWidget(QWidget* parent = nullptr);

  Device::DeviceSettings getSettings() const override;
  void setSettings(const Device::DeviceSettings& settings) override;

private:
  Vendor currentVendor() const;
  void onVendorChanged();
  void refreshDeviceList();
  void updateChannelList(int deviceIndex);
  void updateFormatList();
  void updatePixelFormatList();

  QFormLayout* m_layout{};
  QLineEdit* m_deviceNameEdit{};
  QComboBox* m_vendorCombo{};
  QComboBox* m_deviceCombo{};
  QComboBox* m_channelCombo{};
  QComboBox* m_formatCombo{};
  QComboBox* m_pixelFormatCombo{};
  QComboBox* m_8kModeCombo{};
  QComboBox* m_hdrModeCombo{};
  QSpinBox* m_width{};
  QSpinBox* m_height{};
  QDoubleSpinBox* m_rate{};
  QCheckBox* m_rdmaCheckbox{};
};

} // namespace Gfx::VideoIO
