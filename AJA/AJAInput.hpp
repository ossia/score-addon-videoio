#pragma once
#include <Device/Protocol/DeviceInterface.hpp>
#include <Device/Protocol/DeviceSettings.hpp>
#include <Device/Protocol/ProtocolFactoryInterface.hpp>
#include <Device/Protocol/ProtocolSettingsWidget.hpp>

#include <Gfx/GfxInputDevice.hpp>

#include <ntv2enums.h>

#include <verdigris>

#include <memory>

class QComboBox;
class QFormLayout;
class QSpinBox;
class QLineEdit;

namespace Video
{
class ExternalInput;
}

namespace Gfx::AJA
{

struct AJAInputSettings;

/// CPU-staging capture factory (defined in AJAInput.cpp): opens the AJA card and
/// returns a Video::ExternalInput feeding score's camera pipeline, or null on
/// failure. Declared here so the unified Direct Video I/O device can reuse it.
std::shared_ptr<::Video::ExternalInput> makeAJACapture(const AJAInputSettings&);

/**
 * @brief Per-frame pixel format the AJA card delivers to score.
 *
 * Mirror of NTV2_FBF_*; we expose the formats the score gfx pipeline
 * can decode without a CPU-side unpack.
 *  - YCbCr8:  NTV2_FBF_8BIT_YCBCR   (UYVY 4:2:2, 16 bpp). AV_PIX_FMT_UYVY422.
 *  - YCbCr10: NTV2_FBF_10BIT_YCBCR  (v210, 10-bit 4:2:2, 6 pixels per
 *             16 bytes). The native SDI 10-bit format. GPU-direct path
 *             only — score's v210 fragment-shader decoder unpacks the
 *             bytes on the GPU. AVFrame staging not implemented for
 *             this format.
 *  - ARGB:    NTV2_FBF_ARGB         (32 bpp BGRA byte order on LE hosts).
 *  - RGBA:    NTV2_FBF_ABGR         (32 bpp RGBA byte order on LE hosts —
 *             the SDK's NTV2_FBF_RGBA stores [A,R,G,B], not [R,G,B,A]).
 */
enum class AJAInputPixelFormat
{
  YCbCr8 = 0, // UYVY 4:2:2
  ARGB,       // 32-bit BGRA byte order
  RGBA,       // 32-bit RGBA byte order
  YCbCr10     // v210 10-bit 4:2:2 (GPU-direct only)
};

/**
 * @brief Multi-link SDI input topology.
 *
 *  - Auto: detect the resolution from the master channel's input
 *    signal and pick the smallest set of channels that holds it
 *    (1080p → SingleLink, 4K via 12G → Quad4K, 8K via 4×12G → Quad8K).
 *  - SingleLink: force one channel even if the signal is bigger
 *    (truncates / fails). Useful for explicit testing.
 *  - Quad4K: 4 SDI inputs delivering one 4K frame. Combined with
 *    `routingMode` to pick between SQD (4 frame stores, one quadrant
 *    each) and TSI (2 frame stores, sample-interleaved across 4 SDIs
 *    via the TSI muxer).
 *  - Quad8K: 4 SDI inputs each carrying a 4K quadrant of the 8K frame.
 *    Requires QuadQuad-mode-capable hardware (Kona 5, Corvid 88, etc.).
 *    SQD uses 4 frame stores; TSI uses 2 frame stores (4 SDIs at HFR,
 *    2 SDIs below 50p).
 */
enum class AJAInputResolutionMode
{
  Auto = 0,
  SingleLink,
  Quad4K,
  Quad8K
};

/**
 * @brief Multi-link routing topology (SQD vs TSI).
 *
 *  - SQD (Square Division): 4 SDIs → 4 frame stores, each holding one
 *    quadrant of the full image. Direct, no muxers, simplest.
 *  - TSI (Two-Sample Interleave): 4 SDIs → 2 TSI muxers → 2 frame stores
 *    with sample-interleaved layout. Halves the number of frame stores
 *    needed; required by some sources / pipelines that emit TSI rather
 *    than quadrant stripes.
 */
enum class AJAInputRoutingMode
{
  SQD = 0,
  TSI
};

/// AJA SDI capture configuration. Matches AJAOutputSettings shape so the
/// settings widget code can stay symmetric.
struct AJAInputSettings
{
  QString deviceName;
  int deviceIndex{0};
  int channelIndex{0};
  /// Expected source format. The capture loop verifies the actual SDI
  /// signal matches at startup; if width/height differ from what the
  /// signal reports we use the signal's reported geometry instead.
  QString videoFormat{"1080p5994"};
  /// When set (!= UNKNOWN), used as the configured/expected format directly
  /// (bypasses the videoFormat-string parse) if signal auto-detection fails —
  /// lets callers select ANY NTV2VideoFormat without a matching parse string.
  NTV2VideoFormat videoFormatEnum{NTV2_FORMAT_UNKNOWN};
  AJAInputPixelFormat pixelFormat{AJAInputPixelFormat::YCbCr8};
  AJAInputResolutionMode resolutionMode{AJAInputResolutionMode::Auto};
  AJAInputRoutingMode routingMode{AJAInputRoutingMode::SQD};
  /// Enable the GPU-direct capture path: AJA -> page-locked sysmem
  /// (capture thread) -> DVP DMA -> QRhi texture, no AVFrame upload
  /// pass. Falls back to AVFrame + GPUVideoDecoder if the strategy
  /// can't initialize at runtime (no dvp.dll, unsupported backend).
  /// Default true; flip off to force the CPU-staging path for
  /// debugging or on systems without GPUDirect for Video.
  bool useRDMA{true};
};

class AJAInputProtocolFactory final
    : public Device::ProtocolFactory
{
  SCORE_CONCRETE("0e8c7c1a-21e6-4d37-9a4f-2f2c8d0e7a31")

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

class AJAInputDevice final : public Gfx::GfxInputDevice
{
  W_OBJECT(AJAInputDevice)

public:
  using Gfx::GfxInputDevice::GfxInputDevice;
  ~AJAInputDevice();

private:
  void disconnect() override;
  bool reconnect() override;
  ossia::net::device_base* getDevice() const override { return m_dev.get(); }

  /// Either video_texture_input_protocol (CPU-staging path) or null
  /// (GPU-direct path uses simple_texture_input_protocol which lives
  /// inside m_dev). Non-owning; protocol object's lifetime is owned by
  /// the device.
  Gfx::video_texture_input_protocol* m_protocol{};

  /// Holds either a video_texture_input_device (CPU-staging) or a
  /// simple_texture_input_device (GPU-direct AJAInputNode). The
  /// device kind is resolved at reconnect() time based on the
  /// useRDMA setting.
  mutable std::unique_ptr<ossia::net::device_base> m_dev;
};

class AJAInputSettingsWidget final
    : public Device::ProtocolSettingsWidget
{
public:
  AJAInputSettingsWidget(QWidget* parent = nullptr);

  Device::DeviceSettings getSettings() const override;
  void setSettings(const Device::DeviceSettings& settings) override;

private:
  void refreshDeviceList();
  void updateChannelList(int deviceIndex);

  QFormLayout* m_layout{};
  QLineEdit* m_deviceNameEdit{};
  QComboBox* m_deviceCombo{};
  QComboBox* m_channelCombo{};
  QComboBox* m_formatCombo{};
  QComboBox* m_pixelFormatCombo{};
  QComboBox* m_resolutionModeCombo{};
  QComboBox* m_routingModeCombo{};
};

class AJAInputDeviceEnumerator final
    : public Device::DeviceEnumerator
{
public:
  void enumerate(std::function<void(const QString&, const Device::DeviceSettings&)>
                     func) const override;
};

} // namespace Gfx::AJA

SCORE_SERIALIZE_DATASTREAM_DECLARE(, Gfx::AJA::AJAInputSettings);
Q_DECLARE_METATYPE(Gfx::AJA::AJAInputSettings)
W_REGISTER_ARGTYPE(Gfx::AJA::AJAInputSettings)
