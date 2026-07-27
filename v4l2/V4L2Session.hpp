#pragma once

/**
 * @file V4L2Session.hpp
 * @brief V4L2 capture device + buffer lifecycle, for both zero-copy ingress modes.
 *
 * Two ways to get a V4L2 frame onto the GPU without a CPU copy, both exposed
 * here behind one buffer-slot API:
 *
 *   - `BufferMode::MmapExport` — REQBUFS(V4L2_MEMORY_MMAP), then VIDIOC_EXPBUF
 *     per buffer to obtain a DMA-BUF fd. The *driver* allocates; we import.
 *     Works on any driver that implements export (vivid, uvcvideo, most SoC
 *     camera drivers). EXPBUF is documented as MMAP-only and one call per
 *     plane; the fd is ours to close.
 *
 *   - `BufferMode::DmaBufImport` — REQBUFS(V4L2_MEMORY_DMABUF) and we supply
 *     the fds, one per slot, via `v4l2_buffer::m.fd` (single-planar) or
 *     `m.planes[i].m.fd` (multi-planar). *We* allocate, through a
 *     `DmaBufAllocator`, which is what lets the buffer carry a DRM format
 *     modifier the GPU is happy to sample directly — the case that matters on
 *     Tegra, where the wrong modifier silently costs a blit.
 *
 *   - `BufferMode::MmapRead` — plain mmap, CPU-visible. The portable fallback
 *     rung when neither of the above is available.
 *
 * The API permits a different fd at every QBUF, but we deliberately pin one fd
 * per slot for the session's lifetime so videobuf2 keeps its cached mapping
 * instead of re-attaching the dma_buf on every frame.
 *
 * Buffer ownership is the part that differs from the card vendors (AJA,
 * Magewell, DeckLink), where we own the memory outright: a V4L2 buffer is
 * BORROWED. Every buffer dequeued with VIDIOC_DQBUF must be handed back with
 * VIDIOC_QBUF or the driver runs out and the stream stalls. `dequeue()`
 * therefore transfers a slot to the caller and `requeue()` gives it back;
 * `Session` tracks per-slot ownership so a renderer still sampling a slot can
 * never have it recycled underneath it.
 *
 * Threading: one capture thread calls `dequeue`/`requeue`. `open`/`configure`/
 * `start`/`stop` are setup-thread only.
 */

#include <score_addon_videoio_export.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Gfx::V4L2
{

/// How the driver and the application agree on buffer memory.
enum class BufferMode
{
  MmapRead,     ///< mmap into host memory; CPU-staged fallback
  MmapExport,   ///< driver allocates, VIDIOC_EXPBUF gives us a DMA-BUF fd
  DmaBufImport, ///< we allocate via DmaBufAllocator, driver imports
};

SCORE_ADDON_VIDEOIO_EXPORT const char* toString(BufferMode) noexcept;

/// What a queue actually supports, read from the REQBUFS capability bits
/// rather than assumed from the driver name.
struct BufferCaps
{
  /// False when the capability probe itself failed — typically EBUSY because
  /// another process is streaming. All the flags below are then meaningless,
  /// and must not be read as "the driver does not support this".
  bool probed{};

  /// False when the driver returned no capability bits at all. The field is
  /// optional (Linux 4.20+) and out-of-tree drivers commonly leave it zero --
  /// Magewell ProCapture does. Zero means "not reported", NOT "nothing
  /// supported", so the flags below are then baseline assumptions rather than
  /// driver-stated fact.
  bool reportsCaps{};

  bool mmap{};
  bool userptr{};
  bool dmabuf{};
  bool requests{};
  bool orphanedBufs{};

  /// The best ingress mode this queue can do, ignoring GPU-side support.
  BufferMode preferred() const noexcept;
};

/// Negotiated stream geometry.
struct Format
{
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t fourcc{};       ///< V4L2_PIX_FMT_*
  std::uint32_t bytesPerLine{}; ///< plane 0 stride
  std::uint32_t sizeImage{};    ///< plane 0 byte size
  std::uint32_t planeCount{1};
  bool multiplanar{};

  /// Per-plane stride/size for the multi-planar API; index 0 mirrors the
  /// single-planar fields above.
  std::uint32_t planeStride[4]{};
  std::uint32_t planeSize[4]{};
};

/**
 * @brief Source of externally-allocated DMA-BUFs for `DmaBufImport`.
 *
 * Implemented per platform: a GBM/DRM allocator on desktop, NvBufSurface on
 * Tegra. Keeping it abstract is what allows the buffer's DRM format modifier
 * to be chosen by whoever knows what the GPU wants, instead of being whatever
 * the capture driver happened to pick.
 */
struct DmaBufAllocator
{
  struct Buffer
  {
    int fd{-1};
    std::uint64_t modifier{}; ///< DRM_FORMAT_MOD_*
    std::uint32_t stride{};
    std::uint32_t offset{};
    std::size_t size{};
  };

  virtual ~DmaBufAllocator() = default;

  /// Human-readable, for the honest-strategy-name convention.
  virtual const char* name() const noexcept = 0;

  /// Allocate one buffer able to hold `size` bytes of a `fourcc` image.
  /// Returns false to abort session start (the caller then degrades a rung).
  virtual bool
  allocate(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
           std::size_t size, Buffer& out)
      = 0;

  virtual void release(Buffer&) noexcept = 0;
};

/// One capture buffer as seen by the strategy layer.
struct Slot
{
  std::size_t index{};
  int dmabufFd[4]{-1, -1, -1, -1}; ///< per plane; -1 when not exported
  std::uint64_t modifier{};
  void* mapped[4]{};               ///< MmapRead only
  std::size_t mappedSize[4]{};
  std::uint32_t bytesUsed{};
  std::uint64_t sequence{};        ///< driver frame counter, for gap detection
  std::uint64_t timestampNs{};     ///< v4l2_buffer.timestamp, monotonic
};

class SCORE_ADDON_VIDEOIO_EXPORT Session
{
public:
  Session();
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  /// Opens the node and reads its capabilities. Does not stream.
  bool open(const std::string& path);
  void close();
  bool isOpen() const noexcept;

  const std::string& path() const noexcept;
  const std::string& driver() const noexcept;
  const std::string& card() const noexcept;

  /// REQBUFS capability bits for the capture queue.
  BufferCaps bufferCaps() const noexcept;

  /// Negotiate geometry. Passing 0 for width/height keeps the driver's
  /// current setting. The *granted* format is returned by `format()`, which
  /// may differ from the request — V4L2 never fails a S_FMT for an
  /// unsupported size, it substitutes the closest one.
  bool configure(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc);
  const Format& format() const noexcept;

  /// Allocate buffers and STREAMON. `allocator` is required for
  /// `DmaBufImport` and ignored otherwise.
  bool start(BufferMode mode, std::size_t slotCount,
             DmaBufAllocator* allocator = nullptr);
  void stop();
  bool isStreaming() const noexcept;

  BufferMode mode() const noexcept;
  std::size_t slotCount() const noexcept;
  const Slot& slot(std::size_t i) const noexcept;

  /// Wait up to `timeoutMs` for a frame and dequeue it. Returns the slot
  /// index, or -1 on timeout, or -2 on a fatal stream error. The slot is
  /// owned by the caller until `requeue()`.
  int dequeue(int timeoutMs);

  /// Return a slot to the driver. Required for every successful `dequeue`.
  bool requeue(std::size_t index);

  /// Frames the driver reported as dropped/corrupt (V4L2_BUF_FLAG_ERROR)
  /// and sequence discontinuities, since `start()`.
  std::uint64_t errorFrames() const noexcept;
  std::uint64_t droppedFrames() const noexcept;

  /// Last failing ioctl, for honest reporting up the rung ladder.
  const std::string& lastError() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

/// A capture node discovered under /dev.
struct DeviceInfo
{
  std::string path;
  std::string driver;
  std::string card;
  std::string busInfo;
  bool canCapture{};
  bool multiplanar{};
};

/// Enumerate V4L2 capture nodes. Nodes that are not capture-capable
/// (metadata, output, radio) are skipped.
SCORE_ADDON_VIDEOIO_EXPORT std::vector<DeviceInfo> enumerateDevices();

} // namespace Gfx::V4L2
