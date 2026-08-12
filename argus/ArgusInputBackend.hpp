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

#include <Gfx/Graph/DMACaptureInputNode.hpp>

#include <memory>

namespace score::gfx::interop
{
struct DmaBufImportCapture;
struct BorrowedHostImportCapture;
}

namespace Gfx::Argus
{

class ArgusInputBackend final : public score::gfx::DMACaptureBackend
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

  /// The mode the session settled on, for harnesses and the settings UI.
  std::int32_t resolvedSensorMode() const noexcept;

private:
  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy> makeDmaBufRung();
  std::unique_ptr<score::gfx::interop::VideoCaptureStrategy> makeBorrowedRung();

  ArgusSettings m_settings;
  score::gfx::interop::VideoCaptureSlotRing& m_ring;
  ArgusSession m_session;

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
