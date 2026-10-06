/*
 * VK_LAYER_DROIDDECK_present_wait - a present waits for its frame to be rendered.
 *
 * On the KGSL kernels these phones run (5.15), a dma-buf carries no fences: the kernel has no
 * DMA_BUF_IOCTL_EXPORT/IMPORT_SYNC_FILE and KGSL attaches nothing to the buffer's reservation, so
 * implicit sync does not exist, and there is no DRM syncobj for explicit sync either. A Vulkan
 * swapchain on Wayland (gamescope's, the app's compositor's) then hands its buffer over the moment
 * vkQueuePresentKHR is called, while the GPU may still be drawing into it; the compositor copies
 * or shows it as it stands - an unfinished frame, or the image's previous contents from several
 * frames ago. On screen that is flicker, not stutter, and frame generation interpolates from it.
 *
 * This layer moves the present's semaphore waits into an empty submit with a fence, waits for that
 * fence on the CPU, and then presents with nothing left to wait for: the buffer leaves the program
 * finished. It costs the present thread the rest of the frame's GPU time, which a compositor that
 * cannot wait itself had to pay anyway. DROIDDECK_PRESENT_WAIT=0 turns it off (see the manifest).
 *
 * Built freestanding (no libc headers); memset/memcpy come from the process's libc at load time.
 */
#include <vulkan/vulkan_core.h>

#define EXPORT __attribute__((visibility("default")))

/* ---- the loader's layer chain (vk_layer.h), declared here to stay freestanding ---- */
typedef enum { LAYER_LINK_INFO = 0 } layer_function;
typedef struct layer_instance_link {
    struct layer_instance_link *pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    void *pfnNextGetPhysicalDeviceProcAddr;
} layer_instance_link;
typedef struct {
    VkStructureType sType; /* VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO */
    const void *pNext;
    layer_function function;
    union { layer_instance_link *pLayerInfo; void *other; } u;
} layer_instance_create_info;
typedef struct layer_device_link {
    struct layer_device_link *pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
} layer_device_link;
typedef struct {
    VkStructureType sType; /* VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO */
    const void *pNext;
    layer_function function;
    union { layer_device_link *pLayerInfo; void *other; } u;
} layer_device_create_info;

static void *key_of(const void *dispatchable) { return *(void *const *)dispatchable; }

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---- a small lock and fixed tables: a process has a handful of instances, devices and queues ---- */
static int g_lock;
static void lock(void) { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

#define MAX_INSTANCES 16
#define MAX_DEVICES 16
#define MAX_QUEUES 64

static struct instance_entry {
    void *key;
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy;
} g_instances[MAX_INSTANCES];

static struct device_entry {
    void *key;
    VkDevice device;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice destroy;
    PFN_vkQueuePresentKHR present;
    PFN_vkQueueSubmit submit;
    PFN_vkCreateFence create_fence;
    PFN_vkWaitForFences wait_fences;
    PFN_vkResetFences reset_fences;
    PFN_vkDestroyFence destroy_fence;
} g_devices[MAX_DEVICES];

static struct queue_entry {
    VkQueue queue;
    void *device_key;
    VkFence fence;
} g_queues[MAX_QUEUES];

static struct instance_entry *instance_for(void *key) {
    for (int i = 0; i < MAX_INSTANCES; i++) if (g_instances[i].key == key) return &g_instances[i];
    return 0;
}
static struct device_entry *device_for(void *key) {
    for (int i = 0; i < MAX_DEVICES; i++) if (g_devices[i].key == key) return &g_devices[i];
    return 0;
}

/* ---- instance ---- */
static VKAPI_ATTR VkResult VKAPI_CALL pw_CreateInstance(const VkInstanceCreateInfo *ci,
                                                        const VkAllocationCallbacks *alloc, VkInstance *out) {
    layer_instance_create_info *link = (layer_instance_create_info *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == LAYER_LINK_INFO))
        link = (layer_instance_create_info *)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(ci, alloc, out);
    if (r != VK_SUCCESS) return r;
    lock();
    struct instance_entry *e = instance_for(0);
    if (e) {
        e->key = key_of(*out);
        e->instance = *out;
        e->gipa = gipa;
        e->destroy = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
    }
    unlock();
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL pw_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc) {
    if (!instance) return;
    lock();
    struct instance_entry *e = instance_for(key_of(instance));
    PFN_vkDestroyInstance destroy = e ? e->destroy : 0;
    if (e) e->key = 0;
    unlock();
    if (destroy) destroy(instance, alloc);
}

/* ---- device ---- */
static VKAPI_ATTR VkResult VKAPI_CALL pw_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                                                      const VkAllocationCallbacks *alloc, VkDevice *out) {
    layer_device_create_info *link = (layer_device_create_info *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == LAYER_LINK_INFO))
        link = (layer_device_create_info *)link->pNext;
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    /* The physical device's dispatch key is its instance's: the next layer's vkCreateDevice is
     * asked for on that instance. */
    lock();
    struct instance_entry *ie = instance_for(key_of(phys));
    VkInstance instance = ie ? ie->instance : VK_NULL_HANDLE;
    unlock();
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(phys, ci, alloc, out);
    if (r != VK_SUCCESS) return r;
    VkDevice d = *out;
    lock();
    struct device_entry *e = device_for(0);
    if (e) {
        e->key = key_of(d);
        e->device = d;
        e->gdpa = gdpa;
        e->destroy = (PFN_vkDestroyDevice)gdpa(d, "vkDestroyDevice");
        e->present = (PFN_vkQueuePresentKHR)gdpa(d, "vkQueuePresentKHR");
        e->submit = (PFN_vkQueueSubmit)gdpa(d, "vkQueueSubmit");
        e->create_fence = (PFN_vkCreateFence)gdpa(d, "vkCreateFence");
        e->wait_fences = (PFN_vkWaitForFences)gdpa(d, "vkWaitForFences");
        e->reset_fences = (PFN_vkResetFences)gdpa(d, "vkResetFences");
        e->destroy_fence = (PFN_vkDestroyFence)gdpa(d, "vkDestroyFence");
    }
    unlock();
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL pw_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc) {
    if (!device) return;
    void *key = key_of(device);
    lock();
    struct device_entry *e = device_for(key);
    struct device_entry copy = {0};
    if (e) { copy = *e; e->key = 0; }
    VkFence fences[MAX_QUEUES];
    int n = 0;
    for (int i = 0; i < MAX_QUEUES; i++) {
        if (g_queues[i].queue && g_queues[i].device_key == key) {
            if (g_queues[i].fence) fences[n++] = g_queues[i].fence;
            g_queues[i].queue = 0;
            g_queues[i].fence = 0;
            g_queues[i].device_key = 0;
        }
    }
    unlock();
    if (!e) return;
    for (int i = 0; i < n; i++) copy.destroy_fence(device, fences[i], 0);
    if (copy.destroy) copy.destroy(device, alloc);
}

/* The fence this queue's presents wait on, made on first use. A queue's presents are externally
 * synchronised by the program, so one fence per queue is never waited on twice at once. */
static VkFence fence_for(VkQueue queue, struct device_entry *dev) {
    lock();
    struct queue_entry *free_slot = 0;
    for (int i = 0; i < MAX_QUEUES; i++) {
        if (g_queues[i].queue == queue) { VkFence f = g_queues[i].fence; unlock(); return f; }
        if (!g_queues[i].queue && !free_slot) free_slot = &g_queues[i];
    }
    unlock();
    if (!free_slot) return VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f = VK_NULL_HANDLE;
    if (dev->create_fence(dev->device, &fci, 0, &f) != VK_SUCCESS) return VK_NULL_HANDLE;
    lock();
    if (!free_slot->queue) {
        free_slot->queue = queue;
        free_slot->device_key = dev->key;
        free_slot->fence = f;
        unlock();
        return f;
    }
    unlock();
    dev->destroy_fence(dev->device, f, 0);
    return VK_NULL_HANDLE;
}

#define MAX_WAITS 16

static VKAPI_ATTR VkResult VKAPI_CALL pw_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
    lock();
    struct device_entry *e = device_for(key_of(queue));
    struct device_entry dev = {0};
    if (e) dev = *e;
    unlock();
    if (!e) return VK_ERROR_DEVICE_LOST;
    VkFence fence = info->waitSemaphoreCount <= MAX_WAITS ? fence_for(queue, &dev) : VK_NULL_HANDLE;
    if (fence == VK_NULL_HANDLE) return dev.present(queue, info);

    /* The present's waits become this submit's: once its fence signals, every semaphore the
     * frame was waiting on has been signalled and everything submitted before on this queue is
     * done - the frame is rendered. */
    VkPipelineStageFlags stages[MAX_WAITS];
    for (uint32_t i = 0; i < info->waitSemaphoreCount; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = info->waitSemaphoreCount,
        .pWaitSemaphores = info->pWaitSemaphores,
        .pWaitDstStageMask = stages,
    };
    VkResult r = dev.submit(queue, 1, &si, fence);
    if (r != VK_SUCCESS) return dev.present(queue, info);
    /* Bounded: a hung frame is shown as it stands rather than hanging the program. */
    r = dev.wait_fences(dev.device, 1, &fence, VK_TRUE, 1000000000ull);
    if (r == VK_ERROR_DEVICE_LOST) return r;
    dev.reset_fences(dev.device, 1, &fence);
    if (r != VK_SUCCESS) {
        /* Timed out: the fence may still signal later; a fresh one replaces it for the next frame. */
        lock();
        for (int i = 0; i < MAX_QUEUES; i++) if (g_queues[i].queue == queue) g_queues[i].queue = 0;
        unlock();
    }
    VkPresentInfoKHR p = *info;
    p.waitSemaphoreCount = 0;
    p.pWaitSemaphores = 0;
    return dev.present(queue, &p);
}

/* ---- entry points ---- */
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL pw_GetDeviceProcAddr(VkDevice device, const char *name);

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    if (str_eq(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (str_eq(name, "vkCreateInstance")) return (PFN_vkVoidFunction)pw_CreateInstance;
    if (str_eq(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)pw_DestroyInstance;
    if (str_eq(name, "vkCreateDevice")) return (PFN_vkVoidFunction)pw_CreateDevice;
    if (str_eq(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)pw_GetDeviceProcAddr;
    if (!instance) return 0;
    lock();
    struct instance_entry *e = instance_for(key_of(instance));
    PFN_vkGetInstanceProcAddr gipa = e ? e->gipa : 0;
    unlock();
    return gipa ? gipa(instance, name) : 0;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL pw_GetDeviceProcAddr(VkDevice device, const char *name) {
    lock();
    struct device_entry *e = device_for(key_of(device));
    PFN_vkGetDeviceProcAddr gdpa = e ? e->gdpa : 0;
    PFN_vkQueuePresentKHR present = e ? e->present : 0;
    unlock();
    if (!gdpa) return 0;
    if (str_eq(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)pw_GetDeviceProcAddr;
    if (str_eq(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)pw_DestroyDevice;
    if (str_eq(name, "vkQueuePresentKHR")) return present ? (PFN_vkVoidFunction)pw_QueuePresentKHR : 0;
    return gdpa(device, name);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name) {
    return pw_GetDeviceProcAddr(device, name);
}
