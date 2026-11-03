#include "rubylightlink.h"

#include <Limelight.h>
#include "SDL_compat.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#endif

namespace {

constexpr int64_t PhaseReportIntervalUs = 500000;
constexpr size_t PhaseMinFrames = 10;
// A window longer than this without a frame, or a frame this old, starts the window over.
constexpr int64_t PhaseGapUs = 250000;
constexpr int64_t ClockSyncIntervalUs = 250000;
constexpr size_t FrameAgeSamples = 240;
constexpr size_t HostTimingSamples = 240;

int64_t envMicroseconds(const char* name, int64_t fallback, int64_t low, int64_t high)
{
    const char* value = SDL_getenv(name);
    if (value == nullptr || *value == 0) return fallback;
    char* end = nullptr;
    const long long parsed = std::strtoll(value, &end, 10);
    if (end == value || *end != 0) return fallback;
    return std::clamp<int64_t>(parsed, low, high);
}

template<class T, class Field>
double median(const std::deque<T>& values, Field field)
{
    std::vector<double> sorted;
    sorted.reserve(values.size());
    for (const auto& value : values) sorted.push_back(double(field(value)));
    if (sorted.empty()) return 0;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    return sorted[sorted.size() / 2];
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
void appendFormat(char* output, int length, int* used, const char* format, ...)
{
    if (*used >= length - 1) return;
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(output + *used, size_t(length - *used), format, args);
    va_end(args);
    if (written > 0) *used = (std::min)(length - 1, *used + written);
}

double percentile(std::deque<int64_t> values, size_t percent)
{
    if (values.empty()) return 0;
    std::vector<int64_t> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    return double(sorted[(sorted.size() - 1) * percent / 100]);
}

} // namespace

// The display's refresh clock, from DRM vblank events on the CRTC that drives the
// panel. A compositor (Gamescope) holds DRM master, but any client may wait for
// vblanks. Timestamps are CLOCK_MONOTONIC.
class DisplayLatchClock {
public:
    ~DisplayLatchClock() { stop(); }

    bool start()
    {
#ifdef __linux__
        const char* card = SDL_getenv("PYROLIGHT_DRM_CARD");
        m_Crtc = int(envMicroseconds("PYROLIGHT_DRM_CRTC", 0, 0, 31));
        for (int index = 0; index < 4 && m_Fd < 0; ++index) {
            char path[64];
            if (card != nullptr && *card != 0) {
                if (index > 0) break;
                std::snprintf(path, sizeof(path), "%s", card);
            }
            else {
                std::snprintf(path, sizeof(path), "/dev/dri/card%d", index);
            }
            const int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            if (requestEvent(fd)) {
                m_Fd = fd;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Phase lock: display latch clock from %s, CRTC %d", path, m_Crtc);
            }
            else {
                close(fd);
            }
        }
        if (m_Fd < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Phase lock: no DRM vblank clock (set PYROLIGHT_DRM_CARD/PYROLIGHT_DRM_CRTC); no reports are sent");
            return false;
        }
        m_Stop = false;
        m_Thread = std::thread([this] { run(); });
        return true;
#else
        return false;
#endif
    }

    void stop()
    {
        m_Stop = true;
        if (m_Thread.joinable()) m_Thread.join();
#ifdef __linux__
        if (m_Fd >= 0) close(m_Fd);
#endif
        m_Fd = -1;
    }

    // The latest vblank and the measured refresh period, if both are current.
    bool timing(int64_t nowUs, int64_t* vblankUs, int64_t* periodUs) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_LastVblankUs == 0 || m_PeriodUs == 0 || nowUs - m_LastVblankUs > 100000) return false;
        *vblankUs = m_LastVblankUs;
        *periodUs = m_PeriodUs;
        return true;
    }

    static int64_t nowUs()
    {
#ifdef __linux__
        timespec now {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return int64_t(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
#else
        return 0;
#endif
    }

private:
#ifdef __linux__
    // The kernel's union drm_wait_vblank and struct drm_event_vblank (drm.h), declared
    // here so neither libdrm nor the kernel headers are needed.
    struct WaitVblankRequest { uint32_t type; uint32_t sequence; unsigned long signal; };
    struct WaitVblankReply { uint32_t type; uint32_t sequence; long tvalSec; long tvalUsec; };
    union WaitVblank { WaitVblankRequest request; WaitVblankReply reply; };
    struct EventVblank { uint32_t type; uint32_t length; uint64_t userData; uint32_t tvSec; uint32_t tvUsec; uint32_t sequence; uint32_t crtcId; };
    static constexpr unsigned long IoctlWaitVblank = _IOWR('d', 0x3a, union WaitVblank);
    static constexpr uint32_t VblankRelative = 0x1;
    static constexpr uint32_t VblankEvent = 0x4000000;
    static constexpr uint32_t VblankSecondary = 0x20000000;
    static constexpr uint32_t HighCrtcShift = 1;
    static constexpr uint32_t EventTypeVblank = 0x01;

    bool requestEvent(int fd)
    {
        WaitVblank wait {};
        wait.request.type = VblankRelative | VblankEvent;
        if (m_Crtc == 1) wait.request.type |= VblankSecondary;
        else if (m_Crtc > 1) wait.request.type |= (uint32_t(m_Crtc) << HighCrtcShift) & 0x3e;
        wait.request.sequence = 1;
        return ioctl(fd, IoctlWaitVblank, &wait) == 0;
    }

    void run()
    {
        std::vector<int64_t> deltas;
        int64_t previous = 0;
        char buffer[1024];
        int failures = 0;
        while (!m_Stop) {
            pollfd fd {m_Fd, POLLIN, 0};
            const int ready = poll(&fd, 1, 100);
            if (ready <= 0) {
                // The display may be off; ask again.
                if (ready == 0 && !requestEvent(m_Fd) && ++failures > 50) break;
                continue;
            }
            const ssize_t bytes = read(m_Fd, buffer, sizeof(buffer));
            if (bytes <= 0) continue;
            bool requested = false;
            for (ssize_t offset = 0; offset + ssize_t(sizeof(EventVblank)) <= bytes;) {
                EventVblank event;
                std::memcpy(&event, buffer + offset, sizeof(event));
                if (event.length < sizeof(EventVblank)) break;
                offset += event.length;
                if (event.type != EventTypeVblank) continue;
                // Ask for the next one first, so no vblank is skipped.
                if (!requested) requested = requestEvent(m_Fd);
                const int64_t vblank = int64_t(event.tvSec) * 1000000 + event.tvUsec;
                if (previous != 0) {
                    const int64_t delta = vblank - previous;
                    if (delta >= 2000 && delta <= 41700) {
                        deltas.push_back(delta);
                        if (deltas.size() > 64) deltas.erase(deltas.begin());
                    }
                }
                previous = vblank;
                std::vector<int64_t> sorted = deltas;
                std::sort(sorted.begin(), sorted.end());
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_LastVblankUs = vblank;
                m_PeriodUs = sorted.size() >= 8 ? sorted[sorted.size() / 2] : 0;
            }
            failures = requested ? 0 : failures + 1;
            if (!requested && failures > 50) break;
        }
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_LastVblankUs = 0;
    }
#endif

    int m_Fd = -1;
#ifdef __linux__
    int m_Crtc = 0;
#endif
    std::atomic<bool> m_Stop {false};
    std::thread m_Thread;
    mutable std::mutex m_Mutex;
    int64_t m_LastVblankUs = 0;
    int64_t m_PeriodUs = 0;
};

RubylightLink::RubylightLink() = default;

RubylightLink::~RubylightLink()
{
    stop();
}

void RubylightLink::start(bool phaseLock)
{
    stop();
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Slack.clear();
        m_PhaseWindowStartUs = m_PhaseLastFrameUs = 0;
        m_PhaseReportPending = m_PhaseReported = false;
        m_PhaseReports = 0;
        m_ClockOffset.clear();
        m_FrameAgesUs.clear();
        m_HostTiming.clear();
        m_HaveRates = m_HaveNotice = false;
    }
    const bool any = LiHostAcceptsRubylightMessage(RL_PHASE_LOCK_REPORT) || LiHostAcceptsRubylightMessage(RL_CLOCK_SYNC) ||
                     LiHostAcceptsRubylightMessage(RL_HOST_TIMING) || LiHostAcceptsRubylightMessage(RL_PIPELINE_RATES) ||
                     LiHostAcceptsRubylightMessage(RL_LINK_NOTICE);
    if (!any) return;

    // Requests are reliable: a lost one would leave the stream without reports.
    if (LiHostAcceptsRubylightMessage(RL_HOST_TIMING)) {
        const auto request = Rubylight::enableRequest(true);
        LiSendRubylightMessage(RL_HOST_TIMING, request.data(), int(request.size()), true);
    }
    if (LiHostAcceptsRubylightMessage(RL_PIPELINE_RATES)) {
        const auto request = Rubylight::pipelineRatesRequest(true, 1000);
        LiSendRubylightMessage(RL_PIPELINE_RATES, request.data(), int(request.size()), true);
    }
    if (LiHostAcceptsRubylightMessage(RL_LINK_NOTICE)) {
        const auto request = Rubylight::enableRequest(true);
        LiSendRubylightMessage(RL_LINK_NOTICE, request.data(), int(request.size()), true);
    }

    m_PhaseLockRequested = phaseLock && LiHostAcceptsRubylightMessage(RL_PHASE_LOCK_REPORT);
    if (m_PhaseLockRequested) {
        // How long before the vblank the compositor latches a frame, plus the GPU work
        // still left after the present call. Gamescope commits about 2.5 ms before
        // vblank on the Retroid Pocket 6; the rest is the render and frame generation tail.
        m_LatchLeadUs = envMicroseconds("PYROLIGHT_PHASE_LATCH_LEAD_US", 4000, 0, 20000);
        m_LatchClock = std::make_unique<DisplayLatchClock>();
        if (!m_LatchClock->start()) {
            m_LatchClock.reset();
            m_PhaseLockRequested = false;
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Phase lock: reporting to the host, latch %lld us before vblank",
                        (long long)m_LatchLeadUs);
        }
    }
    else if (phaseLock) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Phase lock: the host does not take phase lock reports");
    }

    m_TickerStop = false;
    m_Running = true;
    m_Ticker = std::thread([this] { tickerLoop(); });
}

void RubylightLink::stop()
{
    m_Running = false;
    {
        std::lock_guard<std::mutex> lock(m_TickerMutex);
        m_TickerStop = true;
    }
    m_TickerWake.notify_all();
    if (m_Ticker.joinable()) m_Ticker.join();
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_LatchClock) {
        m_LatchClock->stop();
        m_LatchClock.reset();
    }
    m_PhaseLockRequested = false;
}

void RubylightLink::tickerLoop()
{
    int64_t nextClockSync = 0;
    std::unique_lock<std::mutex> tickerLock(m_TickerMutex);
    while (!m_TickerStop) {
        tickerLock.unlock();
        const int64_t now = int64_t(LiGetMicroseconds());
        if (now >= nextClockSync && LiHostAcceptsRubylightMessage(RL_CLOCK_SYNC)) {
            // Unreliable: a lost probe is one sample. Stamped as late as possible.
            const auto probe = Rubylight::clockSyncRequest(++m_ClockSyncSequence, LiGetMicroseconds());
            LiSendRubylightMessage(RL_CLOCK_SYNC, probe.data(), int(probe.size()), false);
            nextClockSync = now + ClockSyncIntervalUs;
        }
        // Phase reports are sent here, never on the render thread, which must not wait for ENet.
        bool send = false;
        Rubylight::PhaseReport report;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (m_PhaseReportPending) {
                report = m_PhasePending;
                m_PhaseReportPending = false;
                send = true;
            }
        }
        if (send) {
            const auto bytes = report.encode();
            LiSendRubylightMessage(RL_PHASE_LOCK_REPORT, bytes.data(), int(bytes.size()), false);
        }
        tickerLock.lock();
        m_TickerWake.wait_for(tickerLock, std::chrono::milliseconds(20), [this] { return m_TickerStop; });
    }
}

void RubylightLink::onMessage(uint16_t type, const uint8_t* payload, int length)
{
    switch (type) {
    case RL_CLOCK_SYNC: {
        const uint64_t received = LiGetMicroseconds();
        const auto reply = Rubylight::ClockSyncReply::decode(payload, length);
        if (!reply) return;
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_ClockOffset.add(*reply, received);
        break;
    }
    case RL_HOST_TIMING: {
        const auto records = Rubylight::decodeHostTiming(payload, length);
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (const auto& record : records) {
            m_HostTiming.push_back(record);
            if (m_HostTiming.size() > HostTimingSamples) m_HostTiming.pop_front();
        }
        break;
    }
    case RL_PIPELINE_RATES: {
        const auto rates = Rubylight::PipelineRates::decode(payload, length);
        if (!rates) return;
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Rates = *rates;
        m_HaveRates = true;
        break;
    }
    case RL_LINK_NOTICE: {
        const auto notice = Rubylight::LinkNotice::decode(payload, length);
        if (!notice) return;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Rubylight link notice: %s, %u of %u kbps",
                    notice->state == Rubylight::LinkNotice::Full ? "full" :
                    notice->state == Rubylight::LinkNotice::Limited ? "limited by the network" : "at the floor",
                    notice->currentKbps, notice->ceilingKbps);
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Notice = *notice;
        m_HaveNotice = notice->state != Rubylight::LinkNotice::Full;
        break;
    }
    default:
        break;
    }
}

void RubylightLink::framePresented(uint32_t rtpTimestamp)
{
    if (!m_Running) return;
    const int64_t now = int64_t(LiGetMicroseconds());
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (const auto offset = m_ClockOffset.offsetUs()) {
        const int64_t age = Rubylight::frameAgeUs(rtpTimestamp, now, *offset);
        // Anything over a second is a stale mapping, not a latency.
        if (age >= 0 && age < 1000000) {
            m_FrameAgesUs.push_back(age);
            if (m_FrameAgesUs.size() > FrameAgeSamples) m_FrameAgesUs.pop_front();
        }
    }

    if (!m_PhaseLockRequested || !m_LatchClock) return;
    const int64_t ready = DisplayLatchClock::nowUs();
    int64_t vblank = 0, period = 0;
    if (!m_LatchClock->timing(ready, &vblank, &period)) {
        m_Slack.clear();
        m_PhaseWindowStartUs = m_PhaseLastFrameUs = 0;
        return;
    }
    if (m_PhaseLastFrameUs == 0 || ready - m_PhaseLastFrameUs > PhaseGapUs) {
        m_Slack.clear();
        m_PhaseWindowStartUs = ready;
    }
    m_PhaseLastFrameUs = ready;
    // The slack to the next latch; the window takes it modulo the period.
    m_Slack.push((vblank - m_LatchLeadUs - ready) * 1000, period * 1000);
    const int64_t elapsed = ready - m_PhaseWindowStartUs;
    if (elapsed < PhaseReportIntervalUs) return;
    m_PhaseWindowStartUs = ready;
    if (const auto report = m_Slack.reportOver(PhaseMinFrames, elapsed * 1000)) {
        m_PhasePending = *report;
        m_PhaseReportPending = true;
        m_PhaseLast = *report;
        m_PhaseReported = true;
        ++m_PhaseReports;
    }
}

int RubylightLink::formatOverlay(char* output, int length)
{
    if (!m_Running || length <= 0) return 0;
    std::lock_guard<std::mutex> lock(m_Mutex);
    int used = 0;
#define append(...) appendFormat(output, length, &used, __VA_ARGS__)
    if (!m_HostTiming.empty()) {
        append("Rubylight host present to sent: %.2f ms (queue %.2f, encode %.2f, send %.2f; median)\n",
               median(m_HostTiming, [](const auto& r) { return r.hostUs; }) / 1000.0,
               median(m_HostTiming, [](const auto& r) { return r.queueUs; }) / 1000.0,
               median(m_HostTiming, [](const auto& r) { return r.encodeUs; }) / 1000.0,
               median(m_HostTiming, [](const auto& r) { return r.sendUs; }) / 1000.0);
    }
    if (m_HaveRates) {
        const auto fps = [](uint32_t mhz) { return mhz == Rubylight::PipelineRates::Unknown ? -1.0 : mhz / 1000.0; };
        append("Rubylight host FPS: game %.2f, captured %.2f, encoded %.2f, sent %.2f, repeats %.2f\n",
               fps(m_Rates.sourceMhz), fps(m_Rates.captureMhz), fps(m_Rates.submitMhz),
               fps(m_Rates.outputMhz), fps(m_Rates.repeatMhz));
    }
    if (!m_FrameAgesUs.empty()) {
        append("Host present to client present: %.2f ms median, %.2f ms p95 (clock RTT %.2f ms)\n",
               percentile(m_FrameAgesUs, 50) / 1000.0, percentile(m_FrameAgesUs, 95) / 1000.0,
               double(m_ClockOffset.rttUs().value_or(0)) / 1000.0);
    }
    if (m_PhaseLockRequested) {
        if (m_PhaseReported) {
            append("Phase lock: frames ready %.2f ms before the latch (spread %.2f ms, %.2f Hz, %u reports)\n",
                   m_PhaseLast.leadNs / 1e6, m_PhaseLast.spreadNs / 1e6,
                   m_PhaseLast.periodNs ? 1e9 / m_PhaseLast.periodNs : 0.0, m_PhaseReports);
        }
        else {
            append("Phase lock: waiting for the display clock\n");
        }
    }
    if (m_HaveNotice) {
        append("Host bitrate %s: %.1f of %.1f Mbps\n",
               m_Notice.state == Rubylight::LinkNotice::Floor ? "at its floor for the network" : "lowered for the network",
               m_Notice.currentKbps / 1000.0, m_Notice.ceilingKbps / 1000.0);
    }
#undef append
    return used;
}
