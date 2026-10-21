#include "streaming/input/gamescopeinputfocus.h"
#include <xcb/xcb.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <iostream>
#include <functional>
#include <signal.h>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

void runGamescopeControllerInputTest(const std::vector<std::string>&, const std::function<void(bool)>&);

#include "gamescope_root_fixture.h"


template<class Predicate>
static uint64_t await(GamescopeInputFocus& observer, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    uint64_t state;
    do {
        state = observer.snapshot();
        if (predicate(state)) return state;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    std::cerr << "Observer state timed out: " << state << '\n';
    std::abort();
}

static bool allowed(uint64_t state)
{
    return (state & (GamescopeInputFocus::Known | GamescopeInputFocus::Allowed)) ==
        (GamescopeInputFocus::Known | GamescopeInputFocus::Allowed);
}

int main(int argc, char** argv)
{
    assert(argc == 4);
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0);
    const Uint32 eventType = SDL_RegisterEvents(1);
    Root root(argv[1]);
    Root game(argv[2]); // Atom names exist here, but their root properties do not.
    root.initialize(true);
    {
        // Discover the correct root, not the application's own DISPLAY.
        GamescopeInputFocus observer(eventType, 0, {argv[2], argv[1]});
        uint64_t original = await(observer, allowed);
        root.focus(App); // One same-value refresh must not neutralize held input.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(observer.snapshot() == original);
        root.focus(769); // Steam/QAM takes input; visual game and SDL focus stay unchanged.
        uint64_t blocked = await(observer, [](uint64_t state) {
            return (state & GamescopeInputFocus::Known) && !(state & GamescopeInputFocus::Allowed);
        });
        assert((blocked >> GamescopeInputFocus::GenerationShift) >
               (original >> GamescopeInputFocus::GenerationShift));
        root.focus(App);
        await(observer, allowed);

        // Switch away entirely, then restore both graphical and input ownership.
        root.set(root.graphics, {42});
        root.focus(42);
        await(observer, [](uint64_t state) { return !GamescopeInputFocus::permits(state); });
        root.set(root.graphics, {App});
        root.focus(App);
        await(observer, allowed);

        // Fast open/close must remain observable even if final values are identical.
        original = observer.snapshot();
        root.set(root.input, {769});
        root.set(root.input, {App});
        root.flush();
        await(observer, [=](uint64_t state) {
            return allowed(state) && (state >> GamescopeInputFocus::GenerationShift) >
                (original >> GamescopeInputFocus::GenerationShift);
        });
        for (int i = 0; i < 10; ++i) {
            root.focus(769);
            await(observer, [](uint64_t state) { return !GamescopeInputFocus::permits(state); });
            root.focus(App);
            await(observer, allowed);
        }
        // Deleted, empty, malformed, or incorrectly typed ownership is never "closed".
        root.set(root.input, {});
        root.flush();
        await(observer, [](uint64_t state) { return !GamescopeInputFocus::permits(state); });
        root.set(root.input, {App}, XCB_ATOM_WINDOW);
        root.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(!GamescopeInputFocus::permits(observer.snapshot()));
        xcb_delete_property(root.connection, root.window, root.input);
        root.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(!GamescopeInputFocus::permits(observer.snapshot()));
        root.focus(App);
        await(observer, allowed);
        assert(SDL_HasEvent(eventType));
    }
    root.initialize(true);
    runGamescopeControllerInputTest({argv[2], argv[1]}, [&](bool owns) { root.focus(owns ? App : 769); });
    // Exercise actual setWindow() startup as well as the injected observer.
    bool hasX11 = false;
    for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i) {
        hasX11 |= !std::strcmp(SDL_GetVideoDriver(i), "x11");
    }
    if (hasX11) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        SDL_setenv("SDL_VIDEODRIVER", "x11", 1);
        SDL_setenv("SteamGamepadUI", "1", 1);
        SDL_setenv("PYROLIGHT_GAMESCOPE_DISPLAY", argv[1], 1);
        assert(SDL_InitSubSystem(SDL_INIT_VIDEO) == 0);
        runGamescopeControllerInputTest({}, [&](bool owns) { root.focus(owns ? App : 769); });
    }
    {
        // Native Wayland: use a validated inherited app ID without any X window PID.
        root.initialize(false);
        GamescopeInputFocus observer(eventType, App, {argv[1]});
        await(observer, allowed);
    }
    {
        GamescopeInputFocus unrelated(eventType, 123, {argv[1]});
        await(unrelated, [](uint64_t state) { return !(state & GamescopeInputFocus::Pending); });
        assert(!(unrelated.snapshot() & GamescopeInputFocus::Known));
    }
    {
        GamescopeInputFocus absent(eventType, App, {argv[2]});
        await(absent, [](uint64_t state) { return !(state & GamescopeInputFocus::Pending); });
        assert(!(absent.snapshot() & GamescopeInputFocus::Known));
    }
    root.initialize(true);
    {
        GamescopeInputFocus disconnected(eventType, App, {argv[1]});
        await(disconnected, allowed);
        kill(std::atoi(argv[3]), SIGTERM);
        await(disconnected, [](uint64_t state) {
            return (state & GamescopeInputFocus::Known) && !GamescopeInputFocus::permits(state);
        });
    }
    // A local server that accepts but never answers the X handshake cannot hang shutdown.
    const std::string number = std::to_string(40000 + getpid() % 20000);
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::string name = "/tmp/.X11-unix/X" + number;
    std::memcpy(address.sun_path + 1, name.data(), name.size()); // abstract: no filesystem mutation
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(listener >= 0);
    assert(bind(listener, reinterpret_cast<sockaddr*>(&address), offsetof(sockaddr_un, sun_path) + 1 + name.size()) == 0);
    assert(listen(listener, 1) == 0);
    auto started = std::chrono::steady_clock::now();
    int accepted = -1;
    {
        GamescopeInputFocus stalled(eventType, App, {":" + number});
        accepted = accept(listener, nullptr, nullptr);
        assert(accepted >= 0);
        // Destructor interrupts an in-progress setup rather than joining forever.
    }
    close(accepted);
    assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    close(listener);
    SDL_Quit();
    std::cout << "Gamescope global input ownership observer checks passed\n";
}
