#pragma once

/**
 * @file ArgusInputBackend.hpp
 * @brief Argus as a score DMACaptureBackend, alongside AJA/DeckLink/V4L2.
 *
 * Same contract as every other vendor: open() negotiates geometry, a capture
 * thread fills the strategy's slots and publishes into the ring, and the
 * renderer decodes the wire bytes with the shared unpacker shaders.
 *
 * What is specific to Argus:
 *   - the wire format is always NV12. Argus offers nothing else, which is why
 *     the planar work in the capture ladder had to land first: without it every
 *     frame fell to CPU staging.
 *   - the buffers are ours (see ArgusSession), so both zero-copy rungs are
 *     available: the dma-buf fd goes to DmaBufImportCapture, and the same
 *     surface host-mapped goes to BorrowedHostImportCapture where the driver
 *     will not import a dma-buf.
 *   - a slot may only be handed back to the ISP once the GPU has finished with
 *     it, which is exactly the borrowed-buffer contract those rungs arbitrate;
 *     the session's release loop is driven by their returned-slot bitmask.
 */

#include <argus/ArgusSession.hpp>
#include <argus/ArgusSettings.hpp>
#include <argus/ArgusSyncRig.hpp>

#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <memory>

namespace score::gfx::interop
{
struct DmaBufImportCapture;
struct BorrowedHostImportCapture;
}

namespace Gfx::Argus
{

class ArgusInputBackend final
    : public score::gfx::DMACaptureBackend
    , public ArgusRigMember
{
public:
  ArgusInputBackend(
      ArgusSettings settings, score::gfx::interop::VideoCaptureSlotRing& ring);
  ~ArgusInputBackend() override;

  bool open() override;
  int width() const noexcept override;
  int height() const noexcept override;
  std::uint32_t frameByteSize() const noexcept override;
  Video::ImageFormat imageFormat() const override;

  std::unique_ptr<score::gfx::GPUVideoDecoder>
  makeDecoder(Video::VideoMetadata& meta) override;

  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy> pickStrategy(
      QRhi::Implementation, const score::gfx::interop::GpuCapabilities&) override;

  std::vector<std::function<std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>()>>
  pickStrategies(
      QRhi::Implementation, const score::gfx::interop::GpuCapabilities&) override;

  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy>
  makeCpuStrategy() override;

  void setStrategy(score::gfx::interop::VideoCaptureStrategy*) noexcept override;

  void start() override;
  void stop() override;

  SyncMembership syncGroup() noexcept override;
  void setSyncGroupEngaged(bool b) noexcept override { m_syncEngaged = b; }

  /// The mode the session settled on, for harnesses and the settings UI.
  std::int32_t resolvedSensorMode() const noexcept;

  // ArgusRigMember: what the rig's capture thread needs from us.
  bool grouped() const noexcept override { return m_syncEngaged; }
  void publishUngrouped(std::size_t slot) override;
  std::uint32_t takeReturnedUngrouped() override;

private:
  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy> makeDmaBufRung();
  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy> makeBorrowedRung();

  /// This backend's slots: its sensor's stream of a shared session, or the only
  /// stream of its own.
  const std::vector<ArgusSlot>& mySlots() const noexcept;

  ArgusSettings m_settings;
  score::gfx::interop::VideoCaptureSlotRing& m_ring;

  /// Used when this camera stands alone. A rig member's session belongs to the
  /// rig, because a session is the sync domain and cannot be split across the
  /// nodes that share it.
  ArgusSession m_ownSession;
  /// Whichever of the two is in force; null before open().
  ArgusSession* m_session{};

  /// Shared with the other sensors of the same rig; null when standing alone.
  std::shared_ptr<ArgusRig> m_rig;
  /// Our stream within m_session. Zero for a lone camera and for a rig that
  /// fell back to one session per sensor.
  std::size_t m_stream{0};
  /// Whether the renderer takes our slots from the group. Settled during its
  /// init(), before start() lets any capture thread read it.
  bool m_syncEngaged{false};

  score::gfx::interop::VideoCaptureStrategy* m_strategy{};

  /// Decided in pickStrategies and read by makeDecoder, which runs after it.
  /// The external-image rung and its decoder are a matched pair: the sampler
  /// converts YUV itself, so neither works with the other's counterpart.
  bool m_wantExternal{false};

  /// Set when the engaged strategy honours the borrowed contract, i.e. it will
  /// tell us which slots may go back to the ISP. A strategy that does not (the
  /// CPU rung copies out) leaves this null and every slot is released at once.
  score::gfx::interop::DmaBufImportCapture* m_dmabuf{};
  score::gfx::interop::BorrowedHostImportCapture* m_borrowed{};
};

}
