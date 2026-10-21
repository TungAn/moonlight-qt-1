#pragma once

#include "../../decoder.h"
#include "../renderer.h"
#include "vrrpacingworker.h"

#include <memory>

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>

// The maximum number of frames pacer will ever hold is:
// - 3 frames in the pacing queue
// - 1 frame removed from the render queue in the process of rendering
// - 1 frame for deferred free
// The VRR worker holds at most Vrr::kOwnedFrames (4 waiting + 1 presenting).
#define PACER_MAX_OUTSTANDING_FRAMES (3 + 1 + 1)
static_assert(Vrr::kOwnedFrames <= PACER_MAX_OUTSTANDING_FRAMES,
              "decoder surface pools reserve PACER_MAX_OUTSTANDING_FRAMES for the pacer");

class IVsyncSource {
public:
    virtual ~IVsyncSource() {}
    virtual bool initialize(SDL_Window* window, int displayFps) = 0;

    // Asynchronous sources produce callbacks on their own, while synchronous
    // sources require calls to waitForVsync().
    virtual bool isAsync() = 0;

    virtual bool waitForVsync() {
        // Synchronous sources must implement waitForVsync()!
        SDL_assert(false);
        return false;
    }
};

class Pacer
{
public:
    Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats);

    ~Pacer();

    // `timing` is required for VRR presentation; legacy pacing ignores it.
    void submitFrame(AVFrame* frame, const Vrr::FrameTiming* timing = nullptr);

    // With `vrr` enabled and a renderer that can present adaptively, frames
    // are paced by the VRR worker; otherwise by the classic V-Sync pacer
    // (forced on when VRR was requested, so presentation stays synchronized).
    bool initialize(SDL_Window* window, int maxVideoFps, bool enablePacing,
                    const VRR_PARAMETERS* vrr = nullptr);

    bool vrrActive() const { return m_VrrWorker != nullptr; }
    bool vrrRequested() const { return m_VrrRequested; }
    Vrr::FallbackReason vrrFallbackReason() const { return m_VrrFallback; }
    const char* vrrPresentModeName() { return m_VsyncRenderer->getVrrPresentModeName(); }
    Vrr::PacingWorker::Stats vrrStats() const;
    // Add the VRR worker's presented/dropped frames and timing since the last
    // call to the decoder's video statistics window.
    void accumulateVrrStats();
    // Append VRR state and quality since the previous call to `output`.
    // Returns the number of characters written.
    int formatVrrStats(char* output, int length);
    void logVrrSummary(const char* title, Vrr::PacingWorker::Stats& last);

    void signalVsync();

    void renderOnMainThread();

private:
    static int vsyncThread(void* context);

    static int renderThread(void* context);

    void handleVsync(int timeUntilNextVsyncMillis);

    void enqueueFrameForRenderingAndUnlock(AVFrame* frame);

    void renderFrame(AVFrame* frame);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue);

    QQueue<AVFrame*> m_RenderQueue;
    QQueue<AVFrame*> m_PacingQueue;
    QQueue<int> m_PacingQueueHistory;
    QQueue<int> m_RenderQueueHistory;
    QMutex m_FrameQueueLock;
    QWaitCondition m_RenderQueueNotEmpty;
    QWaitCondition m_PacingQueueNotEmpty;
    QWaitCondition m_VsyncSignalled;
    SDL_Thread* m_RenderThread;
    SDL_Thread* m_VsyncThread;
    AVFrame* m_DeferredFreeFrame;
    bool m_Stopping;

    IVsyncSource* m_VsyncSource;
    IFFmpegRenderer* m_VsyncRenderer;
    int m_MaxVideoFps;
    int m_DisplayFps;
    PVIDEO_STATS m_VideoStats;
    int m_RendererAttributes;

    std::unique_ptr<Vrr::PacingWorker> m_VrrWorker;
    bool m_VrrRequested = false;
    Vrr::FallbackReason m_VrrFallback = Vrr::FallbackReason::NotRequested;
    Vrr::PacingWorker::Stats m_VrrAccumulated;
    Vrr::PacingWorker::Stats m_VrrOverlayLast;
    Vrr::PacingWorker::Stats m_VrrLogLast;
    uint64_t m_VrrLastLogUs = 0;
};
