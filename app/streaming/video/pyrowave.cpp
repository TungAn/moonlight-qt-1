#include "pyrowaveframing.h"
#include "pyrowave.h"
#include "ffmpeg-renderers/pacer/vrrdiagnostics.h"
#include "pyrowavecolor.h"
#include "pyrowaveframe.h"
#include <PyroWave.h>
#include "streaming/session.h"
#include "streaming/streamutils.h"
#include <QtEndian>
#include <cstring>
#include <cstdio>

PyroWaveVideoDecoder::~PyroWaveVideoDecoder()
{
    // The decode thread feeds the VRR worker, which presents through the
    // planes and renderer: stop them in that order.
    if (m_DecodeThread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_DecodeStopping = true;
        }
        m_DecodeWake.notify_one();
        m_DecodeThread.join();
    }
    if (m_DecodePending) {
        recycleEncodedFrame(m_DecodePending);
        m_DecodePending = nullptr;
    }
    if (m_VrrWorker) {
        Vrr::PacingWorker::Stats last = {};
        char summary[1024];
        if (Vrr::formatSummary("VRR session summary (PyroWave)", m_VrrWorker->stats(), last, summary, sizeof(summary))) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", summary);
        }
        PreparationSplit sessionStart;
        logPreparationSplit("VRR decode and preparation (PyroWave, session)", sessionStart);
    }
    m_VrrWorker.reset();
    for (EncodedFrame* encoded : m_FreeEncodedFrames) delete encoded;
    m_FreeEncodedFrames.clear();
    if (m_OverlayAttached) Session::get()->getOverlayManager().setOverlayRenderer(nullptr);
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stopping = true;
    }
    m_FrameReady.notify_one();
    if (m_RenderThread.joinable()) m_RenderThread.join();
    if (m_Decoder) pyrowave_decoder_destroy(m_Decoder);
    if (m_Device) pyrowave_device_destroy(m_Device);
    if (m_Renderer && m_Renderer->getVulkan()) {
        auto vk = m_Renderer->getVulkan();
        // The timeline remains alive until libplacebo has finished all its waits.
        pl_gpu_finish(vk->gpu);
        for (auto& plane : m_Planes.planes) pl_tex_destroy(vk->gpu, &plane);
        for (auto& set : m_VrrSets) {
            for (auto& plane : set->planes) pl_tex_destroy(vk->gpu, &plane);
        }
        if (m_Timeline) {
            auto destroy = reinterpret_cast<PFN_vkDestroySemaphore>(vk->get_proc_addr(vk->instance, "vkDestroySemaphore"));
            destroy(vk->device, m_Timeline, nullptr);
        }
    }
}

void PyroWaveVideoDecoder::lockQueue(void* opaque)
{
    auto self = static_cast<PyroWaveVideoDecoder*>(opaque);
    auto vk = self->m_Renderer->getVulkan();
    vk->lock_queue(vk, self->m_Queue.familyIndex, 0);
}

void PyroWaveVideoDecoder::unlockQueue(void* opaque)
{
    auto self = static_cast<PyroWaveVideoDecoder*>(opaque);
    auto vk = self->m_Renderer->getVulkan();
    vk->unlock_queue(vk, self->m_Queue.familyIndex, 0);
}

void PyroWaveVideoDecoder::collectPerformanceStat(void* opaque, const char* message)
{
    auto self = static_cast<PyroWaveVideoDecoder*>(opaque);
    double milliseconds;
    if (std::sscanf(message, "Dequant: %lf ms per frame", &milliseconds) == 1) {
        self->m_GpuDequantMs = milliseconds;
    }
    else if (std::sscanf(message, "iDWT: %lf ms per frame", &milliseconds) == 1 ||
             std::sscanf(message, "iDWT fragment: %lf ms per frame", &milliseconds) == 1) {
        self->m_GpuIdwtMs = milliseconds;
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "PyroWave GPU: %s", message);
}

bool PyroWaveVideoDecoder::borrowDevice()
{
    auto vk = m_Renderer->getVulkan();
    auto inst = m_Renderer->getVulkanInstance();
    auto getPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        vk->get_proc_addr(vk->instance, "vkGetPhysicalDeviceProperties"));
    if (!getPhysicalDeviceProperties) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave could not query the Vulkan physical device");
        return false;
    }
    VkPhysicalDeviceProperties properties {};
    getPhysicalDeviceProperties(vk->phys_device, &properties);
    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave refuses software Vulkan device '%s'; a hardware Vulkan GPU is required",
                     properties.deviceName);
        return false;
    }
    m_AppInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    m_AppInfo.apiVersion = inst->api_version;
    m_InstanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    m_InstanceInfo.pApplicationInfo = &m_AppInfo;
    m_InstanceInfo.enabledExtensionCount = inst->num_extensions;
    m_InstanceInfo.ppEnabledExtensionNames = inst->extensions;
    m_QueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    m_QueueInfo.queueFamilyIndex = vk->queue_graphics.index;
    m_QueueInfo.queueCount = 1;
    m_QueueInfo.pQueuePriorities = &m_Priority;
    m_DeviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    m_DeviceInfo.pNext = vk->features;
    m_DeviceInfo.queueCreateInfoCount = 1;
    m_DeviceInfo.pQueueCreateInfos = &m_QueueInfo;
    m_DeviceInfo.enabledExtensionCount = vk->num_extensions;
    m_DeviceInfo.ppEnabledExtensionNames = vk->extensions;
    m_Queue.familyIndex = vk->queue_graphics.index;
    auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(vk->get_proc_addr(vk->instance, "vkGetDeviceQueue"));
    if (!getQueue) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave could not obtain vkGetDeviceQueue");
        return false;
    }
    getQueue(vk->device, m_Queue.familyIndex, 0, &m_Queue.queue);
    pyrowave_device_create_info info {};
    info.GetInstanceProcAddr = vk->get_proc_addr;
    info.instance = vk->instance;
    info.physical_device = vk->phys_device;
    info.device = vk->device;
    info.instance_create_info = &m_InstanceInfo;
    info.device_create_info = &m_DeviceInfo;
    info.queue_info = &m_Queue;
    info.queue_info_count = 1;
    info.queue_lock_callback = lockQueue;
    info.queue_unlock_callback = unlockQueue;
    info.userdata = this;
    const auto createResult = pyrowave_create_device(&info, &m_Device);
    if (createResult != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave failed to borrow the Vulkan device (result %d)", int(createResult));
        return false;
    }
    const auto queueResult = pyrowave_device_set_queue_type(m_Device, VK_QUEUE_GRAPHICS_BIT);
    if (queueResult != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave failed to select the graphics queue (result %d)", int(queueResult));
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave hardware Vulkan decode device: %s (vendor 0x%04x, device 0x%04x)",
                properties.deviceName, properties.vendorID, properties.deviceID);
    return true;
}

bool PyroWaveVideoDecoder::createPlanes()
{
    auto vk = m_Renderer->getVulkan();
    VkSemaphoreTypeCreateInfo type {};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type;
    auto create = reinterpret_cast<PFN_vkCreateSemaphore>(vk->get_proc_addr(vk->instance, "vkCreateSemaphore"));
    if (create(vk->device, &info, nullptr, &m_Timeline) != VK_SUCCESS) return false;
    return createPlaneTextures(m_Planes);
}

bool PyroWaveVideoDecoder::createPlaneTextures(PlaneSet& set)
{
    auto vk = m_Renderer->getVulkan();
    for (int p = 0; p < 3; p++) {
        const int shift = p && !(m_Format & VIDEO_FORMAT_PYROWAVE_444) ? 1 : 0;
        pl_tex_params params {};
        // These are the visible stream dimensions. PyroWave keeps its 32-pixel
        // wavelet alignment internal; exposing aligned extents here would make
        // libplacebo rescale padded rows/columns back into the visible picture.
        params.w = pyroWaveVisiblePlaneDimension(m_Width, shift != 0);
        params.h = pyroWaveVisiblePlaneDimension(m_Height, shift != 0);
        params.format = pl_find_named_fmt(vk->gpu, "r16");
        params.sampleable = true;
        params.storable = !m_FragmentPath;
        params.renderable = m_FragmentPath;
        if (!params.format || !(set.planes[p] = pl_tex_create(vk->gpu, &params))) return false;
        auto& view = set.buffers.planes[p];
        view.image = pl_vulkan_unwrap(vk->gpu, set.planes[p], &view.image_format, nullptr);
        view.width = params.w;
        view.height = params.h;
        view.view_format = view.image_format;
        view.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        view.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
        view.layout = VK_IMAGE_LAYOUT_GENERAL;
        if (!view.image) return false;
    }
    return true;
}

bool PyroWaveVideoDecoder::initialize(PDECODER_PARAMETERS params)
{
    uint32_t major, minor, patch;
    pyrowave_get_api_version(&major, &minor, &patch);
    if (major != 1 || minor != 1 || patch != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave C API %u.%u.%u is incompatible; exactly 1.1.0 is required",
                     major, minor, patch);
        return false;
    }
    if (params->width <= 0 || params->height <= 0 || params->width > 8192 || params->height > 8192 ||
        params->vds == StreamingPreferences::VDS_FORCE_SOFTWARE) return false;
    m_Width = params->width;
    m_Height = params->height;
    m_Format = params->videoFormat;
    m_Dialect = LiGetPyroWaveDialect();
    m_Window = params->window;
    m_Renderer = std::make_unique<PlVkRenderer>();
    if (!m_Renderer->initialize(params) || !borrowDevice()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave cannot share the Vulkan rendering device");
        return false;
    }
    pyrowave_decoder_create_info info {};
    info.device = m_Device;
    info.width = m_Width;
    info.height = m_Height;
    const auto profile = LiPyroWaveProfile(m_Format);
    info.chroma = profile.chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    bool fragmentOverrideSet = false;
    const int fragmentOverride = qEnvironmentVariableIntValue("PYROWAVE_FRAGMENT_PATH", &fragmentOverrideSet);
    m_FragmentPath = fragmentOverrideSet ? fragmentOverride != 0 :
        pyrowave_decoder_device_prefers_fragment_path(m_Device);
    info.fragment_path = m_FragmentPath;
    if (pyrowave_decoder_create(&info, &m_Decoder) != PYROWAVE_SUCCESS || !createPlanes()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave decoder/plane creation failed");
        return false;
    }
    if (!params->testOnly) {
        Session::get()->getOverlayManager().setOverlayRenderer(m_Renderer.get());
        m_OverlayAttached = true;
        m_VrrRequested = params->vrr.enabled;
        if (m_VrrRequested) {
            Vrr::PresentProtection protection = Vrr::PresentProtection::SoftwareFloor;
            m_VrrFallback = Vrr::FallbackReason::UnsupportedRenderer;
            if (m_Renderer->getVrrPresenter(&protection, &m_VrrFallback)) {
                Vrr::SessionConfig config;
                config.displayRefreshHz = params->vrr.displayRefreshHz;
                config.streamRateHz = params->frameRate;
                config.latencyMode = params->vrr.latencyMode;
                config.timing = params->vrr.timing;
                config.reduceJudder = params->vrr.reduceJudder;
                config.protection = protection;
                m_VrrWorker = std::make_unique<Vrr::PacingWorker>(config, static_cast<Vrr::IFramePresenter*>(this), [] { return LiGetMicroseconds(); });
                m_VrrWorker->setStopStallReporter(Vrr::logStopStall);
                if (m_VrrWorker->start([] {
                        if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL) < 0) {
                            SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
                        }
                    })) {
                    m_FreeSets.push_back(&m_Planes);
                    try {
                        m_DecodeThread = std::thread(&PyroWaveVideoDecoder::vrrDecodeLoop, this);
                    }
                    catch (const std::system_error& error) {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to start PyroWave decode thread: %s", error.what());
                    }
                }
                if (m_DecodeThread.joinable()) {
                    m_VrrFallback = Vrr::FallbackReason::NoFallback;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "PyroWave VRR presentation active: %d FPS stream on a %d Hz display (%s)",
                                params->frameRate, params->vrr.displayRefreshHz, m_Renderer->getVrrPresentModeName());
                }
                else {
                    m_VrrWorker.reset();
                    m_FreeSets.clear();
                    m_VrrFallback = Vrr::FallbackReason::InitializationFailed;
                }
            }
            if (!m_VrrWorker) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "PyroWave VRR presentation unavailable (%s); presenting on decode",
                            Vrr::fallbackReasonName(m_VrrFallback));
                // The workers are stopped; present synchronized, not on the
                // adaptive swapchain prepared for VRR.
                if (!m_Renderer->restoreSynchronizedPresentation()) {
                    return false;
                }
            }
        }
        if (!m_VrrWorker) {
            try {
                m_Threaded = true;
                m_RenderThread = std::thread(&PyroWaveVideoDecoder::renderLoop, this);
            }
            catch (const std::system_error& error) {
                m_Threaded = false;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to start PyroWave render thread: %s", error.what());
                return false;
            }
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Native PyroWave Vulkan decoder: %dx%d@%d, %s, %s, %s path; display %d Hz, V-sync %s, frame pacing %s",
                m_Width, m_Height, params->frameRate,
                m_Format & VIDEO_FORMAT_PYROWAVE_444 ? "4:4:4" : "4:2:0",
                m_Format & VIDEO_FORMAT_PYROWAVE_HDR ? "HDR" : "SDR",
                m_FragmentPath ? "fragment" : "compute",
                StreamUtils::getDisplayRefreshRate(m_Window),
                params->enableVsync ? "on" : "off",
                params->enableFramePacing ? "on" : "off");
    return true;
}

void PyroWaveVideoDecoder::addVideoStats(const VIDEO_STATS& src, VIDEO_STATS& dst, uint64_t now)
{
    dst.receivedFrames += src.receivedFrames;
    dst.decodedFrames += src.decodedFrames;
    dst.renderedFrames += src.renderedFrames;
    dst.totalFrames += src.totalFrames;
    dst.networkDroppedFrames += src.networkDroppedFrames;
    dst.pacerDroppedFrames += src.pacerDroppedFrames;
    dst.totalReassemblyTimeUs += src.totalReassemblyTimeUs;
    dst.totalDecodeTimeUs += src.totalDecodeTimeUs;
    dst.totalPacerTimeUs += src.totalPacerTimeUs;
    dst.totalRenderTimeUs += src.totalRenderTimeUs;

    if (dst.minHostProcessingLatency == 0 ||
        (src.minHostProcessingLatency != 0 && src.minHostProcessingLatency < dst.minHostProcessingLatency)) {
        dst.minHostProcessingLatency = src.minHostProcessingLatency;
    }
    dst.maxHostProcessingLatency = qMax(dst.maxHostProcessingLatency, src.maxHostProcessingLatency);
    dst.totalHostProcessingLatency += src.totalHostProcessingLatency;
    dst.framesWithHostProcessingLatency += src.framesWithHostProcessingLatency;

    if (!LiGetEstimatedRttInfo(&dst.lastRtt, &dst.lastRttVariance)) {
        dst.lastRtt = dst.lastRttVariance = 0;
    }
    if (dst.measurementStartUs == 0 ||
        (src.measurementStartUs != 0 && src.measurementStartUs < dst.measurementStartUs)) {
        dst.measurementStartUs = src.measurementStartUs;
    }
    if (dst.measurementStartUs != 0 && now > dst.measurementStartUs) {
        const double seconds = double(now - dst.measurementStartUs) / 1000000.0;
        dst.totalFps = double(dst.totalFrames) / seconds;
        dst.receivedFps = double(dst.receivedFrames) / seconds;
        dst.decodedFps = double(dst.decodedFrames) / seconds;
        dst.renderedFps = double(dst.renderedFrames) / seconds;
    }
}

void PyroWaveVideoDecoder::updatePerformanceOverlay(const VIDEO_STATS& stats)
{
    auto& overlay = Session::get()->getOverlayManager();
    if (!overlay.isOverlayEnabled(Overlay::OverlayDebug) || stats.receivedFrames == 0) {
        return;
    }

    char text[2048];
    const char* chroma = m_Format & VIDEO_FORMAT_PYROWAVE_444 ? " 4:4:4" : " 4:2:0";
    const char* dynamicRange = m_Format & VIDEO_FORMAT_PYROWAVE_HDR ? " HDR" : " SDR";
    char rtt[64];
    if (stats.lastRtt != 0) {
        std::snprintf(rtt, sizeof(rtt), "%u ms (variance: %u ms)", stats.lastRtt, stats.lastRttVariance);
    }
    else {
        std::snprintf(rtt, sizeof(rtt), "N/A");
    }

    int used = std::snprintf(text, sizeof(text),
        "Video stream: %dx%d %.2f FPS (Codec: PyroWave%s%s)\n"
        "Incoming frame rate from network: %.2f FPS\n"
        "Decoding frame rate: %.2f FPS\n"
        "Rendering frame rate: %.2f FPS\n",
        m_Width, m_Height, stats.totalFps, dynamicRange, chroma,
        stats.receivedFps, stats.decodedFps, stats.renderedFps);
    if (used < 0 || used >= int(sizeof(text))) {
        return;
    }

    if (stats.framesWithHostProcessingLatency != 0) {
        const int written = std::snprintf(text + used, sizeof(text) - size_t(used),
            "Host processing latency min/max/average: %.1f/%.1f/%.1f ms\n",
            float(stats.minHostProcessingLatency) / 10.0f,
            float(stats.maxHostProcessingLatency) / 10.0f,
            float(stats.totalHostProcessingLatency) / 10.0f / stats.framesWithHostProcessingLatency);
        if (written < 0 || written >= int(sizeof(text)) - used) {
            return;
        }
        used += written;
    }

    if (m_GpuDequantMs > 0.0 || m_GpuIdwtMs > 0.0) {
        const int gpuWritten = std::snprintf(text + used, sizeof(text) - size_t(used),
            "PyroWave GPU dequant/iDWT: %.2f/%.2f ms\n",
            m_GpuDequantMs, m_GpuIdwtMs);
        if (gpuWritten < 0 || gpuWritten >= int(sizeof(text)) - used) {
            return;
        }
        used += gpuWritten;
    }

    if (stats.renderedFrames != 0) {
        const double networkDropPercent = stats.totalFrames != 0 ?
            double(stats.networkDroppedFrames) * 100.0 / stats.totalFrames : 0.0;
        const double clientDropPercent = stats.receivedFrames != 0 ?
            double(stats.pacerDroppedFrames) * 100.0 / stats.receivedFrames : 0.0;
        const int written = std::snprintf(text + used, sizeof(text) - size_t(used),
            "Frames dropped by your network connection: %.2f%%\n"
            "Frames dropped by the client frame queue: %.2f%%\n"
            "Average network latency: %s\n"
            "Average decode submission time: %.2f ms\n"
            "Average frame queue delay: %.2f ms\n"
            "Average presentation wait + render submission time: %.2f ms\n",
            networkDropPercent, clientDropPercent, rtt,
            double(stats.totalDecodeTimeUs) / 1000.0 / stats.decodedFrames,
            double(stats.totalPacerTimeUs) / 1000.0 / stats.renderedFrames,
            double(stats.totalRenderTimeUs) / 1000.0 / stats.renderedFrames);
        if (written < 0 || written >= int(sizeof(text)) - used) {
            return;
        }
        used += written;
    }

    if (m_VrrWorker) {
        used += Vrr::formatOverlay(m_VrrWorker->stats(), m_VrrOverlayLast, m_VrrWorker->config(),
                                   m_Renderer->getVrrPresentModeName(), text + used, int(sizeof(text)) - used);
    }
    else if (m_VrrRequested) {
        used += Vrr::formatFallback(m_VrrFallback, text + used, int(sizeof(text)) - used);
    }

    overlay.updateOverlayText(Overlay::OverlayDebug, text);
}

PyroWaveVideoDecoder::EncodedFrame* PyroWaveVideoDecoder::takeEncodedFrame()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_FreeEncodedFrames.empty()) {
        return new EncodedFrame;
    }
    EncodedFrame* encoded = m_FreeEncodedFrames.back();
    m_FreeEncodedFrames.pop_back();
    return encoded;
}

int PyroWaveVideoDecoder::submitDecodeUnit(PDECODE_UNIT du)
{
    if (du->fullLength < 8 || du->fullLength > int(PYROWAVE_MAX_FRAME_BYTES)) return DR_OK;
    // VRR presentation queues the encoded frame in a recycled buffer. Every
    // early return below recycles it through this guard.
    struct EncodedHold {
        PyroWaveVideoDecoder* decoder;
        EncodedFrame* encoded;
        std::vector<uint32_t>* frame;
        ~EncodedHold() {
            if (encoded) {
                encoded->data.swap(*frame);
                decoder->recycleEncodedFrame(encoded);
            }
        }
    };
    std::vector<uint32_t> frame;
    EncodedHold hold{this, m_VrrWorker ? takeEncodedFrame() : nullptr, &frame};
    if (hold.encoded) {
        frame.swap(hold.encoded->data);
    }
    else {
        std::lock_guard<std::mutex> lock(m_Mutex);
        frame.swap(m_Spare);
    }
    frame.resize((du->fullLength + 3) / 4);
    size_t offset = 0;
    std::vector<PyroWaveFraming::Segment> segments;
    const bool recordMode = m_Dialect == PYROWAVE_DIALECT_RECORD_FRAMED;
    for (auto entry = du->bufferList; entry; entry = entry->next) {
        if (entry->length <= 0 || size_t(entry->length) > size_t(du->fullLength) - offset) return DR_OK;
        if (recordMode) segments.push_back({offset, size_t(entry->length), entry->bufferType == BUFFER_TYPE_LOST, entry->bufferType == BUFFER_TYPE_RECORD_START});
        memcpy(reinterpret_cast<uint8_t*>(frame.data()) + offset, entry->data, entry->length);
        offset += entry->length;
    }
    if (offset != size_t(du->fullLength)) return DR_OK;
    size_t frameSize = offset;
    if (recordMode) {
        PyroWaveFraming::Frame parsed;
        std::string error;
        auto* data = reinterpret_cast<uint8_t*>(frame.data());
        if (!PyroWaveFraming::parse(data, frameSize, segments, du->pyrowaveCriticalPackets,
            {m_Width, m_Height, bool(m_Format & VIDEO_FORMAT_PYROWAVE_444)}, parsed, error) ||
            !parsed.coarseLevelIntact) return DR_OK;
        size_t write = 0;
        for (const auto& span : parsed.spans) {
            std::memmove(data + write, data + span.offset, span.size);
            write += span.size;
        }
        // Clear() zeros absent coefficients. Count only received records so
        // the authoritative decoder can finish an explicitly salvaged frame.
        if (write < 8 || parsed.blockRecords > 0xffffff) return DR_OK;
        uint32_t header = pyroWaveReadWord(data + 4);
        header = (header & 0xff000000) | parsed.blockRecords;
        for (unsigned i = 0; i < 4; ++i) data[4+i] = uint8_t(header >> (i*8));
        frameSize = write;
        if (!pyroWaveUnpackRecords(data, frameSize, m_Format & VIDEO_FORMAT_PYROWAVE_HDR)) return DR_OK;
    }
    const uint64_t now = LiGetMicroseconds();
    VIDEO_STATS overlayStats {};
    bool refreshOverlay = false;
    uint32_t lostPackets = 0;
    for (auto entry = du->bufferList; entry; entry = entry->next) {
        lostPackets += entry->bufferType == BUFFER_TYPE_LOST ? 1 : 0;
    }
    if (hold.encoded) {
        // Frames presented or dropped by the VRR worker since the last window.
        const Vrr::PacingWorker::Stats vrr = m_VrrWorker->stats();
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_ActiveVideoStats.renderedFrames += uint32_t(vrr.presented - m_VrrAccumulated.presented);
        m_ActiveVideoStats.pacerDroppedFrames += uint32_t((vrr.queueDrops - m_VrrAccumulated.queueDrops) +
                                                          (vrr.staleDrops - m_VrrAccumulated.staleDrops) +
                                                          (vrr.failedPreparations - m_VrrAccumulated.failedPreparations) +
                                                          (m_DecodeSkipped - m_DecodeSkippedAccumulated));
        m_DecodeSkippedAccumulated = m_DecodeSkipped;
        const uint64_t preparation = vrr.totalPreparationUs - m_VrrAccumulated.totalPreparationUs;
        const uint64_t queued = vrr.totalArrivalToPresentUs - m_VrrAccumulated.totalArrivalToPresentUs;
        m_ActiveVideoStats.totalRenderTimeUs += preparation + (vrr.totalPresentCallUs - m_VrrAccumulated.totalPresentCallUs);
        m_ActiveVideoStats.totalPacerTimeUs += queued > preparation ? queued - preparation : 0;
        m_VrrAccumulated = vrr;
    }
    // A periodic summary in the log, without per-frame logging.
    if (hold.encoded && !m_VrrLastLogUs) {
        m_VrrLastLogUs = now;
    }
    else if (hold.encoded && now - m_VrrLastLogUs >= 10000000) {
        char summary[1024];
        if (Vrr::formatSummary("VRR presentation (PyroWave, last 10 s)", m_VrrWorker->stats(), m_VrrLogLast,
                               summary, sizeof(summary))) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", summary);
        }
        logPreparationSplit("VRR decode and preparation (PyroWave, last 10 s)", m_VrrSplitLogged);
        m_VrrLastLogUs = now;
    }
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_ActiveVideoStats.measurementStartUs == 0) {
            m_ActiveVideoStats.measurementStartUs = now;
            m_LastFrameNumber = du->frameNumber;
        }
        else {
            if (du->frameNumber > m_LastFrameNumber + 1) {
                const uint32_t dropped = uint32_t(du->frameNumber - (m_LastFrameNumber + 1));
                m_ActiveVideoStats.networkDroppedFrames += dropped;
                m_ActiveVideoStats.totalFrames += dropped;
            }
            m_LastFrameNumber = du->frameNumber;
        }

        if (now > m_ActiveVideoStats.measurementStartUs + 1000000) {
            m_PendingOverlayStats = {};
            addVideoStats(m_LastVideoStats, m_PendingOverlayStats, now);
            addVideoStats(m_ActiveVideoStats, m_PendingOverlayStats, now);
            m_LastVideoStats = m_ActiveVideoStats;
            m_ActiveVideoStats = {};
            m_ActiveVideoStats.measurementStartUs = now;
            m_OverlayRefreshPending = true;
        }

        if (du->frameHostProcessingLatency != 0) {
            if (m_ActiveVideoStats.minHostProcessingLatency == 0) {
                m_ActiveVideoStats.minHostProcessingLatency = du->frameHostProcessingLatency;
            }
            else {
                m_ActiveVideoStats.minHostProcessingLatency =
                    qMin(m_ActiveVideoStats.minHostProcessingLatency, du->frameHostProcessingLatency);
            }
            m_ActiveVideoStats.maxHostProcessingLatency =
                qMax(m_ActiveVideoStats.maxHostProcessingLatency, du->frameHostProcessingLatency);
            m_ActiveVideoStats.totalHostProcessingLatency += du->frameHostProcessingLatency;
            m_ActiveVideoStats.framesWithHostProcessingLatency++;
        }
        m_ActiveVideoStats.receivedFrames++;
        m_ActiveVideoStats.totalFrames++;
        m_ActiveVideoStats.totalReassemblyTimeUs += du->enqueueTimeUs - du->receiveTimeUs;
        if (hold.encoded) {
            if (m_OverlayRefreshPending) {
                overlayStats = m_PendingOverlayStats;
                m_OverlayRefreshPending = false;
                refreshOverlay = true;
            }
        }
        else if (!m_Pending.empty()) {
            // The mailbox keeps the newest complete frame to minimize latency.
            m_ActiveVideoStats.pacerDroppedFrames++;
        }
    }
    if (hold.encoded) {
        if (refreshOverlay) updatePerformanceOverlay(overlayStats);
        Vrr::FrameTiming timing;
        timing.frameNumber = du->frameNumber;
        timing.rtpTimestamp = du->rtpTimestamp;
        timing.timestampValid = true;
        timing.receiveUs = du->receiveTimeUs;
        timing.lostPackets = lostPackets;
        EncodedFrame* encoded = hold.encoded;
        hold.encoded = nullptr;
        encoded->data.swap(frame);
        encoded->size = frameSize;
        encoded->enqueueUs = now;
        // The decode thread owns the frame now. A frame it has not started is
        // replaced: decoding it would only delay the newer one.
        EncodedFrame* replaced = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            replaced = m_DecodePending;
            m_DecodePending = encoded;
            m_DecodePendingTiming = timing;
            m_DecodeSkipped += replaced ? 1 : 0;
        }
        m_DecodeWake.notify_one();
        if (replaced) recycleEncodedFrame(replaced);
        return DR_OK;
    }
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (!m_Pending.empty()) {
            m_Pending.swap(frame);
            if (frame.capacity() > m_Spare.capacity()) m_Spare.swap(frame);
        }
        else {
            m_Pending = std::move(frame);
        }
        m_PendingSize = frameSize;
        m_EnqueueTime = now;
        if (!m_Threaded && !m_EventQueued) {
            SDL_Event event {};
            event.type = SDL_USEREVENT;
            event.user.code = SDL_CODE_FRAME_READY;
            event.user.windowID = SDL_GetWindowID(m_Window);
            m_EventQueued = SDL_PushEvent(&event) == 1;
        }
    }
    if (m_Threaded) m_FrameReady.notify_one();
    return DR_OK;
}

void PyroWaveVideoDecoder::releasePlanes(PlaneSet& set, uint64_t value)
{
    auto vk = m_Renderer->getVulkan();
    for (auto plane : set.planes) {
        pl_vulkan_release_params params {};
        params.tex = plane;
        params.layout = VK_IMAGE_LAYOUT_GENERAL;
        params.qf = VK_QUEUE_FAMILY_IGNORED;
        params.semaphore = {m_Timeline, value};
        pl_vulkan_release_ex(vk->gpu, &params);
    }
}

bool PyroWaveVideoDecoder::decodeToPlanes(const std::vector<uint32_t>& bytes, size_t size, PlaneSet& set,
                                          pl_frame& frame, uint64_t* decodeTimeUs)
{

    if (size < 8 || size > PYROWAVE_MAX_FRAME_BYTES || size > bytes.size() * sizeof(uint32_t)) return false;
    // Sequence header metadata is used directly; no assumptions about bit depth.
    const uint32_t b = qFromLittleEndian(bytes[1]);
    if (!pyroWaveValidateFrame(reinterpret_cast<const uint8_t*>(bytes.data()), size, m_Width, m_Height,
                              m_Format & VIDEO_FORMAT_PYROWAVE_444,
                              m_Format & VIDEO_FORMAT_PYROWAVE_HDR, m_BlockSeen)) return false;
    pyrowave_decoder_clear(m_Decoder);
    if (pyrowave_decoder_push_packet(m_Decoder, bytes.data(), size) != PYROWAVE_SUCCESS ||
        !pyrowave_decoder_decode_is_ready(m_Decoder, false)) return false;
    auto vk = m_Renderer->getVulkan();
    for (int p = 0; p < 3; p++) {
        pl_vulkan_hold_params params {};
        params.tex = set.planes[p];
        params.layout = VK_IMAGE_LAYOUT_GENERAL;
        params.qf = VK_QUEUE_FAMILY_IGNORED;
        params.semaphore = {m_Timeline, ++m_Value};
        if (!pl_vulkan_hold_ex(vk->gpu, &params)) {
            // Return every successfully held plane; device reset handles failure.
            for (int i = 0; i < p; i++) {
                pl_vulkan_release_params release {};
                release.tex = set.planes[i]; release.layout = VK_IMAGE_LAYOUT_GENERAL;
                release.qf = VK_QUEUE_FAMILY_IGNORED; release.semaphore = {m_Timeline, m_Value - 1};
                pl_vulkan_release_ex(vk->gpu, &release);
            }
            return false;
        }
    }
    pyrowave_gpu_sync_operation acquire {}, release {};
    acquire.sync = {m_Timeline, m_Value};
    release.sync = {m_Timeline, ++m_Value};
    const auto decodeStart = LiGetMicroseconds();
    auto result = pyrowave_decoder_decode_gpu_buffer(m_Decoder, &acquire, &release, &set.buffers);
    if (result != PYROWAVE_SUCCESS) {
        // No valid signal will follow a failed decode. Finish before dropping ownership.
        pl_gpu_finish(vk->gpu);
        releasePlanes(set, 0);
        return false;
    }
    const auto decodeEnd = LiGetMicroseconds();
    releasePlanes(set, m_Value);
    frame = {};
    frame.num_planes = 3;
    frame.crop = {0, 0, float(m_Width), float(m_Height)};
    frame.repr = pyroWaveColorRepresentation(b & (1u << 28), b & (1u << 30), b & (1u << 29));
    frame.color.primaries = b & (1u << 27) ? PL_COLOR_PRIM_BT_2020 : PL_COLOR_PRIM_BT_709;
    frame.color.transfer = pyroWaveTransferFunction(b & (1u << 28));
    if ((m_Format & VIDEO_FORMAT_PYROWAVE_HDR) && m_OverlayAttached) {
        SS_HDR_METADATA metadata {};
        if (LiGetHdrMetadata(&metadata)) {
            if (metadata.maxDisplayLuminance) {
                frame.color.hdr.max_luma = metadata.maxDisplayLuminance;
                frame.color.hdr.min_luma = metadata.minDisplayLuminance ? metadata.minDisplayLuminance / 10000.0f : PL_COLOR_HDR_BLACK;
            }
            frame.color.hdr.max_cll = metadata.maxContentLightLevel;
            frame.color.hdr.max_fall = metadata.maxFrameAverageLightLevel;
            if (metadata.displayPrimaries[0].x) {
                frame.color.hdr.prim.red = {metadata.displayPrimaries[0].x / 50000.0f, metadata.displayPrimaries[0].y / 50000.0f};
                frame.color.hdr.prim.green = {metadata.displayPrimaries[1].x / 50000.0f, metadata.displayPrimaries[1].y / 50000.0f};
                frame.color.hdr.prim.blue = {metadata.displayPrimaries[2].x / 50000.0f, metadata.displayPrimaries[2].y / 50000.0f};
                frame.color.hdr.prim.white = {metadata.whitePoint.x / 50000.0f, metadata.whitePoint.y / 50000.0f};
            }
        }
    }
    for (int p = 0; p < 3; p++) {
        frame.planes[p].texture = set.planes[p];
        frame.planes[p].components = 1;
        frame.planes[p].component_mapping[0] = p;
    }
    pl_frame_set_chroma_location(&frame, b >> 31 ? PL_CHROMA_LEFT : PL_CHROMA_CENTER);
    if (decodeTimeUs) *decodeTimeUs = decodeEnd - decodeStart;
    reportGpuStats(decodeStart);
    return true;
}

void PyroWaveVideoDecoder::reportGpuStats(uint64_t nowUs)
{
    if (nowUs - m_LastStatsTime >= 1000000) {
        pyrowave_device_report_performance_stats(m_Device, collectPerformanceStat, this, true);
        m_LastStatsTime = nowUs;
        if ((m_GpuDequantMs > 0.0 || m_GpuIdwtMs > 0.0) &&
            nowUs - m_LastGpuStatsLogTime >= 5000000) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PyroWave GPU decode (%s path): dequant %.2f ms, iDWT %.2f ms",
                        m_FragmentPath ? "fragment" : "compute",
                        m_GpuDequantMs, m_GpuIdwtMs);
            m_LastGpuStatsLogTime = nowUs;
        }
    }
}

bool PyroWaveVideoDecoder::decodeFrame(const std::vector<uint32_t>& bytes, size_t size,
                                       uint64_t* decodeTimeUs, uint64_t* renderTimeUs,
                                       bool rendererReady)
{
    pl_frame frame;
    if (!decodeToPlanes(bytes, size, m_Planes, frame, decodeTimeUs)) return false;
    const auto renderStart = LiGetMicroseconds();
    if (!rendererReady) m_Renderer->waitToRender();
    m_Renderer->renderPlaceboFrame(frame);
    const auto renderEnd = LiGetMicroseconds();
    if (renderTimeUs) *renderTimeUs = renderEnd - renderStart;
    return true;
}

PyroWaveVideoDecoder::PlaneSet* PyroWaveVideoDecoder::takePlaneSet()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_FreeSets.empty()) {
            PlaneSet* set = m_FreeSets.back();
            m_FreeSets.pop_back();
            return set;
        }
        // The worker owns at most Vrr::kOwnedFrames sets; one more decodes.
        if (m_VrrSets.size() + 1 >= size_t(Vrr::kOwnedFrames + 1)) return nullptr;
    }
    // Only the decode thread grows the pool.
    auto set = std::make_unique<PlaneSet>();
    if (!createPlaneTextures(*set)) {
        auto vk = m_Renderer->getVulkan();
        for (auto& plane : set->planes) pl_tex_destroy(vk->gpu, &plane);
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_VrrSets.push_back(std::move(set));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PyroWave VRR: %zu decode plane sets in use", m_VrrSets.size() + 1);
    return m_VrrSets.back().get();
}

void PyroWaveVideoDecoder::vrrDecodeLoop()
{
    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unable to raise PyroWave decode thread priority: %s", SDL_GetError());
    }
    for (;;) {
        EncodedFrame* encoded = nullptr;
        Vrr::FrameTiming timing;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_DecodeWake.wait(lock, [this] { return m_DecodeStopping || m_DecodePending; });
            if (m_DecodeStopping) return;
            encoded = m_DecodePending;
            m_DecodePending = nullptr;
            timing = m_DecodePendingTiming;
        }
        PlaneSet* set = takePlaneSet();
        if (!set) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No PyroWave plane set available; frame skipped");
            recycleEncodedFrame(encoded);
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_DecodeSkipped++;
            continue;
        }
        uint64_t decodeTimeUs = 0;
        const uint64_t decodeStart = LiGetMicroseconds();
        const bool decoded = decodeToPlanes(encoded->data, encoded->size, *set, set->frame, &decodeTimeUs);
        const uint64_t decodeEnd = LiGetMicroseconds();
        recycleEncodedFrame(encoded);
        if (!decoded) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Rejected incomplete/invalid PyroWave frame or GPU operation failed");
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_FreeSets.push_back(set);
                m_DecodeSkipped++;
            }
            if (pl_gpu_is_failed(m_Renderer->getVulkan()->gpu)) {
                SDL_Event event {}; event.type = SDL_RENDER_DEVICE_RESET; SDL_PushEvent(&event);
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_ActiveVideoStats.decodedFrames++;
            m_ActiveVideoStats.totalDecodeTimeUs += decodeTimeUs;
            m_VrrSplit.decodeSubmitUs += decodeEnd - decodeStart;
            m_VrrSplit.decodedFrames++;
        }
        // Like a hardware decoder's output, the frame is ready for the worker
        // once its decode is submitted; the GPU decode itself completes within
        // preparation, which waits for the render that consumes it.
        timing.readyUs = decodeEnd;
        // The worker owns the set now and releases it when done.
        m_VrrWorker->submit(timing, set);
    }
}

Vrr::PrepareResult PyroWaveVideoDecoder::vrrPrepare(void* payload, bool)
{
    auto* set = static_cast<PlaneSet*>(payload);
    // When the renderer must render the planes at present time instead, the
    // set stays owned by the worker until then.
    bool retainsPlanes = false;
    const bool prepared = m_Renderer->vrrPrepareMappedFrame(set->frame, retainsPlanes);
    if (prepared && !retainsPlanes) {
        const PlVkRenderer::VrrPrepareTiming& timing = m_Renderer->vrrLastPrepareTiming();
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_VrrSplit.frames++;
        m_VrrSplit.renderSubmitUs += timing.renderSubmitUs;
        m_VrrSplit.gpuWaitUs += timing.gpuWaitUs;
    }
    if (!prepared) {
        return {};
    }
    return {true, !retainsPlanes};
}

void PyroWaveVideoDecoder::logPreparationSplit(const char* title, PreparationSplit& since)
{
    PreparationSplit now;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        now = m_VrrSplit;
    }
    const uint64_t decoded = now.decodedFrames - since.decodedFrames;
    const uint64_t frames = now.frames - since.frames;
    if (decoded && frames) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "%s: decode thread %.2f ms (%llu frames); preparation: render submit %.2f ms, "
                    "GPU wait %.2f ms (%llu frames)",
                    title, double(now.decodeSubmitUs - since.decodeSubmitUs) / double(decoded) / 1000.0,
                    (unsigned long long)decoded,
                    double(now.renderSubmitUs - since.renderSubmitUs) / double(frames) / 1000.0,
                    double(now.gpuWaitUs - since.gpuWaitUs) / double(frames) / 1000.0,
                    (unsigned long long)frames);
    }
    since = now;
}

Vrr::PresentResult PyroWaveVideoDecoder::vrrPresent(bool)
{
    return {m_Renderer->vrrPresentPrepared()};
}

void PyroWaveVideoDecoder::vrrCancel()
{
    m_Renderer->vrrDiscardPrepared();
}

void PyroWaveVideoDecoder::vrrRelease(void* payload)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_FreeSets.push_back(static_cast<PlaneSet*>(payload));
}

void PyroWaveVideoDecoder::recycleEncodedFrame(EncodedFrame* encoded)
{
    encoded->size = 0;
    std::lock_guard<std::mutex> lock(m_Mutex);
    // The receive thread and the decode thread hold at most one frame each,
    // plus the mailbox: a few buffers are enough.
    if (m_FreeEncodedFrames.size() < 4) {
        m_FreeEncodedFrames.push_back(encoded);
    }
    else {
        delete encoded;
    }
}

void PyroWaveVideoDecoder::vrrThreadStopping()
{
    m_Renderer->vrrDiscardPrepared();
}

void PyroWaveVideoDecoder::recycleFrame(std::vector<uint32_t>& frame)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    frame.clear();
    if (frame.capacity() > m_Spare.capacity()) m_Spare.swap(frame);
}

void PyroWaveVideoDecoder::renderPendingFrame(bool rendererReady)
{
    std::vector<uint32_t> frame;
    VIDEO_STATS overlayStats {};
    bool refreshOverlay = false;
    size_t size;
    uint64_t enqueueTime;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        frame = std::move(m_Pending);
        size = m_PendingSize;
        enqueueTime = m_EnqueueTime;
        m_EventQueued = false;
    }
    if (frame.empty()) return;
    const uint64_t processingStart = LiGetMicroseconds();
    uint64_t decodeTimeUs = 0;
    uint64_t renderTimeUs = 0;
    if (!decodeFrame(frame, size, &decodeTimeUs, &renderTimeUs, rendererReady)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Rejected incomplete/invalid PyroWave frame or GPU operation failed");
        // The synchronous probe path may already own a swapchain frame.
        if (rendererReady) m_Renderer->cleanupRenderContext();
        if (pl_gpu_is_failed(m_Renderer->getVulkan()->gpu)) {
            SDL_Event event {}; event.type = SDL_RENDER_DEVICE_RESET; SDL_PushEvent(&event);
        }
    }
    else {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_ActiveVideoStats.decodedFrames++;
        m_ActiveVideoStats.renderedFrames++;
        m_ActiveVideoStats.totalDecodeTimeUs += decodeTimeUs;
        m_ActiveVideoStats.totalPacerTimeUs += processingStart - enqueueTime;
        m_ActiveVideoStats.totalRenderTimeUs += renderTimeUs;
        if (m_OverlayRefreshPending) {
            overlayStats = m_PendingOverlayStats;
            m_OverlayRefreshPending = false;
            refreshOverlay = true;
        }
    }
    recycleFrame(frame);
    if (refreshOverlay) updatePerformanceOverlay(overlayStats);
}

void PyroWaveVideoDecoder::renderLoop()
{
    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unable to set PyroWave render thread priority: %s", SDL_GetError());
    }
    while (true) {
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_FrameReady.wait(lock, [this] { return m_Stopping || !m_Pending.empty(); });
            if (m_Stopping) break;
        }

        // Submit decode before waiting for presentation capacity. The timeline
        // dependency still orders this decode after the preceding render, but
        // the GPU can execute it while this thread waits for the next swapchain
        // frame. Waiting first creates a decode -> present -> decode bubble that
        // is especially costly at 120 Hz and above.
        renderPendingFrame(false);
    }
    m_Renderer->cleanupRenderContext();
}

void PyroWaveVideoDecoder::renderFrameOnMainThread()
{
    // Production uses the dedicated Vulkan render thread. The synchronous path
    // remains for renderer probing and the GPU smoke test.
    if (m_Threaded) return;
    m_Renderer->waitToRender();
    renderPendingFrame(true);
}

bool PyroWaveVideoDecoder::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info)
{
    return m_Renderer->notifyWindowChanged(info);
}
