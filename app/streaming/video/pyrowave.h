#pragma once
#include "decoder.h"
#include "ffmpeg-renderers/plvk.h"
#include <pyrowave/pyrowave.h>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

constexpr int pyroWaveVisiblePlaneDimension(int visibleDimension, bool chroma420Plane)
{
    return visibleDimension >> (chroma420Plane ? 1 : 0);
}

// PyroWave's aligned wavelet extent must never leak into renderer textures.
static_assert(pyroWaveVisiblePlaneDimension(1920, false) == 1920);
static_assert(pyroWaveVisiblePlaneDimension(1080, false) == 1080);
static_assert(pyroWaveVisiblePlaneDimension(2560, false) == 2560);
static_assert(pyroWaveVisiblePlaneDimension(1440, false) == 1440);
static_assert(pyroWaveVisiblePlaneDimension(3440, false) == 3440);
static_assert(pyroWaveVisiblePlaneDimension(3840, false) == 3840);
static_assert(pyroWaveVisiblePlaneDimension(2160, false) == 2160);
static_assert(((3440 + 31) & ~31) == 3456 && pyroWaveVisiblePlaneDimension(3440, false) != 3456);
static_assert(((2160 + 31) & ~31) == 2176 && pyroWaveVisiblePlaneDimension(2160, false) != 2176);

// GPU operations run on a dedicated render thread. submitDecodeUnit is a
// non-blocking producer for a single-slot encoded-frame mailbox; newer frames
// replace it so presentation latency cannot accumulate.
//
// With VRR presentation a decode thread replaces the render thread. It takes
// encoded frames from a single-slot mailbox (an overloaded decoder skips frames
// before decoding them), parses and submits each decode into one of a small,
// on-demand pool of plane sets, and queues the decoded frame to the VRR pacing
// worker. The worker then only renders and presents it at its target time, as
// it does for hardware-decoded codecs, so the CPU-heavy packet parsing of one
// frame overlaps the presentation of the previous one. submitDecodeUnit runs on
// the receive thread (direct submit) and never decodes.
class PyroWaveVideoDecoder final : public IVideoDecoder, private Vrr::IFramePresenter {
public:
    ~PyroWaveVideoDecoder() override;
    bool initialize(PDECODER_PARAMETERS params) override;
    bool isHardwareAccelerated() override { return true; }
    bool isAlwaysFullScreen() override { return false; }
    bool isHdrSupported() override { return true; }
    int getDecoderCapabilities() override { return CAPABILITY_DIRECT_SUBMIT; }
    int getDecoderColorspace() override { return COLORSPACE_REC_709; }
    int getDecoderColorRange() override { return COLOR_RANGE_FULL; }
    QSize getDecoderMaxResolution() override { return QSize(8192, 8192); }
    int submitDecodeUnit(PDECODE_UNIT du) override;
    void renderFrameOnMainThread() override;
    void setHdrMode(bool) override {}
    bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override;
private:
    friend class PyroWaveDecoderSmokeTest;
    static void lockQueue(void* opaque);
    static void unlockQueue(void* opaque);
    static void collectPerformanceStat(void* opaque, const char* message);
    bool borrowDevice();
    bool createPlanes();
    // Decode targets: the planes and the PyroWave views of them.
    struct PlaneSet {
        pl_tex planes[3] = {};
        pyrowave_gpu_buffers buffers = {};
        pl_frame frame = {};
    };
    bool createPlaneTextures(PlaneSet& set);
    bool decodeFrame(const std::vector<uint32_t>& bytes, size_t size,
                     uint64_t* decodeTimeUs = nullptr, uint64_t* renderTimeUs = nullptr,
                     bool rendererReady = false);
    // Decode into a plane set and describe it as a libplacebo frame.
    bool decodeToPlanes(const std::vector<uint32_t>& bytes, size_t size, PlaneSet& set,
                        pl_frame& frame, uint64_t* decodeTimeUs);
    void reportGpuStats(uint64_t nowUs);

    // VRR presentation: encoded frames wait for the decode thread; the
    // worker's payload is a decoded PlaneSet.
    struct EncodedFrame {
        std::vector<uint32_t> data;
        size_t size = 0;
        uint64_t enqueueUs = 0;
        bool presented = false;
    };
    Vrr::PrepareResult vrrPrepare(void* payload, bool latched) override;
    Vrr::PresentResult vrrPresent(bool latched) override;
    void vrrCancel() override;
    void vrrRelease(void* payload) override;
    void vrrThreadStopping() override;
    EncodedFrame* takeEncodedFrame();
    void recycleEncodedFrame(EncodedFrame* encoded);
    PlaneSet* takePlaneSet();
    void vrrDecodeLoop();
    void renderPendingFrame(bool rendererReady);
    void renderLoop();
    void recycleFrame(std::vector<uint32_t>& frame);
    void releasePlanes(PlaneSet& set, uint64_t value);
    static void addVideoStats(const VIDEO_STATS& src, VIDEO_STATS& dst, uint64_t now);
    void updatePerformanceOverlay(const VIDEO_STATS& stats);
    std::unique_ptr<PlVkRenderer> m_Renderer;
    pyrowave_device m_Device = nullptr;
    pyrowave_decoder m_Decoder = nullptr;
    VkSemaphore m_Timeline = VK_NULL_HANDLE;
    uint64_t m_Value = 0;
    // Plane set of the render thread (and of the first VRR decode).
    PlaneSet m_Planes;
    VkApplicationInfo m_AppInfo = {};
    VkInstanceCreateInfo m_InstanceInfo = {};
    VkDeviceCreateInfo m_DeviceInfo = {};
    VkDeviceQueueCreateInfo m_QueueInfo = {};
    pyrowave_device_create_queue_info m_Queue = {};
    float m_Priority = 1.0f;
    int m_Width = 0, m_Height = 0, m_Format = 0;
    PYROWAVE_DIALECT m_Dialect = PYROWAVE_DIALECT_NONE;
    bool m_FragmentPath = false;
    SDL_Window* m_Window = nullptr;
    std::mutex m_Mutex;
    std::condition_variable m_FrameReady;
    std::thread m_RenderThread;
    std::vector<uint8_t> m_BlockSeen;
    std::vector<uint32_t> m_Pending;
    std::vector<uint32_t> m_Spare;
    size_t m_PendingSize = 0;
    bool m_EventQueued = false;
    bool m_Threaded = false;
    bool m_Stopping = false;
    bool m_OverlayAttached = false;
    uint64_t m_EnqueueTime = 0;
    uint32_t m_PendingRtp = 0;
    uint64_t m_LastStatsTime = 0;
    uint64_t m_LastGpuStatsLogTime = 0;
    double m_GpuDequantMs = 0.0;
    double m_GpuIdwtMs = 0.0;
    VIDEO_STATS m_ActiveVideoStats = {};
    VIDEO_STATS m_LastVideoStats = {};
    VIDEO_STATS m_PendingOverlayStats = {};
    bool m_OverlayRefreshPending = false;
    int m_LastFrameNumber = 0;

    std::unique_ptr<Vrr::PacingWorker> m_VrrWorker;
    bool m_VrrRequested = false;
    Vrr::FallbackReason m_VrrFallback = Vrr::FallbackReason::NotRequested;
    // Recycled encoded-frame buffers: no allocation once warmed up.
    std::vector<EncodedFrame*> m_FreeEncodedFrames;
    // Plane sets owned by the decode thread or the worker; created on demand,
    // at most one more than the worker can own.
    std::vector<std::unique_ptr<PlaneSet>> m_VrrSets;
    std::vector<PlaneSet*> m_FreeSets;
    std::thread m_DecodeThread;
    std::condition_variable m_DecodeWake;
    EncodedFrame* m_DecodePending = nullptr;
    Vrr::FrameTiming m_DecodePendingTiming;
    bool m_DecodeStopping = false;
    uint64_t m_DecodeSkipped = 0;
    uint64_t m_DecodeSkippedAccumulated = 0;
    Vrr::PacingWorker::Stats m_VrrOverlayLast;
    Vrr::PacingWorker::Stats m_VrrAccumulated;
    uint64_t m_VrrLastLogUs = 0;
    Vrr::PacingWorker::Stats m_VrrLogLast;
    // Where VRR decode and preparation time goes, summed under m_Mutex.
    struct PreparationSplit {
        uint64_t decodedFrames = 0;
        uint64_t decodeSubmitUs = 0;
        uint64_t frames = 0;
        uint64_t renderSubmitUs = 0;
        uint64_t gpuWaitUs = 0;
    };
    PreparationSplit m_VrrSplit;
    PreparationSplit m_VrrSplitLogged;
    void logPreparationSplit(const char* title, PreparationSplit& since);
};
