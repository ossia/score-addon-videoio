#pragma once

/**
 * @file V4L2SyncRig.hpp
 * @brief Correlators shared by the V4L2 backends that make up one camera rig.
 *
 * A rig is several V4L2 devices that are meant to be rendered from the same
 * capture -- the 360 head's two sensors, sharing a GMSL clock. Each device has
 * its own backend, its own thread and its own slot ring, so the thing they have
 * to share is the correlator that turns their separate arrivals into one
 * published set.
 *
 * Backends find each other by rig name rather than by pointer because they are
 * constructed independently, from separate graph nodes, in whatever order the
 * document happens to instantiate them. The registry hands the first caller a
 * new correlator and every later caller the same one; when the last backend
 * drops its reference the correlator goes with it, so a stopped rig leaves
 * nothing behind for the next one to inherit.
 */

#include <Gfx/Graph/interop/CaptureCorrelator.hpp>

#include <score_addon_videoio_export.h>

#include <cstddef>
#include <memory>
#include <string>

namespace Gfx::V4L2
{

/**
 * @brief The correlator for `name`, creating it if this is the first member.
 *
 * `memberCount` is only honoured for the caller that creates the rig: a later
 * member disagreeing about the size would have to resize a correlator its
 * partners are already publishing into. Returns null for an empty name, which
 * is how a device says it is not part of a rig.
 */
SCORE_ADDON_VIDEOIO_EXPORT std::shared_ptr<score::gfx::interop::CaptureCorrelator>
acquireSyncRig(const std::string& name, std::size_t memberCount);

} // namespace Gfx::V4L2
