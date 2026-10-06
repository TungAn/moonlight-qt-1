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
 * finished. It costs the present thread the rest of the frame's GPU time: a frame that used to be
 * picked up while still drawing (shown finished only when the race was won) now reaches the
 * compositor that much later.
 *
 * Off unless asked for, per program: a streaming client gains nothing from it, an emulator drawing
 * heavy frames does. It is on for a program named in /storage/emulated/0/Download/
 * droiddeck-present-wait.txt (one name per line, '#' comments; a case-insensitive match anywhere in
 * the process name, its executable's path or its argv[0]). DROIDDECK_PRESENT_WAIT=1 turns it on for
 * every program, =0 for none (the manifest's disable_environment then skips loading the layer).
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

/* ---- which programs it is on for ---- */
extern int open(const char *path, int flags, ...);
extern long read(int fd, void *buf, unsigned long n);
extern int close(int fd);
extern long readlink(const char *path, char *buf, unsigned long n);
extern char *getenv(const char *name);
static long write_out(const char *s, unsigned long n);

#define LIST_PATH "/storage/emulated/0/Download/droiddeck-present-wait.txt"

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }
/* Case-insensitive: does needle (n bytes) occur in hay? */
static int contains(const char *hay, const char *needle, int n) {
    if (n <= 0) return 0;
    for (; *hay; hay++) {
        int i = 0;
        while (i < n && hay[i] && lower(hay[i]) == lower(needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}
static int read_file(const char *path, char *buf, int cap) {
    int fd = open(path, 0 /* O_RDONLY */);
    if (fd < 0) return 0;
    long n = read(fd, buf, (unsigned long)(cap - 1));
    close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    return (int)n;
}

static int g_decided, g_enabled;
static char g_why[160];

static void put_why(const char *a, const char *b, int bn) {
    char *p = g_why, *end = g_why + sizeof(g_why) - 1;
    while (*a && p < end) *p++ = *a++;
    for (int i = 0; i < bn && p < end; i++) *p++ = b[i];
    *p = 0;
}

static void decide(void) {
    const char *env = getenv("DROIDDECK_PRESENT_WAIT");
    if (env && env[0] == '1') { g_enabled = 1; put_why("DROIDDECK_PRESENT_WAIT=1", "", 0); return; }
    if (env && env[0] == '0') { g_enabled = 0; put_why("DROIDDECK_PRESENT_WAIT=0", "", 0); return; }
    static char comm[64], exe[512], argv0[512], list[4096];
    read_file("/proc/self/comm", comm, sizeof(comm));
    long n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    exe[n > 0 ? n : 0] = 0;
    read_file("/proc/self/cmdline", argv0, sizeof(argv0)); /* the first NUL ends argv[0] */
    if (!read_file(LIST_PATH, list, sizeof(list))) { put_why("no " LIST_PATH, "", 0); return; }
    for (char *line = list; *line;) {
        char *e = line;
        while (*e && *e != '\n') e++;
        char *a = line, *b = e;
        while (a < b && (*a == ' ' || *a == '\t' || *a == '\r')) a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r')) b--;
        if (a < b && *a != '#' &&
            (contains(comm, a, (int)(b - a)) || contains(exe, a, (int)(b - a)) || contains(argv0, a, (int)(b - a)))) {
            g_enabled = 1;
            put_why("listed: ", a, (int)(b - a));
            return;
        }
        line = *e ? e + 1 : e;
    }
    put_why("not in " LIST_PATH, "", 0);
}

/* Decided once per process, when its first device is made; said on stderr (the session log). */
static int enabled(void) {
    lock();
    int first = !g_decided;
    if (first) { g_decided = 1; decide(); }
    int on = g_enabled;
    unlock();
    if (first) {
        static char comm[64];
        read_file("/proc/self/comm", comm, sizeof(comm));
        char line[300], *p = line;
        const char *head = on ? "[present-wait] on for " : "[present-wait] off for ";
        while (*head) *p++ = *head++;
        for (const char *c = comm; *c && *c != '\n' && p < line + 80; c++) *p++ = *c;
        *p++ = ' '; *p++ = '(';
        for (const char *c = g_why; *c && p < line + 290; c++) *p++ = *c;
        *p++ = ')'; *p++ = '\n';
        write_out(line, (unsigned long)(p - line));
    }
    return on;
}

/* ---- what the wait costs, said every 10 s on stderr (the session log) ----
 * "waited" is how long a present was held for its frame's GPU work: the time the frame would
 * otherwise have reached the compositor unfinished. The libc calls resolve at load time. */
struct pw_timespec { long tv_sec; long tv_nsec; };
extern int clock_gettime(int clock, struct pw_timespec *ts);
extern long write(int fd, const void *buf, unsigned long n);
extern int getpid(void);
static long write_out(const char *s, unsigned long n) { return write(2, s, n); }

static long long now_ns(void) {
    struct pw_timespec ts;
    clock_gettime(1 /* CLOCK_MONOTONIC */, &ts);
    return (long long)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_uint(char *p, unsigned long long v) {
    char tmp[24];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}
static char *put_ms(char *p, long long ns) { /* milliseconds with two decimals */
    if (ns < 0) ns = 0;
    long long hundredths = (ns + 5000) / 10000;
    p = put_uint(p, (unsigned long long)(hundredths / 100));
    *p++ = '.';
    *p++ = (char)('0' + hundredths / 10 % 10);
    *p++ = (char)('0' + hundredths % 10);
    return p;
}

static struct { long long since, total, max; unsigned n, over_4ms; } g_stat;

static void stat_add(long long waited) {
    lock();
    long long t = now_ns();
    if (!g_stat.since) g_stat.since = t;
    g_stat.n++;
    g_stat.total += waited;
    if (waited > g_stat.max) g_stat.max = waited;
    if (waited > 4000000) g_stat.over_4ms++;
    int report = t - g_stat.since >= 10000000000ll;
    unsigned n = g_stat.n, over = g_stat.over_4ms;
    long long total = g_stat.total, max = g_stat.max;
    if (report) { g_stat.since = t; g_stat.n = 0; g_stat.total = 0; g_stat.max = 0; g_stat.over_4ms = 0; }
    unlock();
    if (!report || !n) return;
    char line[200], *p = line;
    p = put_str(p, "[present-wait] pid ");
    p = put_uint(p, (unsigned long long)getpid());
    p = put_str(p, ", last 10 s: ");
    p = put_uint(p, n);
    p = put_str(p, " presents, held for their GPU work avg ");
    p = put_ms(p, total / n);
    p = put_str(p, " ms, max ");
    p = put_ms(p, max);
    p = put_str(p, " ms, over 4 ms: ");
    p = put_uint(p, over);
    *p++ = '\n';
    write(2, line, (unsigned long)(p - line));
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
    long long t0 = now_ns();
    r = dev.wait_fences(dev.device, 1, &fence, VK_TRUE, 1000000000ull);
    stat_add(now_ns() - t0);
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
    if (str_eq(name, "vkQueuePresentKHR")) {
        if (!present) return 0;
        return enabled() ? (PFN_vkVoidFunction)pw_QueuePresentKHR : (PFN_vkVoidFunction)present;
    }
    return gdpa(device, name);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name) {
    return pw_GetDeviceProcAddr(device, name);
}
