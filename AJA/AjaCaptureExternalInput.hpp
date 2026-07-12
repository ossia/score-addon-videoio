#pragma once
#include <AJA/AJAInput.hpp>

#include <score_addon_videoio_export.h>

#include <memory>

namespace Video
{
class ExternalInput;
}

namespace Gfx::AJA
{
/// Build the real CPU-staging SDI capture (aja_input_capture +
/// aja_input_decoder) as a Video::ExternalInput, bypassing the
/// device/protocol layer. Used by the round-trip test harness to read
/// AVFrames directly. Returns nullptr if the card can't be opened.
SCORE_ADDON_VIDEOIO_EXPORT std::shared_ptr<::Video::ExternalInput>
makeAJACapture(const AJAInputSettings& s);
}
