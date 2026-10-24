#pragma once

#include "renderer.h"
#include "pacer/vrrpacingworker.h"

#ifdef Q_OS_WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#ifdef Q_OS_DARWIN
class MetalVulkanTextureFactory {
public:
    MetalVulkanTextureFactory(pl_vulkan vulkan);
    ~MetalVulkanTextureFactory();

    bool mapVideoToolboxToPlacebo(const AVFrame *frame, pl_frame* mappedFrame);
    void unmapVideoToolboxFromPlacebo(pl_frame* mappedFrame);

private:
    pl_vulkan m_Vulkan;
    /* CVMetalTextureCacheRef */ void* m_TextureCache = nullptr;
};

// Work around direct-to-display mode sometimes (but not always!) blocking us
// from getting a new drawable while the current one is getting scanned out.
#define PLVK_USE_DYNAMIC_SWAPCHAIN_DEPTH 1

// MoltenVK will block for the next drawable when we render the next frame
// rather than inside pl_swapchain_start_frame(), so we will force it to wait
// by rendering some no-op work right after we get the new swapchain frame.
#define PLVK_USE_EARLY_RENDER_TO_WAIT 1

#endif

class PlVkRenderer : public IFFmpegRenderer, public Vrr::IFramePresenter {
public:
    PlVkRenderer(AVHWDeviceType hwDeviceType = AV_HWDEVICE_TYPE_NONE, IFFmpegRenderer *backendRenderer = nullptr);
    virtual ~PlVkRenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual bool testRenderFrame(AVFrame* frame) override;
    virtual void waitToRender() override;
    virtual void cleanupRenderContext() override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType) override;
    virtual void notifyKeyboardOverlayUpdated() override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO) override;
    virtual int getRendererAttributes() override;
    virtual int getDecoderColorspace() override;
    virtual int getDecoderColorRange() override;
    virtual int getDecoderCapabilities() override;
    virtual bool isPixelFormatSupported(int videoFormat, enum AVPixelFormat pixelFormat) override;
    virtual AVPixelFormat getPreferredPixelFormat(int videoFormat) override;

    // GPU codec integration: the renderer owns the Vulkan device and textures.
    pl_vulkan getVulkan() const { return m_Vulkan; }
    pl_vk_inst getVulkanInstance() const { return m_PlVkInstance; }
    void renderPlaceboFrame(pl_frame& mappedFrame);

    // VRR presentation (Linux). The swapchain uses an adaptive present mode
    // chosen at initialization: Mailbox (synchronized, never tears) on
    // Wayland, Immediate elsewhere. Preparation renders into a renderer-owned
    // texture and waits for its GPU completion, so the source can be released
    // and the swapchain image is only acquired, blitted and presented at the
    // target: holding an acquired image across the wait lets the next frame's
    // GPU work delay its flip.
    Vrr::IFramePresenter* getVrrPresenter(Vrr::PresentProtection* protection,
                                          Vrr::FallbackReason* reason) override;
    const char* getVrrPresentModeName() override;
    bool restoreSynchronizedPresentation() override;
    Vrr::PrepareResult vrrPrepare(void* payload, bool latched) override;
    Vrr::PresentResult vrrPresent(bool latched) override;
    void vrrCancel() override;
    void vrrRelease(void* payload) override;
    void vrrThreadStopping() override;
    // Shared with the PyroWave decoder: prepare a mapped frame. Returns false
    // on failure. When `retainsSource` is set on return, the frame could not
    // be pre-rendered (no swapchain colorimetry yet) and is rendered directly
    // at present time, so its source must stay valid until then.
    bool vrrPrepareMappedFrame(pl_frame& mappedFrame, bool& retainsSource);
    bool vrrPresentPrepared();
    void vrrDiscardPrepared();
    // How the last vrrPrepareMappedFrame() spent its time: recording and
    // submitting the render, then waiting for the GPU to complete it.
    struct VrrPrepareTiming {
        uint64_t renderSubmitUs = 0;
        uint64_t gpuWaitUs = 0;
    };
    const VrrPrepareTiming& vrrLastPrepareTiming() const { return m_VrrPrepareTiming; }

private:
    static void lockQueue(AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index);
    static void unlockQueue(AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index);
    static void overlayUploadComplete(void* opaque);

    void beginRenderTiming();
    void endRenderTiming();
    // Render a mapped frame and overlays into `target`, scaled with aspect ratio.
    void drawFrame(pl_frame& mappedFrame, pl_frame& targetFrame);
    bool chooseVrrPresentMode();
    bool applyColorspaceHint(const pl_color_space& color);

    bool createSwapchain(int depth);
    bool createOverlay(pl_overlay* overlay, SDL_Surface* surface);
    bool mapAvFrameToPlacebo(const AVFrame *frame, pl_frame* mappedFrame);
    void unmapAvFrameFromPlacebo(const AVFrame *frame, pl_frame* mappedFrame);
    bool populateQueues(int videoFormat);
    bool chooseVulkanDevice(PDECODER_PARAMETERS params, bool hdrOutputRequired);
    bool tryInitializeDevice(VkPhysicalDevice device, VkPhysicalDeviceProperties* deviceProps,
                             PDECODER_PARAMETERS decoderParams, bool hdrOutputRequired);
    bool isExtensionSupportedByPhysicalDevice(VkPhysicalDevice device, const char* extensionName);
    bool isPresentModeSupportedByPhysicalDevice(VkPhysicalDevice device, VkPresentModeKHR presentMode);
    bool isColorSpaceSupportedByPhysicalDevice(VkPhysicalDevice device, VkColorSpaceKHR colorSpace);
    bool isSurfacePresentationSupportedByPhysicalDevice(VkPhysicalDevice device);

    // The backend renderer if we're frontend-only
    IFFmpegRenderer* m_Backend;
    AVHWDeviceType m_HwDeviceType;

#ifdef Q_OS_DARWIN
    std::unique_ptr<MetalVulkanTextureFactory> m_MetalTextureFactory;
#endif

#ifdef PLVK_USE_DYNAMIC_SWAPCHAIN_DEPTH
    int m_DelayedPresents = 0;
    Uint32 m_RenderStartTime = 0;
#endif

    // SDL state
    SDL_Window* m_Window = nullptr;

    // Stream state
    int m_MaxVideoFps;

    // The libplacebo rendering state
    pl_log m_Log = nullptr;
    pl_vk_inst m_PlVkInstance = nullptr;
    VkSurfaceKHR m_VkSurface = VK_NULL_HANDLE;
    // Decoder probe under Gamescope's WSI layer: no window surface or swapchain
    bool m_SurfacelessProbe = false;
    int m_SwapchainDepth = 0;
    VkPresentModeKHR m_VkPresentMode;
    pl_vulkan m_Vulkan = nullptr;
    pl_swapchain m_Swapchain = nullptr;
    pl_renderer m_Renderer = nullptr;
    pl_tex m_Textures[PL_MAX_PLANES] = {};
    pl_color_space m_LastColorspace = {};

#ifdef PLVK_USE_EARLY_RENDER_TO_WAIT
    pl_overlay m_EmptyOverlay = {};
    pl_overlay_part m_EmptyOverlayPart = {};
#endif

    // Pending swapchain state shared between waitToRender(), renderFrame(), and cleanupRenderContext()
    pl_swapchain_frame m_SwapchainFrame = {};
    bool m_HasPendingSwapchainFrame = false;

    // VRR presentation state, used only by the pacing worker thread.
    bool m_VrrEnabled = false;
    Vrr::PresentProtection m_VrrProtection = Vrr::PresentProtection::SoftwareFloor;
    Vrr::FallbackReason m_VrrFallback = Vrr::FallbackReason::NotRequested;
    // The renderer-owned image prepared for the next present.
    pl_tex m_VrrTarget = nullptr;
    bool m_VrrPrepared = false;
    // Colorimetry and size of the last swapchain image, used to pre-render
    // into m_VrrTarget. Invalid until the first present or after a
    // colorspace change, when frames are rendered at present time instead.
    bool m_VrrSwapchainKnown = false;
    pl_color_space m_VrrColor = {};
    pl_color_repr m_VrrRepr = {};
    pl_fmt m_VrrFormat = nullptr;
    int m_VrrWidth = 0;
    int m_VrrHeight = 0;
    // A source retained for rendering at present time.
    bool m_VrrDirect = false;
    pl_frame m_VrrDirectFrame = {};
    AVFrame* m_VrrDirectAvFrame = nullptr;
    VrrPrepareTiming m_VrrPrepareTiming;

    // Overlay state
    SDL_SpinLock m_OverlayLock = 0;
    struct {
        // The staging overlay state is copied here under the overlay lock in the render thread.
        //
        // These values can be safely read by the render thread outside of the overlay lock,
        // but the copy from stagingOverlay to overlay must only happen under the overlay
        // lock when hasStagingOverlay is true.
        bool hasOverlay;
        pl_overlay overlay;

        // This state is written by the overlay update thread
        //
        // NB: hasStagingOverlay may be false even if there is a staging overlay texture present,
        // because this is how the overlay update path indicates that the overlay is not currently
        // safe for the render thread to read.
        //
        // It is safe for the overlay update thread to write to stagingOverlay outside of the lock,
        // as long as hasStagingOverlay is false.
        bool hasStagingOverlay;
        pl_overlay stagingOverlay;
    } m_Overlays[Overlay::OverlayMax + 1] = {};

    // The on-screen PC keyboard uses the slot after the text overlays
    static constexpr int k_KeyboardOverlaySlot = Overlay::OverlayMax;

    void stageOverlaySurface(int slot, SDL_Surface* newSurface, bool enabled);

    // Device context used for hwaccel decoders
    AVBufferRef* m_HwDeviceCtx = nullptr;

    // Vulkan functions we call directly
    PFN_vkDestroySurfaceKHR fn_vkDestroySurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 fn_vkGetPhysicalDeviceQueueFamilyProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR fn_vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR fn_vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkEnumeratePhysicalDevices fn_vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties fn_vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR fn_vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties fn_vkEnumerateDeviceExtensionProperties = nullptr;
};
