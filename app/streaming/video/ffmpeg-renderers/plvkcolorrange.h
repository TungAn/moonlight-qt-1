#pragma once

#include <libplacebo/colorspace.h>

// Range of a decoded standard-codec frame as libplacebo should interpret it.
// `mapped` is what libplacebo read from the AVFrame; frames that carried no
// range already received the negotiated one from the decoder.
//
// AMF AV1 encoding on Windows hosts marks full-range output as limited. AV1
// always codes its range, so that broken tag reads as an explicit "limited":
// it is corrected only for AV1 streams negotiated as full range. Explicit
// H.264/HEVC ranges and streams negotiated as limited are honored, so
// correctly tagged limited video is not shown with raised blacks.
inline pl_color_levels plVkFrameLevels(pl_color_levels mapped, bool av1, bool negotiatedFullRange)
{
    if (av1 && negotiatedFullRange && mapped == PL_COLOR_LEVELS_LIMITED) {
        return PL_COLOR_LEVELS_FULL;
    }
    return mapped;
}
