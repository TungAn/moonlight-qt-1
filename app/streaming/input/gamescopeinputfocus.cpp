#include "gamescopeinputfocus.h"

#ifdef HAS_GAMESCOPE_INPUT
#include <QDir>
#include <QtGlobal>
#include <algorithm>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <X11/Xauth.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <system_error>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

struct GamescopeInputFocus::Impl
{
    Uint32 wakeEvent;
    uint32_t appId;
    std::vector<std::string> displays;
    std::atomic<uint64_t> state{Pending};
    std::atomic<bool> stopping{false};
    std::atomic<Uint32> setupStarted{0};
    std::mutex socketLock;
    int interruptSocket = -1; // An owned dup, never an FD XCB might close/reuse.
    int wakeFd = -1;
    std::thread worker;

    Impl(Uint32 event, uint32_t id, const std::vector<std::string>& candidates)
        : wakeEvent(event), appId(id), displays(candidates)
    {
        wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (wakeFd < 0) {
            state = 0;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Gamescope input: unable to create observer wake FD");
            return;
        }
        try {
            worker = std::thread([this] { run(); });
        }
        catch (const std::system_error& error) {
            close(wakeFd);
            wakeFd = -1;
            state = 0;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Gamescope input: unable to start observer: %s", error.what());
        }
    }

    ~Impl()
    {
        stopping = true;
        uint64_t value = 1;
        if (wakeFd >= 0) {
            (void)write(wakeFd, &value, sizeof(value));
        }
        interrupt(); // Also cancels a server stalled during XCB setup.
        if (worker.joinable()) {
            worker.join();
        }
        if (wakeFd >= 0) {
            close(wakeFd);
        }
    }

    void interrupt(bool stalledSetupOnly = false)
    {
        std::lock_guard<std::mutex> lock(socketLock);
        const Uint32 started = setupStarted.load();
        if (stalledSetupOnly && (!started || int32_t(SDL_GetTicks() - started) <= 1000)) {
            return; // Setup may have completed since snapshot() checked it.
        }
        if (interruptSocket >= 0) {
            shutdown(interruptSocket, SHUT_RDWR);
        }
    }

    void closeInterruptSocket()
    {
        std::lock_guard<std::mutex> lock(socketLock);
        if (interruptSocket >= 0) {
            close(interruptSocket);
            interruptSocket = -1;
        }
        setupStarted = 0;
    }

    void publish(uint64_t flags)
    {
        uint64_t previous = state.load();
        uint64_t generation = previous >> GenerationShift;
        if ((previous & Allowed) && !(flags & Allowed)) {
            ++generation;
        }
        uint64_t next = (generation << GenerationShift) | flags;
        if (next == previous) {
            return;
        }
        state.store(next);
        if (wakeEvent) {
            SDL_Event event = {};
            event.type = wakeEvent;
            SDL_PushEvent(&event); // No observer/handler pointer crosses threads.
        }
    }

    bool waitForSocket(int fd, int timeout, short events = POLLIN)
    {
        pollfd fds[] = {{wakeFd, POLLIN, 0}, {fd, events, 0}};
        int result;
        do {
            result = poll(fds, 2, timeout);
        } while (result < 0 && errno == EINTR && !stopping);
        return result > 0 && !stopping && !fds[0].revents &&
            (fds[1].revents & events) && !(fds[1].revents & (POLLERR | POLLHUP | POLLNVAL));
    }

    void* reply(xcb_connection_t* connection, unsigned sequence)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        for (;;) {
            void* data = nullptr;
            xcb_generic_error_t* error = nullptr;
            if (xcb_poll_for_reply(connection, sequence, &data, &error)) {
                free(error);
                return data;
            }
            if (stopping || xcb_connection_has_error(connection)) {
                return nullptr;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0 || !waitForSocket(xcb_get_file_descriptor(connection), int(left))) {
                xcb_discard_reply(connection, sequence);
                return nullptr;
            }
        }
    }

    xcb_connection_t* connectLocal(const std::string& display)
    {
        // Never probe remote X servers or assume that Gamescope uses :0.
        if (display.empty() || display[0] != ':') {
            return nullptr;
        }
        const std::string number = display.substr(1, display.find('.') - 1);
        if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos || number.size() > 5) {
            return nullptr;
        }
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) {
            return nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(socketLock);
            interruptSocket = fcntl(fd, F_DUPFD_CLOEXEC, 0);
            if (interruptSocket < 0 || stopping) {
                close(fd);
                if (interruptSocket >= 0) close(interruptSocket);
                interruptSocket = -1;
                return nullptr;
            }
        }
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        const std::string path = "/tmp/.X11-unix/X" + number;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        int result = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (result < 0 && errno == ENOENT) {
            // Linux Xwayland can expose only an abstract Unix socket in a container.
            std::memmove(address.sun_path + 1, path.data(), path.size());
            address.sun_path[0] = 0;
            result = connect(fd, reinterpret_cast<sockaddr*>(&address),
                             offsetof(sockaddr_un, sun_path) + 1 + path.size());
        }
        if ((result < 0 && errno != EINPROGRESS && errno != EAGAIN) ||
                (result < 0 && !waitForSocket(fd, 500, POLLOUT))) {
            close(fd);
            closeInterruptSocket();
            return nullptr;
        }
        char hostname[256] = {};
        gethostname(hostname, sizeof(hostname) - 1);
        char cookieName[] = "MIT-MAGIC-COOKIE-1";
        const char* authHostname = SDL_getenv("XAUTHLOCALHOSTNAME");
        if (!authHostname || !*authHostname) authHostname = hostname;
        Xauth* auth = XauGetAuthByAddr(FamilyLocal, std::strlen(authHostname), authHostname,
                                      number.size(), number.data(), sizeof(cookieName) - 1, cookieName);
        xcb_auth_info_t info = {};
        if (auth) {
            info.namelen = auth->name_length;
            info.name = auth->name;
            info.datalen = auth->data_length;
            info.data = auth->data;
        }
        setupStarted = SDL_GetTicks() + 1;
        auto* connection = xcb_connect_to_fd(fd, auth ? &info : nullptr);
        setupStarted = 0;
        if (auth) XauDisposeAuth(auth);
        if (xcb_connection_has_error(connection)) {
            xcb_disconnect(connection);
            closeInterruptSocket();
            return nullptr;
        }
        return connection;
    }

    bool property(xcb_connection_t* connection, xcb_window_t root, xcb_atom_t atom,
                  std::vector<uint32_t>& values)
    {
        auto request = xcb_get_property(connection, 0, root, atom, XCB_ATOM_CARDINAL, 0, 3072);
        xcb_flush(connection);
        auto* data = static_cast<xcb_get_property_reply_t*>(reply(connection, request.sequence));
        const bool valid = data && data->type == XCB_ATOM_CARDINAL && data->format == 32 && !data->bytes_after;
        values.clear();
        if (valid) {
            auto* first = static_cast<uint32_t*>(xcb_get_property_value(data));
            values.assign(first, first + xcb_get_property_value_length(data) / sizeof(uint32_t));
        }
        free(data);
        return valid;
    }

    bool observe(xcb_connection_t* connection, const std::string& display)
    {
        auto roots = xcb_setup_roots_iterator(xcb_get_setup(connection));
        if (!roots.rem) return false;
        const auto root = roots.data->root;
        const char* names[] = {"GAMESCOPE_FOCUSED_APP", "GAMESCOPE_FOCUSED_APP_GFX", "GAMESCOPE_FOCUSABLE_WINDOWS"};
        xcb_atom_t atoms[3] = {};
        for (int i = 0; i < 3; ++i) {
            auto request = xcb_intern_atom(connection, 1, std::strlen(names[i]), names[i]);
            xcb_flush(connection);
            auto* data = static_cast<xcb_intern_atom_reply_t*>(reply(connection, request.sequence));
            atoms[i] = data ? data->atom : xcb_atom_t(XCB_NONE);
            free(data);
            if (!atoms[i]) return false;
        }
        const uint32_t events = XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(connection, root, XCB_CW_EVENT_MASK, &events);
        std::vector<uint32_t> input, graphics, windows;
        // Actual properties, not merely interned atom names, identify the root server.
        if (!property(connection, root, atoms[0], input) || input.size() > 1 ||
                !property(connection, root, atoms[1], graphics) || graphics.size() > 1 ||
                !property(connection, root, atoms[2], windows) || windows.size() % 3) {
            return false;
        }
        uint32_t identity = 0;
        for (size_t i = 0; i < windows.size(); i += 3) {
            if (windows[i + 2] == uint32_t(getpid()) && windows[i + 1]) {
                identity = windows[i + 1];
                break;
            }
        }
        // Wayland windows need the inherited launch ID. Require association with
        // this compositor's current game or its list before selecting its server.
        if (!identity && appId) {
            for (size_t i = 0; i < windows.size(); i += 3) {
                if (windows[i + 1] == appId) identity = appId;
            }
            if (graphics.size() == 1 && graphics[0] == appId) identity = appId;
        }
        if (!identity) return false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Gamescope input observer: display=%s app=%u (SDL window focus is independent)",
                    display.c_str(), identity);
        unsigned inputNotifications = 0, graphicsNotifications = 0;
        for (;;) {
            bool changed = false;
            while (auto* event = xcb_poll_for_event(connection)) {
                if ((event->response_type & 0x7f) == XCB_PROPERTY_NOTIFY) {
                    auto* propertyEvent = reinterpret_cast<xcb_property_notify_event_t*>(event);
                    if (propertyEvent->atom == atoms[0]) {
                        ++inputNotifications;
                        changed = true;
                    }
                    else if (propertyEvent->atom == atoms[1]) {
                        ++graphicsNotifications;
                        changed = true;
                    }
                }
                free(event);
            }
            if (changed) {
                // More than one update can hide a complete loss/restoration
                // before the read. Preserve that boundary conservatively. A
                // single same-value refresh must not interrupt held controls.
                if (inputNotifications > 1 || graphicsNotifications > 1) publish(Known);
                bool valid = property(connection, root, atoms[0], input) && input.size() == 1 &&
                    property(connection, root, atoms[1], graphics) && graphics.size() == 1;
                if (!valid) {
                    input.clear();
                    graphics.clear();
                }
                if (!valid || input[0] != identity || graphics[0] != identity) publish(Known);
                continue; // Drain updates received while reading before allowing input.
            }
            const bool allowed = input.size() == 1 && graphics.size() == 1 &&
                input[0] == identity && graphics[0] == identity;
            publish(Known | (allowed ? Allowed : 0));
            inputNotifications = graphicsNotifications = 0;
            if (stopping) return true;
            if (xcb_connection_has_error(connection) ||
                    !waitForSocket(xcb_get_file_descriptor(connection), -1)) {
                publish(Known);
                if (!stopping) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Gamescope input observer disconnected; controller input remains suspended");
                return true;
            }
        }
    }

    void run()
    {
        for (const auto& display : displays) {
            if (stopping) return;
            auto* connection = connectLocal(display);
            if (!connection) continue;
            const bool found = observe(connection, display);
            xcb_disconnect(connection);
            closeInterruptSocket();
            if (found) return;
        }
        if (!stopping) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Gamescope input observer unavailable: no accessible root with this app's identity; using SDL focus only");
            publish(0);
        }
    }
};
#else
struct GamescopeInputFocus::Impl {};
#endif

GamescopeInputFocus::GamescopeInputFocus(Uint32 event, uint32_t appId,
                                       const std::vector<std::string>& displays)
{
#ifdef HAS_GAMESCOPE_INPUT
    m_Impl.reset(new Impl(event, appId, displays));
#else
    (void)event;
    (void)appId;
    (void)displays;
#endif
}

GamescopeInputFocus::~GamescopeInputFocus() = default;

std::unique_ptr<GamescopeInputFocus> GamescopeInputFocus::createForCurrentProcess(Uint32 wakeEvent)
{
#ifdef __linux__
    const auto equals = [](const char* name, const char* value) {
        const char* setting = SDL_getenv(name);
        return setting && SDL_strcmp(setting, value) == 0;
    };
    const bool gamescopeSession = equals("SteamGamepadUI", "1") ||
        SDL_getenv("GAMESCOPE_WAYLAND_DISPLAY") || SDL_getenv("STEAM_MULTIPLE_XWAYLANDS") ||
        equals("XDG_CURRENT_DESKTOP", "gamescope");
    if (!gamescopeSession) return {};
#ifdef HAS_GAMESCOPE_INPUT
    const char* driver = SDL_GetCurrentVideoDriver();
    // The Qt menus initialize SDL controllers without SDL video. Their Qt
    // window still belongs to Gamescope and needs the same global observer.
    if (driver && strcmp(driver, "x11") && strcmp(driver, "wayland")) return {};
    if (wakeEvent == Uint32(-1)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Gamescope input observer: unable to register wake event");
        return {};
    }
    std::vector<std::string> displays;
    auto addDisplay = [&](const QByteArray& display) {
        if (!display.isEmpty() && display[0] == ':' &&
                std::find(displays.begin(), displays.end(), display.toStdString()) == displays.end()) {
            displays.push_back(display.toStdString());
        }
    };
    addDisplay(qgetenv("PYROLIGHT_GAMESCOPE_DISPLAY"));
    addDisplay(qgetenv("DISPLAY"));
    for (const QString& socket : QDir("/tmp/.X11-unix").entryList({"X*"}, QDir::System | QDir::Files)) {
        if (displays.size() < 32) addDisplay((":" + socket.mid(1)).toUtf8());
    }
    bool validId = false;
    quint64 id = qgetenv("SteamAppId").toULongLong(&validId);
    if (!validId || !id || id > UINT32_MAX) {
        id = qgetenv("SteamGameId").toULongLong(&validId);
        if (validId && id > UINT32_MAX) id >>= 32;
    }
    return std::unique_ptr<GamescopeInputFocus>(new GamescopeInputFocus(
        wakeEvent, validId ? uint32_t(id) : 0, displays));
#else
    (void)wakeEvent;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Gamescope input observer unavailable in this build (xcb/xau support missing); using window focus only");
#endif
#else
    (void)wakeEvent;
#endif
    return {};
}

uint64_t GamescopeInputFocus::snapshot()
{
#ifdef HAS_GAMESCOPE_INPUT
    const Uint32 started = m_Impl->setupStarted.load();
    if (started && int32_t(SDL_GetTicks() - started) > 1000) {
        m_Impl->interrupt(true); // Recheck under the socket lock before cancelling setup.
    }
    return m_Impl->state.load();
#else
    return 0;
#endif
}
