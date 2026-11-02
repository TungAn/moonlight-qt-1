#pragma once

// Platform-neutral data of the VRR presentation path. Nothing here depends on
// SDL, Qt, FFmpeg or a renderer: the timing controller and the pacing worker
// are unit tested on these types alone.

#include "settings/vrrtimingoptions.h"

#include <cstdint>

namespace Vrr {

constexpr uint64_t kMicrosecondsPerSecond = 1000000;
constexpr uint64_t kRtpClockHz = 90000;

// Waiting decoded frames plus the one being prepared or presented. The
// decoder's surface pool reserves this many extra frames.
constexpr int kQueuedFrames = 4;
constexpr int kOwnedFrames = kQueuedFrames + 1;

// Why a requested VRR session uses fixed pacing instead.
enum class FallbackReason : uint8_t {
    NoFallback,
    NotRequested,
    VsyncDisabled,
    UnknownRefresh,
    StreamAboveRefresh,
    UnsupportedPlatform,
    UnsupportedRenderer,
    NoAdaptivePresentMode,
    InitializationFailed,
};

inline const char* fallbackReasonName(FallbackReason reason)
{
    switch (reason) {
    case FallbackReason::NoFallback:
        return "none";
    case FallbackReason::NotRequested:
        return "not requested";
    case FallbackReason::VsyncDisabled:
        return "V-Sync is disabled";
    case FallbackReason::UnknownRefresh:
        return "display refresh rate is unknown";
    case FallbackReason::StreamAboveRefresh:
        return "stream frame rate exceeds the display refresh rate";
    case FallbackReason::UnsupportedPlatform:
        return "not supported on this platform";
    case FallbackReason::UnsupportedRenderer:
        return "the selected renderer cannot present VRR frames";
    case FallbackReason::NoAdaptivePresentMode:
        return "no synchronized presentation mode";
    case FallbackReason::InitializationFailed:
        return "VRR presentation failed to initialize";
    }
    return "unknown";
}

// How the presenter shows frames that arrive faster than the panel can scan.
enum class PresentProtection : uint8_t {
    // Presentation is synchronized: a present inside the panel's minimum
    // period waits for the next refresh and never tears (Vulkan Mailbox,
    // DXGI interval 1). No software spacing floor is needed for it.
    Native,
    // The controller enforces one display period plus a guard between
    // submissions in software: Vulkan FIFO, which would otherwise queue
    // presents behind each other, and an explicitly allowed Immediate,
    // whose flips may tear when the panel is not in adaptive mode.
    SoftwareFloor,
};

enum class PresentMode : uint8_t {
    Fifo,
    Mailbox,
    Immediate,
};

struct PresentChoice {
    PresentMode mode = PresentMode::Fifo;
    PresentProtection protection = PresentProtection::SoftwareFloor;
};

// VRR requires V-Sync, so the chosen mode must never tear on its own. No
// portable query tells whether the output is actually scanning out
// adaptively (a fixed-refresh panel, adaptive sync disabled in the
// compositor or a windowed surface all look the same to the swapchain), so
// Immediate, which only avoids tearing while adaptive scanout is active, is
// used only when the user explicitly allows tearing. Mailbox and FIFO both
// follow the adaptive refresh when the compositor enables it and stay
// synchronized when it does not. FIFO is always available.
inline PresentChoice chooseSynchronizedPresentMode(bool mailboxSupported, bool immediateSupported,
                                                   bool tearingAllowed)
{
    if (tearingAllowed && immediateSupported) {
        return { PresentMode::Immediate, PresentProtection::SoftwareFloor };
    }
    if (mailboxSupported) {
        return { PresentMode::Mailbox, PresentProtection::Native };
    }
    return { PresentMode::Fifo, PresentProtection::SoftwareFloor };
}

inline const char* presentModeName(PresentMode mode)
{
    switch (mode) {
    case PresentMode::Fifo:
        return "FIFO";
    case PresentMode::Mailbox:
        return "Mailbox";
    case PresentMode::Immediate:
        return "Immediate";
    }
    return "unknown";
}

struct SessionConfig {
    int displayRefreshHz = 0;
    int streamRateHz = 0;
    // 0 Smoothest, 1 Balanced, 2 Lowest latency.
    int latencyMode = 1;
    VrrTimingOptions timing;
    // "Reduce judder": regularize uneven source timestamps within a bounded
    // retiming allowance.
    bool reduceJudder = true;
    PresentProtection protection = PresentProtection::SoftwareFloor;
};

// Identity and timing of one source frame as it reaches the pacing worker.
// All times are LiGetMicroseconds() (client monotonic clock).
struct FrameTiming {
    // Host frame number; -1 when unknown.
    int64_t frameNumber = -1;
    // 90 kHz RTP timestamp of the frame. Zero is a valid timestamp.
    uint32_t rtpTimestamp = 0;
    bool timestampValid = false;
    // When the frame became available to the worker: decoder output for
    // decoded frames, reassembly for frames decoded during preparation.
    uint64_t readyUs = 0;
    // First packet of the frame received from the network; 0 if unknown.
    uint64_t receiveUs = 0;
    // Packets lost in the frame (PyroWave presents partial frames). Such
    // frames are shown on schedule but never teach the buffer to grow.
    uint32_t lostPackets = 0;
};

struct Decision {
    uint64_t targetUs = 0;
    // Earliest time preparation should start; preparation normally starts on
    // arrival, within the playout interval.
    uint64_t renderStartUs = 0;
    // Target before clamping to the present and before spacing floors.
    uint64_t intendedTargetUs = 0;
    uint64_t sourceTimeUs = 0;
    uint64_t sourcePeriodUs = 0;
    uint64_t sourceIntervalUs = 0;
    uint64_t playoutDelayUs = 0;
    int64_t smoothingUs = 0;
    // Frame readiness relative to its mapped source time.
    int64_t readyOffsetUs = 0;
    // How far a spacing floor or late recovery pushed the target.
    uint64_t floorPushUs = 0;
    // Present synchronized to the next refresh (latched) instead of adaptively.
    bool latched = false;
    bool timestampPlayout = false;
    bool rebased = false;
    bool phaseDiscontinuity = false;
    bool sourceRateChanged = false;
    bool catchUp = false;
};

// The outcome of preparing and presenting a scheduled frame.
struct Submission {
    bool presented = false;
    bool cancelled = false;
    uint64_t submitUs = 0;
    // Preparation duration (rendering, including GPU completion when the
    // presenter observes it), excluding intentional waits.
    uint64_t preparationUs = 0;
    // When the prepared image was ready to present.
    uint64_t readyUs = 0;
};

}
