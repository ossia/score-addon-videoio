#pragma once

/**
 * @file ArgusRuntimeInternal.hpp
 * @brief Raw libargus handles, for the Argus sources only.
 *
 * ArgusRuntime.hpp deliberately exposes plain structs so the rest of the addon
 * (and every non-Tegra build) compiles without the Argus headers. The session
 * does need the real handles, though, and there may be only one CameraProvider
 * per process -- so it borrows the runtime's rather than making a second, which
 * libargus would refuse.
 *
 * Only include this from a translation unit already guarded by SCORE_HAS_ARGUS.
 */

#if !defined(SCORE_HAS_ARGUS)
#error "ArgusRuntimeInternal.hpp requires SCORE_HAS_ARGUS"
#endif

#include <Argus/Argus.h>

#include <vector>

namespace Gfx::Argus
{

/// The process-wide provider, or null when unavailable. Not owned by callers.
::Argus::ICameraProvider* argusProviderHandle() noexcept;

/// Device handles in the same order as argusCameras(), so an index from one is
/// valid in the other. Empty when unavailable.
const std::vector<::Argus::CameraDevice*>& argusDeviceHandles();

}
