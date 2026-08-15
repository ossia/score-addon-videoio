#pragma once
#include <Device/Protocol/DeviceInterface.hpp>
#include <Device/Protocol/DeviceSettings.hpp>
#include <Device/Protocol/ProtocolFactoryInterface.hpp>
#include <Device/Protocol/ProtocolSettingsWidget.hpp>

#include <Gfx/GfxInputDevice.hpp>

#include <VideoIOSettings.hpp>

#include <verdigris>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLineEdit;

namespace Gfx::VideoIO
{

/**
 * @brief Unified "Direct Video I/O" capture protocol (AJA + DeckLink today).
 *
 * Per-vendor enumerators; makeDevice/the widget dispatch on
 * VideoInputSettings::vendor. The device picks the GPU-direct or CPU-staging
 * path per vendor (AJA supports both; DeckLink is host-staged today).
 */
class VideoInputProtocolFactory final : public Device::ProtocolFactory
{
  SCORE_CONCRETE("b3f0a91c-7d24-4e6b-8a3f-1c5e9d2074af")

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

class VideoInputDevice final : public Gfx::GfxInputDevice
{
  W_OBJECT(VideoInputDevice)

public:
  using Gfx::GfxInputDevice::GfxInputDevice;
  ~VideoInputDevice();

private:
  void disconnect() override;
  bool reconnect() override;
  ossia::net::device_base* getDevice() const override { return m_dev.get(); }

  Gfx::video_texture_input_protocol* m_protocol{};
  mutable std::unique_ptr<ossia::net::device_base> m_dev;
};

class VideoInputSettingsWidget final : public Device::ProtocolSettingsWidget
{
public:
  VideoInputSettingsWidget(QWidget* parent = nullptr);

  Device::DeviceSettings getSettings() const override;
  void setSettings(const Device::DeviceSettings& settings) override;

private:
  Vendor currentVendor() const;
  void onVendorChanged();
  void refreshDeviceList();
  void updateFormatList();

  QFormLayout* m_layout{};
  QLineEdit* m_deviceNameEdit{};
  QLineEdit* m_rigPathsEdit{};
  QComboBox* m_vendorCombo{};
  QComboBox* m_deviceCombo{};
  QComboBox* m_channelCombo{};
  QComboBox* m_formatCombo{};
  QComboBox* m_pixelFormatCombo{};
  QComboBox* m_resolutionModeCombo{};
  QComboBox* m_routingModeCombo{};
  QCheckBox* m_rdmaCheckbox{};
};

} // namespace Gfx::VideoIO
