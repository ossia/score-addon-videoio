#pragma once

/**
 * @file DeckLinkBufferPool.hpp
 * @brief Frame buffers we own and the DeckLink SDK DMAs into.
 *
 * SDK 16.0's `EnableVideoInputWithAllocatorProvider` lets the caller supply the
 * memory the card captures into. Without it the SDK allocates its own frames and
 * `VideoInputFrameArrived` has to memcpy each one into a capture slot -- a full
 * frame per frame, ~500 MB/s at 1080p60 BGRA.
 *
 * With it, the pool below IS the capture slot set: the card DMAs straight into
 * pages we allocated, and on Vulkan those same pages are imported once through
 * `BorrowedHostImportCapture`, so the GPU DMAs out of them. Nothing copies.
 *
 * OWNERSHIP -- two independent holders, and a buffer is reusable only when
 * neither wants it:
 *
 *   SDK      AllocateVideoBuffer -> takes a free buffer, `sdkHolding`.
 *            Releases it (refcount 0) when the frame is retired.
 *   renderer ingestFrame publishes the index; the strategy hands it back
 *            through takeReturnedSlots once the GPU is finished, which is
 *            `FramesInFlight + 1` acquisitions later.
 *
 * Releasing on the SDK's word alone would let the card overwrite a frame the
 * renderer is still sampling; releasing on the renderer's alone would hand the
 * SDK a buffer it still owns. Both flags must clear.
 *
 * The buffers themselves are allocated once and freed only at teardown, so the
 * addresses the strategy imported stay valid for the stream's lifetime -- an
 * import is not re-establishable per frame.
 */

#include <decklink/DeckLink.hpp>

#include <QDebug>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace Gfx::DeckLink
{

/// Page-aligned frame buffers shared between the card and Vulkan.
/// 4096 because VK_EXT_external_memory_host's minImportedHostPointerAlignment
/// is 4096 on every driver measured.
class DeckLinkBufferPool
{
public:
  ~DeckLinkBufferPool() { destroy(); }

  bool create(std::size_t count, std::size_t bytes)
  {
    destroy();
    if(count == 0 || bytes == 0)
      return false;
    m_bytes = ((bytes + 4095u) / 4096u) * 4096u;
    m_slots.resize(count);
    for(auto& s : m_slots)
    {
      void* p = nullptr;
      if(::posix_memalign(&p, 4096, m_bytes) != 0 || !p)
      {
        destroy();
        return false;
      }
      std::memset(p, 0, m_bytes);
      s.mem = p;
    }
    return true;
  }

  void destroy()
  {
    for(auto& s : m_slots)
      ::free(s.mem);
    m_slots.clear();
    m_bytes = 0;
  }

  std::size_t count() const noexcept { return m_slots.size(); }
  std::size_t bufferBytes() const noexcept { return m_bytes; }
  void* buffer(std::size_t i) const noexcept
  {
    return i < m_slots.size() ? m_slots[i].mem : nullptr;
  }

  /// Index of the buffer at @p p, or -1 when the pointer is not ours -- which
  /// is how the callback tells a pooled frame from one the SDK allocated
  /// itself after declining the provider.
  int indexOf(const void* p) const noexcept
  {
    for(std::size_t i = 0; i < m_slots.size(); ++i)
      if(m_slots[i].mem == p)
        return int(i);
    return -1;
  }

  /// SDK thread. A buffer neither holder wants, marked as the SDK's.
  int acquireForSdk() noexcept
  {
    std::lock_guard g{m_mutex};
    for(std::size_t i = 0; i < m_slots.size(); ++i)
    {
      auto& s = m_slots[i];
      if(!s.sdkHolding && !s.rendererHolding)
      {
        s.sdkHolding = true;
        return int(i);
      }
    }
    return -1;
  }

  void sdkReleased(std::size_t i) noexcept
  {
    std::lock_guard g{m_mutex};
    if(i < m_slots.size())
      m_slots[i].sdkHolding = false;
  }

  void rendererTook(std::size_t i) noexcept
  {
    std::lock_guard g{m_mutex};
    if(i < m_slots.size())
      m_slots[i].rendererHolding = true;
  }

  void rendererReturned(std::size_t i) noexcept
  {
    std::lock_guard g{m_mutex};
    if(i < m_slots.size())
      m_slots[i].rendererHolding = false;
  }

private:
  struct Slot
  {
    void* mem{};
    bool sdkHolding{};
    bool rendererHolding{};
  };
  mutable std::mutex m_mutex;
  std::vector<Slot> m_slots;
  std::size_t m_bytes{};
};

/// One pooled buffer, handed to the SDK. Owns no memory: at refcount zero it
/// only tells the pool the SDK is done with its slot.
class DeckLinkPooledBuffer final : public IDeckLinkVideoBuffer
{
public:
  DeckLinkPooledBuffer(DeckLinkBufferPool& pool, std::size_t index)
      : m_pool{pool}
      , m_index{index}
  {
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
  {
    if(!out)
      return E_POINTER;
    if(IsEqualIID(iid, IID_IDeckLinkVideoBuffer)
       || IsEqualIID(iid, IID_IUnknown))
    {
      AddRef();
      *out = static_cast<IDeckLinkVideoBuffer*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return ULONG(m_ref.fetch_add(1, std::memory_order_relaxed) + 1);
  }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const auto n = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if(n == 0)
    {
      m_pool.sdkReleased(m_index);
      delete this;
    }
    return ULONG(n);
  }

  HRESULT STDMETHODCALLTYPE GetBytes(void** buffer) override
  {
    if(!buffer)
      return E_POINTER;
    *buffer = m_pool.buffer(m_index);
    return *buffer ? S_OK : E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE GetSize(uint64_t* size) override
  {
    if(!size)
      return E_POINTER;
    *size = m_pool.bufferBytes();
    return S_OK;
  }
  // Plain host memory: nothing to map or flush.
  HRESULT STDMETHODCALLTYPE StartAccess(BMDBufferAccessFlags) override
  {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EndAccess(BMDBufferAccessFlags) override
  {
    return S_OK;
  }

private:
  ~DeckLinkPooledBuffer() override = default;
  DeckLinkBufferPool& m_pool;
  std::size_t m_index;
  std::atomic<int> m_ref{1};
};

class DeckLinkPoolAllocator final : public IDeckLinkVideoBufferAllocator
{
public:
  explicit DeckLinkPoolAllocator(DeckLinkBufferPool& pool)
      : m_pool{pool}
  {
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
  {
    if(!out)
      return E_POINTER;
    if(IsEqualIID(iid, IID_IDeckLinkVideoBufferAllocator)
       || IsEqualIID(iid, IID_IUnknown))
    {
      AddRef();
      *out = static_cast<IDeckLinkVideoBufferAllocator*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return ULONG(m_ref.fetch_add(1, std::memory_order_relaxed) + 1);
  }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const auto n = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if(n == 0)
      delete this;
    return ULONG(n);
  }

  HRESULT STDMETHODCALLTYPE
  AllocateVideoBuffer(IDeckLinkVideoBuffer** allocated) override
  {
    if(!allocated)
      return E_POINTER;
    const int i = m_pool.acquireForSdk();
    ++m_allocCalls;
    if(i < 0)
    {
      // Every buffer is spoken for. Failing is correct -- handing back one the
      // renderer is sampling would tear the displayed frame -- but a pool that
      // runs dry means it is too shallow for the SDK's queue plus
      // FramesInFlight. Warn once: the SDK retries per frame, so logging each
      // time buries every other message.
      if(!m_warnedExhausted)
      {
        m_warnedExhausted = true;
        qWarning() << "DeckLink pool: exhausted after" << m_allocCalls
                   << "allocations; deepen kPoolDepth";
      }
      *allocated = nullptr;
      return E_OUTOFMEMORY;
    }
    *allocated = new DeckLinkPooledBuffer(m_pool, std::size_t(i));
    return S_OK;
  }

  int allocCalls() const noexcept { return m_allocCalls; }

private:
  ~DeckLinkPoolAllocator() override = default;
  DeckLinkBufferPool& m_pool;
  std::atomic<int> m_ref{1};
  int m_allocCalls{};
  bool m_warnedExhausted{};
};

class DeckLinkAllocatorProvider final : public IDeckLinkVideoBufferAllocatorProvider
{
public:
  DeckLinkAllocatorProvider(DeckLinkBufferPool& pool, std::uint32_t expectRowBytes)
      : m_pool{pool}
      , m_expectRowBytes{expectRowBytes}
  {
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
  {
    if(!out)
      return E_POINTER;
    if(IsEqualIID(iid, IID_IDeckLinkVideoBufferAllocatorProvider)
       || IsEqualIID(iid, IID_IUnknown))
    {
      AddRef();
      *out = static_cast<IDeckLinkVideoBufferAllocatorProvider*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return ULONG(m_ref.fetch_add(1, std::memory_order_relaxed) + 1);
  }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const auto n = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if(n == 0)
      delete this;
    return ULONG(n);
  }

  HRESULT STDMETHODCALLTYPE GetVideoBufferAllocator(
      uint32_t bufferSize, uint32_t width, uint32_t height, uint32_t rowBytes,
      BMDPixelFormat, IDeckLinkVideoBufferAllocator** allocator) override
  {
    if(!allocator)
      return E_POINTER;
    m_asked = true;
    qDebug() << "DeckLink pool: GetVideoBufferAllocator size" << bufferSize << width
             << "x" << height << "rowBytes" << rowBytes << "(pool buffer"
             << m_pool.bufferBytes() << "expect rowBytes" << m_expectRowBytes << ")";
    // Declining is safe: the SDK falls back to its own allocation and the
    // callback's pointer lookup then misses, so the copy path runs. Accepting a
    // geometry our buffers do not match would shear or overrun frames.
    if(bufferSize > m_pool.bufferBytes() || rowBytes != m_expectRowBytes)
    {
      qWarning() << "DeckLink pool: declining, geometry differs from the pool";
      *allocator = nullptr;
      m_declined = true;
      return E_FAIL;
    }
    *allocator = new DeckLinkPoolAllocator(m_pool);
    return S_OK;
  }

  bool wasAsked() const noexcept { return m_asked; }
  bool declined() const noexcept { return m_declined; }

private:
  ~DeckLinkAllocatorProvider() override = default;
  DeckLinkBufferPool& m_pool;
  std::uint32_t m_expectRowBytes{};
  std::atomic<int> m_ref{1};
  bool m_asked{}, m_declined{};
};

} // namespace Gfx::DeckLink
