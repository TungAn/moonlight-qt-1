#pragma once

#include "rubylightprotocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

class DisplayLatchClock;

// One stream's use of Rubylight's control messages (Rubylight docs/control-messages.md),
// sent only for the ids the host listed in its DESCRIBE answer:
// - phase lock (0x5530): every half second, how long before the display's latch each
//   shown frame was ready, so the host times its captures to land just before it;
// - clock sync (0x5537): the host's video clock, for the host-present to client-present
//   latency of each frame;
// - host timing (0x5536) and pipeline rates (0x553D): where the host spent each frame's
//   time and at what rate each host stage ran;
// - link notice (0x5538): when the host lowered the bitrate for the network.
class RubylightLink {
public:
    RubylightLink();
    ~RubylightLink();

    // After LiStartConnection() succeeded. phaseLock adds the 0x5530 reports.
    void start(bool phaseLock);
    // Before LiStopConnection().
    void stop();

    // ConnListenerRubylightMessage, on the control receive thread.
    void onMessage(uint16_t type, const uint8_t* payload, int length);

    // On the render thread, right after a frame's present call returned. The RTP
    // timestamp is the frame's 90 kHz presentation time from the host.
    void framePresented(uint32_t rtpTimestamp);

    // Appends the overlay lines; returns the bytes written.
    int formatOverlay(char* output, int length);

private:
    void tickerLoop();

    std::atomic<bool> m_Running {false};
    bool m_PhaseLockRequested = false;
    std::thread m_Ticker;
    std::mutex m_TickerMutex;
    std::condition_variable m_TickerWake;
    bool m_TickerStop = false;
    uint32_t m_ClockSyncSequence = 0;

    std::unique_ptr<DisplayLatchClock> m_LatchClock;
    int64_t m_LatchLeadUs = 0;

    std::mutex m_Mutex;
    // Phase lock, fed by framePresented().
    Rubylight::SlackWindow m_Slack;
    int64_t m_PhaseWindowStartUs = 0;
    int64_t m_PhaseLastFrameUs = 0;
    bool m_PhaseReportPending = false;
    Rubylight::PhaseReport m_PhasePending;
    bool m_PhaseReported = false;
    Rubylight::PhaseReport m_PhaseLast;
    uint32_t m_PhaseReports = 0;
    // Clock sync and frame ages.
    Rubylight::ClockOffset m_ClockOffset;
    std::deque<int64_t> m_FrameAgesUs;
    // Host reports.
    std::deque<Rubylight::HostTimingRecord> m_HostTiming;
    bool m_HaveRates = false;
    Rubylight::PipelineRates m_Rates;
    bool m_HaveNotice = false;
    Rubylight::LinkNotice m_Notice;
};
