// Range interpretation of decoded standard-codec frames (PlVk renderer).
#include "streaming/video/ffmpeg-renderers/plvkcolorrange.h"

#include <cassert>
#include <initializer_list>
#include <cstdio>

int main()
{
    // Explicit ranges are honored for every codec when negotiation agrees.
    for (bool av1 : {false, true}) {
        assert(plVkFrameLevels(PL_COLOR_LEVELS_FULL, av1, true) == PL_COLOR_LEVELS_FULL);
        assert(plVkFrameLevels(PL_COLOR_LEVELS_LIMITED, av1, false) == PL_COLOR_LEVELS_LIMITED);
        assert(plVkFrameLevels(PL_COLOR_LEVELS_FULL, av1, false) == PL_COLOR_LEVELS_FULL);
    }
    // A limited H.264/HEVC frame stays limited even when full was requested:
    // the host tagged what it encoded.
    assert(plVkFrameLevels(PL_COLOR_LEVELS_LIMITED, false, true) == PL_COLOR_LEVELS_LIMITED);
    // The AMF AV1 case: full requested, bitstream claims limited.
    assert(plVkFrameLevels(PL_COLOR_LEVELS_LIMITED, true, true) == PL_COLOR_LEVELS_FULL);
    // Unknown stays unknown here; the decoder fills it from negotiation.
    assert(plVkFrameLevels(PL_COLOR_LEVELS_UNKNOWN, false, true) == PL_COLOR_LEVELS_UNKNOWN);

    // Limited 8-bit black/white decode to 0/1 only when interpreted as limited.
    pl_color_repr repr = {};
    repr.sys = PL_COLOR_SYSTEM_BT_709;
    repr.levels = PL_COLOR_LEVELS_LIMITED;
    repr.bits.color_depth = repr.bits.sample_depth = 8;
    pl_transform3x3 t = pl_color_repr_decode(&repr, nullptr);
    float black[] = {16 / 255.0f, 128 / 255.0f, 128 / 255.0f};
    float white[] = {235 / 255.0f, 128 / 255.0f, 128 / 255.0f};
    pl_transform3x3_apply(&t, black);
    pl_transform3x3_apply(&t, white);
    for (float c : black) assert(c > -1e-5f && c < 1e-5f);
    for (float c : white) assert(c > 1 - 1e-5f && c < 1 + 1e-5f);

    std::puts("plvk-color-range: all tests passed");
    return 0;
}
