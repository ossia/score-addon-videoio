#include <decklink/DeckLinkDevices.hpp>

#include <QString>

#include <cstdlib>

namespace Gfx::DeckLink
{

bool ensureComInit() noexcept
{
#if defined(_WIN32)
  const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  // S_FALSE = already initialised on this thread; RPC_E_CHANGED_MODE = already
  // initialised in a different (STA) model, which is fine for our use.
  return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
#else
  // Linux DeckLinkAPI is not COM-registered; nothing to initialise.
  return true;
#endif
}

namespace
{
ComPtr<IDeckLinkIterator> makeIterator()
{
  ComPtr<IDeckLinkIterator> it;
#if defined(_WIN32)
  CoCreateInstance(
      CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator,
      it.putVoid());
#else
  // Owned (AddRef'd) instance from the dispatch stub; adopt without AddRef.
  // Returns nullptr when Desktop Video (libDeckLinkAPI.so) is not installed.
  *it.put() = CreateDeckLinkIteratorInstance();
#endif
  return it;
}

/// Consume a display-name out-param: Windows hands back a BSTR, Linux a
/// malloc'd UTF-8 const char*. Both are freed here.
#if defined(_WIN32)
std::string takeDisplayName(BSTR s)
{
  if(!s)
    return {};
  const auto qs = QString::fromWCharArray(
      reinterpret_cast<const wchar_t*>(s), int(SysStringLen(s)));
  SysFreeString(s);
  return qs.toStdString();
}
#else
std::string takeDisplayName(const char* s)
{
  if(!s)
    return {};
  std::string out{s};
  free(const_cast<char*>(s));
  return out;
}
#endif
} // namespace

std::vector<DeviceInfo> enumerateDevices()
{
  ensureComInit();
  std::vector<DeviceInfo> out;
  auto it = makeIterator();
  if(!it)
    return out;

  ComPtr<IDeckLink> dev;
  for(int idx = 0; it->Next(dev.put()) == S_OK; ++idx, dev.reset())
  {
    DeviceInfo info;
    info.index = idx;

#if defined(_WIN32)
    BSTR name = nullptr;
#else
    const char* name = nullptr;
#endif
    if(dev->GetDisplayName(&name) == S_OK)
      info.displayName = takeDisplayName(name);

    ComPtr<IDeckLinkProfileAttributes> attr;
    if(dev->QueryInterface(IID_IDeckLinkProfileAttributes, attr.putVoid())
       == S_OK)
    {
      LONGLONG io = 0;
      if(attr->GetInt(BMDDeckLinkVideoIOSupport, &io) == S_OK)
      {
        info.canOutput = (io & bmdDeviceSupportsPlayback) != 0;
        info.canInput = (io & bmdDeviceSupportsCapture) != 0;
      }
      LONGLONG pid = 0;
      if(attr->GetInt(BMDDeckLinkPersistentID, &pid) == S_OK)
        info.persistentId = pid;
    }

    out.push_back(std::move(info));
  }
  return out;
}

ComPtr<IDeckLink> openDevice(int index)
{
  ensureComInit();
  auto it = makeIterator();
  if(!it)
    return {};

  ComPtr<IDeckLink> dev;
  for(int i = 0; it->Next(dev.put()) == S_OK; ++i)
  {
    if(i == index)
      return dev;
    dev.reset();
  }
  return {};
}

} // namespace Gfx::DeckLink
