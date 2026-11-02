#include "pacer.h"
#include "streaming/streamutils.h"
#include "streaming/session.h"
#include "vrrdiagnostics.h"

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "dxvsyncsource.h"
#endif

#ifdef HAS_WAYLAND
#include "waylandvsyncsource.h"
#endif

#include <SDL_syswm.h>

// Limit the number of queued frames to prevent excessive memory consumption
// if the V-Sync source or renderer is blocked for a while. It's important
// that the sum of all queued frames between both pacing and rendering queues
// must not exceed the number buffer pool size to avoid running the decoder
// out of available decoding surfaces.
#define MAX_QUEUED_FRAMES 3
static_assert(PACER_MAX_OUTSTANDING_FRAMES == MAX_QUEUED_FRAMES + 2,
              "PACER_MAX_OUTSTANDING_FRAMES and MAX_QUEUED_FRAMES must agree");

// We may be woken up slightly late so don't go all the way
// up to the next V-sync since we may accidentally step into
// the next V-sync period. It also takes some amount of time
// to do the render itself, so we can't render right before
// V-sync happens.
#define TIMER_SLACK_MS 3

Pacer::Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats) :
    m_RenderThread(nullptr),
    m_VsyncThread(nullptr),
    m_DeferredFreeFrame(nullptr),
    m_Stopping(false),
    m_VsyncSource(nullptr),
    m_VsyncRenderer(renderer),
    m_MaxVideoFps(0),
    m_DisplayFps(0),
    m_VideoStats(videoStats)
{

}

Pacer::~Pacer()
{
    // The VRR worker presents through the renderer: stop it first. Its thread
    // releases the presenter state; queued frames are freed here.
    if (m_VrrWorker) {
        Vrr::PacingWorker::Stats sessionStart = {};
        logVrrSummary("VRR session summary", sessionStart);
    }
    m_VrrWorker.reset();

    m_Stopping = true;

    // Stop the V-sync thread
    if (m_VsyncThread != nullptr) {
        m_PacingQueueNotEmpty.wakeAll();
        m_VsyncSignalled.wakeAll();
        SDL_WaitThread(m_VsyncThread, nullptr);
    }

    // Stop V-sync callbacks
    delete m_VsyncSource;
    m_VsyncSource = nullptr;

    // Stop the render thread
    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_RenderThread, nullptr);
    }
    else {
        // Stop any further overlay callbacks
        Session::get()->getOverlayManager().setOverlayRenderer(nullptr);

        // Notify the renderer that it is being destroyed soon
        // NB: This must happen on the same thread that calls renderFrame().
        m_VsyncRenderer->cleanupRenderContext();
    }

    // Delete any remaining unconsumed frames
    while (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        av_frame_free(&frame);
    }
    while (!m_PacingQueue.isEmpty()) {
        AVFrame* frame = m_PacingQueue.dequeue();
        av_frame_free(&frame);
    }
    av_frame_free(&m_DeferredFreeFrame);
}

void Pacer::renderOnMainThread()
{
    // Ignore this call for renderers that work on a dedicated render thread
    if (m_RenderThread != nullptr) {
        return;
    }

    m_FrameQueueLock.lock();

    if (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        m_FrameQueueLock.unlock();

        renderFrame(frame);
    }
    else {
        m_FrameQueueLock.unlock();
    }
}

int Pacer::vsyncThread(void *context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    bool async = me->m_VsyncSource->isAsync();
    while (!me->m_Stopping) {
        if (async) {
            // Wait for the VSync source to invoke signalVsync() or 100ms to elapse
            QMutexLocker locker(&me->m_FrameQueueLock);
            me->m_VsyncSignalled.wait(&me->m_FrameQueueLock, 100);
        }
        else {
            // Let the VSync source wait in the context of our thread
            if (!me->m_VsyncSource->waitForVsync()) {
                // Fall back to a fixed delay if the VSync source fails
                //
                // NB: We don't actually expect signalVsync() to be called, since
                // this is a synchronous VSync source, but we wait on m_VsyncSignalled
                // anyway so that the destructor can immediately wake us on shutdown.
                QMutexLocker locker(&me->m_FrameQueueLock);
                me->m_VsyncSignalled.wait(&me->m_FrameQueueLock, 1000 / me->m_DisplayFps);
            }
        }

        if (me->m_Stopping) {
            break;
        }

        me->handleVsync(1000 / me->m_DisplayFps);
    }

    return 0;
}

int Pacer::renderThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set render thread to high priority: %s",
                    SDL_GetError());
    }

    while (!me->m_Stopping) {
        // Wait for the renderer to be ready for the next frame
        me->m_VsyncRenderer->waitToRender();

        AVFrame* frame;
        {
            // Acquire the frame queue lock to protect the queue and
            // the not empty condition
            QMutexLocker locker(&me->m_FrameQueueLock);

            // Wait for a frame to be ready to render
            while (!me->m_Stopping && me->m_RenderQueue.isEmpty()) {
                me->m_RenderQueueNotEmpty.wait(&me->m_FrameQueueLock);
            }

            if (me->m_Stopping) {
                // Exit this thread
                break;
            }

            frame = me->m_RenderQueue.dequeue();
        }

        me->renderFrame(frame);
    }

    // Stop any further overlay callbacks
    Session::get()->getOverlayManager().setOverlayRenderer(nullptr);

    // Notify the renderer that it is being destroyed soon
    // NB: This must happen on the same thread that calls renderFrame().
    me->m_VsyncRenderer->cleanupRenderContext();

    return 0;
}

void Pacer::enqueueFrameForRenderingAndUnlock(AVFrame *frame)
{
    dropFrameForEnqueue(m_RenderQueue);
    m_RenderQueue.enqueue(frame);

    m_FrameQueueLock.unlock();

    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeOne();
    }
    else {
        SDL_Event event;

        // For main thread rendering, we'll push an event to trigger a callback
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_FRAME_READY;
        SDL_PushEvent(&event);
    }
}

// Called in an arbitrary thread by the IVsyncSource on V-sync
// or an event synchronized with V-sync
void Pacer::handleVsync(int timeUntilNextVsyncMillis)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    m_FrameQueueLock.lock();

    // If the queue length history entries are large, be strict
    // about dropping excess frames.
    int frameDropTarget = 1;

    // If we may get more frames per second than we can display, use
    // frame history to drop frames only if consistently above the
    // one queued frame mark.
    if (m_MaxVideoFps >= m_DisplayFps) {
        for (int queueHistoryEntry : std::as_const(m_PacingQueueHistory)) {
            if (queueHistoryEntry <= 1) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 3;
                break;
            }
        }

        // Keep a rolling 500 ms window of pacing queue history
        if (m_PacingQueueHistory.count() == m_DisplayFps / 2) {
            m_PacingQueueHistory.dequeue();
        }

        m_PacingQueueHistory.enqueue(m_PacingQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_PacingQueue.count() > frameDropTarget) {
        AVFrame* frame = m_PacingQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    if (m_PacingQueue.isEmpty()) {
        // Wait for a frame to arrive or our V-sync timeout to expire
        if (!m_PacingQueueNotEmpty.wait(&m_FrameQueueLock, SDL_max(timeUntilNextVsyncMillis, TIMER_SLACK_MS) - TIMER_SLACK_MS)) {
            // Wait timed out - unlock and bail
            m_FrameQueueLock.unlock();
            return;
        }

        if (m_Stopping || m_PacingQueue.isEmpty()) {
            m_FrameQueueLock.unlock();
            return;
        }
    }

    // Place the first frame on the render queue
    enqueueFrameForRenderingAndUnlock(m_PacingQueue.dequeue());
}

static uint64_t vrrClock()
{
    return LiGetMicroseconds();
}

bool Pacer::initialize(SDL_Window* window, int maxVideoFps, bool enablePacing,
                       const VRR_PARAMETERS* vrr)
{
    m_MaxVideoFps = maxVideoFps;
    m_DisplayFps = StreamUtils::getDisplayRefreshRate(window);
    m_RendererAttributes = m_VsyncRenderer->getRendererAttributes();

    m_VrrRequested = vrr != nullptr && vrr->enabled;
    if (m_VrrRequested) {
        Vrr::PresentProtection protection = Vrr::PresentProtection::SoftwareFloor;
        m_VrrFallback = Vrr::FallbackReason::UnsupportedRenderer;
        Vrr::IFramePresenter* presenter = m_VsyncRenderer->isRenderThreadSupported() ?
            m_VsyncRenderer->getVrrPresenter(&protection, &m_VrrFallback) : nullptr;
        if (presenter != nullptr) {
            Vrr::SessionConfig config;
            config.displayRefreshHz = vrr->displayRefreshHz;
            config.streamRateHz = maxVideoFps;
            config.latencyMode = vrr->latencyMode;
            config.timing = vrr->timing;
            config.reduceJudder = vrr->reduceJudder;
            config.protection = protection;
            m_VrrWorker = std::make_unique<Vrr::PacingWorker>(config, presenter, vrrClock);
            m_VrrWorker->setStopStallReporter(Vrr::logStopStall);
            if (m_VrrWorker->start([] {
                    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL) < 0) {
                        SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
                    }
                })) {
                m_VrrFallback = Vrr::FallbackReason::NoFallback;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "VRR presentation active: %d FPS stream on a %d Hz display (%s, %s protection)",
                            maxVideoFps, vrr->displayRefreshHz, m_VsyncRenderer->getVrrPresentModeName(),
                            protection == Vrr::PresentProtection::Native ? "native" : "software floor");

                // The VRR worker replaces the render thread, so it needs the
                // same final preparations and overlay registration below.
                m_VsyncRenderer->prepareToRender();
                Session::get()->getOverlayManager().setOverlayRenderer(m_VsyncRenderer);
                return true;
            }
            m_VrrWorker.reset();
            m_VrrFallback = Vrr::FallbackReason::InitializationFailed;
        }
        // A requested VRR session that cannot present adaptively keeps
        // synchronized presentation with classic pacing, never the adaptive
        // swapchain the renderer prepared for VRR.
        if (!m_VsyncRenderer->restoreSynchronizedPresentation()) {
            return false;
        }
        enablePacing = true;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "VRR presentation unavailable (%s); using fixed frame pacing",
                    Vrr::fallbackReasonName(m_VrrFallback));
    }

    if (enablePacing) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);

        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetWindowWMInfo() failed: %s",
                         SDL_GetError());
            return false;
        }

        switch (info.subsystem) {
    #ifdef Q_OS_WIN32
        case SDL_SYSWM_WINDOWS:
            m_VsyncSource = new DxVsyncSource(this);
            break;
    #endif

    #if defined(SDL_VIDEO_DRIVER_WAYLAND) && defined(HAS_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            m_VsyncSource = new WaylandVsyncSource(this);
            break;
    #endif

        default:
            // Platforms without a VsyncSource will just render frames
            // immediately like they used to.
            break;
        }

        SDL_assert(m_VsyncSource != nullptr || !(m_RendererAttributes & RENDERER_ATTRIBUTE_FORCE_PACING));

        if (m_VsyncSource != nullptr && !m_VsyncSource->initialize(window, m_DisplayFps)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vsync source failed to initialize. Frame pacing will not be available!");
            delete m_VsyncSource;
            m_VsyncSource = nullptr;
        }
        else if (m_VsyncSource != nullptr) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Frame pacing V-sync source: %s",
                        info.subsystem == SDL_SYSWM_WAYLAND ? "Wayland frame callbacks" : "DXGI");
        }
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing disabled: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);
    }

    if (m_VsyncSource != nullptr) {
        m_VsyncThread = SDL_CreateThread(Pacer::vsyncThread, "PacerVsync", this);
    }

    // Allow the renderer to perform final preparations for rendering
    m_VsyncRenderer->prepareToRender();

    // Tell overlay manager to use this frontend renderer
    Session::get()->getOverlayManager().setOverlayRenderer(m_VsyncRenderer);

    if (m_VsyncRenderer->isRenderThreadSupported()) {
        m_RenderThread = SDL_CreateThread(Pacer::renderThread, "PacerRender", this);
    }

    return true;
}

void Pacer::signalVsync()
{
    m_VsyncSignalled.wakeOne();
}

void Pacer::renderFrame(AVFrame* frame)
{
    // Count time spent in Pacer's queues
    uint64_t beforeRender = LiGetMicroseconds();
    m_VideoStats->totalPacerTimeUs += (beforeRender - (uint64_t)frame->pkt_dts);

    // Render it
    m_VsyncRenderer->acquireRenderLock();
    m_VsyncRenderer->renderFrame(frame);
    m_VsyncRenderer->releaseRenderLock();
    uint64_t afterRender = LiGetMicroseconds();

    m_VideoStats->totalRenderTimeUs += (afterRender - beforeRender);
    m_VideoStats->renderedFrames++;

    // Wait until after next frame to free this one to ensure the GPU
    // doesn't stall or read garbage if the backing buffer gets returned
    // to the pool and the decoder tries to write a new frame into it
    std::swap(frame, m_DeferredFreeFrame);
    av_frame_free(&frame);

    // Drop frames if we have too many queued up for a while
    QMutexLocker frameQueueLocker(&m_FrameQueueLock);

    int frameDropTarget;

    if (m_RendererAttributes & RENDERER_ATTRIBUTE_NO_BUFFERING) {
        // Renderers that don't buffer any frames but don't support waitToRender() need us to buffer
        // an extra frame to ensure they don't starve while waiting to present.
        frameDropTarget = 1;
    }
    else {
        frameDropTarget = 0;
        for (int queueHistoryEntry : std::as_const(m_RenderQueueHistory)) {
            if (queueHistoryEntry == 0) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 2;
                break;
            }
        }

        // Keep a rolling 500 ms window of render queue history
        if (m_RenderQueueHistory.count() == m_MaxVideoFps / 2) {
            m_RenderQueueHistory.dequeue();
        }

        m_RenderQueueHistory.enqueue(m_RenderQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_RenderQueue.count() > frameDropTarget) {
        AVFrame* frame = m_RenderQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        frameQueueLocker.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        frameQueueLocker.relock();
    }
}

void Pacer::dropFrameForEnqueue(QQueue<AVFrame*>& queue)
{
    SDL_assert(queue.size() <= MAX_QUEUED_FRAMES);
    if (queue.size() == MAX_QUEUED_FRAMES) {
        AVFrame* frame = queue.dequeue();
        av_frame_free(&frame);
    }
}

void Pacer::submitFrame(AVFrame* frame, const Vrr::FrameTiming* timing)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    if (m_VrrWorker) {
        Vrr::FrameTiming frameTiming;
        if (timing) {
            frameTiming = *timing;
        }
        else {
            frameTiming.readyUs = LiGetMicroseconds();
        }
        // The worker owns the frame from here and frees it when done.
        m_VrrWorker->submit(frameTiming, frame);
        return;
    }

    // Queue the frame and possibly wake up the render thread
    m_FrameQueueLock.lock();
    if (m_VsyncSource != nullptr) {
        dropFrameForEnqueue(m_PacingQueue);
        m_PacingQueue.enqueue(frame);
        m_FrameQueueLock.unlock();
        m_PacingQueueNotEmpty.wakeOne();
    }
    else {
        enqueueFrameForRenderingAndUnlock(frame);
    }
}

Vrr::PacingWorker::Stats Pacer::vrrStats() const
{
    return m_VrrWorker ? m_VrrWorker->stats() : Vrr::PacingWorker::Stats{};
}

void Pacer::accumulateVrrStats()
{
    if (!m_VrrWorker) {
        return;
    }
    const Vrr::PacingWorker::Stats now = m_VrrWorker->stats();
    const Vrr::PacingWorker::Stats& last = m_VrrAccumulated;
    const uint64_t presented = now.presented - last.presented;
    const uint64_t dropped = (now.queueDrops - last.queueDrops) + (now.staleDrops - last.staleDrops) +
                             (now.failedPreparations - last.failedPreparations);
    const uint64_t preparation = now.totalPreparationUs - last.totalPreparationUs;
    const uint64_t presentCall = now.totalPresentCallUs - last.totalPresentCallUs;
    const uint64_t queued = now.totalArrivalToPresentUs - last.totalArrivalToPresentUs;
    m_VideoStats->renderedFrames += uint32_t(presented);
    m_VideoStats->pacerDroppedFrames += uint32_t(dropped);
    m_VideoStats->totalRenderTimeUs += preparation + presentCall;
    // Frame queue delay: arrival to submission (queueing, playout delay and
    // target waits), excluding preparation.
    m_VideoStats->totalPacerTimeUs += queued > preparation ? queued - preparation : 0;
    m_VrrAccumulated = now;

    // A periodic summary in the log, without per-frame logging.
    const uint64_t nowUs = LiGetMicroseconds();
    if (!m_VrrLastLogUs) {
        m_VrrLastLogUs = nowUs;
    }
    else if (nowUs - m_VrrLastLogUs >= 10000000) {
        logVrrSummary("VRR presentation (last 10 s)", m_VrrLogLast);
        m_VrrLastLogUs = nowUs;
    }
}

int Pacer::formatVrrStats(char* output, int length)
{
    if (!m_VrrWorker) {
        return m_VrrRequested ? Vrr::formatFallback(m_VrrFallback, output, length) : 0;
    }
    return Vrr::formatOverlay(m_VrrWorker->stats(), m_VrrOverlayLast, m_VrrWorker->config(),
                              m_VsyncRenderer->getVrrPresentModeName(), output, length);
}

void Pacer::logVrrSummary(const char* title, Vrr::PacingWorker::Stats& last)
{
    if (!m_VrrWorker) {
        return;
    }
    char summary[1024];
    if (Vrr::formatSummary(title, m_VrrWorker->stats(), last, summary, sizeof(summary))) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", summary);
    }
}
