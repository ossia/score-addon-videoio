#pragma once

/**
 * @file AjaOutputBackend.hpp
 * @brief AJA-specific device state + setup for the playout path.
 *
 * Extracted from AJANode so the device code (open, channel/routing, quad-link/
 * TSI/12G topology, VPID, HDR ANC) lives apart from the QRhi/render-loop node.
 * Step toward the shared score::gfx::DirectVideoOutputBackend seam (Direct Video
 * I/O Phase A): once the consumer thread is folded into pacing hooks this class
 * will implement that interface and the node will become vendor-neutral.
 *
 * Members are public: this is an internal vendor impl class owned by AJANode.
 */

#include <AJA/AJAOutput.hpp>
#include <Gfx/Graph/DirectVideoOutputBackend.hpp>

#include <ntv2card.h>
#include <ntv2enums.h>
#include <ntv2formatdescriptor.h>
#include <ntv2publicinterface.h>
#include <ntv2signalrouter.h>

#include <ossia/detail/flat_map.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class QRhi;

namespace Gfx::AJA
{

class AjaOutputBackend final : public score::gfx::DirectVideoOutputBackend
{
public:
  explicit AjaOutputBackend(const AJAOutputSettings& settings);
  ~AjaOutputBackend() override;

  AjaOutputBackend(const AjaOutputBackend&) = delete;
  AjaOutputBackend& operator=(const AjaOutputBackend&) = delete;

  // -- DirectVideoOutputBackend ----------------------------------------------
  bool open(score::gfx::GraphicsApi) override { return initializeAJADevice(); }
  void close() override { shutdownAJADevice(); }

  int width() const noexcept override { return m_settings.width; }
  int height() const noexcept override { return m_settings.height; }
  double frameRate() const noexcept override { return m_settings.rate; }
  bool isOpen() const noexcept override { return m_deviceInitialized; }
  uint32_t frameByteSize() const noexcept override { return m_frameBufferSize; }
  int visibleRows() const noexcept override;
  score::gfx::interop::VideoPixelFormat wireFormat() const noexcept override;
  score::gfx::interop::VideoPixelFormat encoderFormat() const noexcept override;
  bool prefersFloatRender() const noexcept override;
  QString colorConversion() const override;
  std::vector<score::gfx::interop::HostStagedPlane> planes() const override;
  score::gfx::interop::VendorDmaRegistrar registrar() override;
  score::gfx::DirectVideoOutputBackend::CustomStage customStage() override;
  std::vector<std::function<std::unique_ptr<score::gfx::interop::VideoOutputStrategy>()>>
  gpuDirectCandidates(QRhi* rhi, score::gfx::GraphicsApi api) override;
  score::gfx::interop::PacedFramePump::Hooks pacingHooks() override;

  /// Open the card + set the video standard, link/routing, VPID and HDR ANC.
  /// Returns false on failure. Geometry/format valid only after a true return.
  bool initializeAJADevice();
  /// Stop AutoCirculate, restore task mode, close the card.
  void shutdownAJADevice();

  // -- AJA device state (public: owned-by-node impl detail) ------------------
  AJAOutputSettings m_settings;

  std::unique_ptr<CNTV2Card> m_card;
  NTV2Channel m_channel{NTV2_CHANNEL1};
  NTV2VideoFormat m_videoFormat{NTV2_FORMAT_1080p_5994_A};
  NTV2FrameBufferFormat m_bufferFormat{NTV2_FBF_10BIT_YCBCR};
  NTV2FormatDescriptor m_formatDesc;
  uint32_t m_frameBufferSize{0};

  /// Saved retail-services task mode; restored on close.
  NTV2EveryFrameTaskMode m_savedTaskMode{NTV2_STANDARD_TASKS};
  bool m_taskModeSaved{false};
  /// Active SDI spigots / frame stores.
  NTV2ChannelSet m_activeSDIs;
  NTV2ChannelSet m_activeFrameStores;
  /// Single-link 12G UHD (vs SQD/TSI 4-cable, vs sub-4K).
  bool m_use12G{false};
  /// TSI muxers (4K TSI YUV / 8K TSI).
  bool m_useTSI{false};
  /// 4K UHD (single- or quad-link), distinct from 12G and the 8K QuadQuad family.
  bool m_is4K{false};
  /// 8K quad-quad.
  bool m_isQuadQuad{false};

  static constexpr uint16_t kFrameCount{7}; // AJA-recommended AC depth

  /// Resolved max in-flight AutoCirculate output frames (see cardCanAccept).
  /// -1 until first computed; precedence env > user setting > default(2).
  int m_maxQueuedDepth{-1};

  /// Pre-built HDR Static Metadata Descriptor ANC packet (page-aligned).
  /// Empty unless settings.hdrMode != Off.
  std::vector<uint32_t> m_hdrAncBuffer;

  bool m_deviceInitialized{false};

  /// v210 framestore at a width not divisible by 6: the GPU V210Encoder can't
  /// handle it, so we emit UYVY on the GPU and pack to v210 in customStage.
  bool m_v210NeedsCpuPack{false};

private:
  /// Build the SDI-out signal routing for the active topology.
  void buildSignalRoute(NTV2XptConnections& outConnections);
  /// Build + write VPID payload bytes to each active SDI output spigot.
  void writeOutputVPIDs();

  // -- PacedFramePump hooks (run on the pump's consumer thread) ---------------
  bool waitForVBI();
  bool cardCanAccept();
  bool submitFrame(void* framePtr);

  /// Per-VBI free-running RP188 LTC counter + loop-invariant TC constants.
  uint64_t m_outputFrame{0};
  uint32_t m_framesPerSec{1};
  bool m_dropFrame{false};
  /// Preallocated SDI LTC map (overwritten in place each VBI, no per-frame alloc).
  ossia::flat_map<NTV2TCIndex, NTV2_RP188> m_tcMap;
  /// Reused AutoCirculateTransfer (re-pointed each frame; avoids per-VBI ctor).
  AUTOCIRCULATE_TRANSFER m_xfer;
  uint32_t m_acGoodXfers{0};
  bool m_acStarted{false};
};

} // namespace Gfx::AJA
