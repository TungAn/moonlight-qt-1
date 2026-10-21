#pragma once

#include <xcb/xcb.h>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <unistd.h>

static constexpr uint32_t App = 0x87654321;

struct Root {
    xcb_connection_t* connection;
    xcb_window_t window;
    xcb_atom_t input, graphics, windows;

    explicit Root(const char* display)
    {
        connection = xcb_connect(display, nullptr);
        assert(!xcb_connection_has_error(connection));
        window = xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
        input = atom("GAMESCOPE_FOCUSED_APP");
        graphics = atom("GAMESCOPE_FOCUSED_APP_GFX");
        windows = atom("GAMESCOPE_FOCUSABLE_WINDOWS");
    }
    ~Root() { xcb_disconnect(connection); }
    xcb_atom_t atom(const char* name)
    {
        auto* reply = xcb_intern_atom_reply(connection,
            xcb_intern_atom(connection, 0, std::strlen(name), name), nullptr);
        assert(reply);
        auto result = reply->atom;
        free(reply);
        return result;
    }
    void set(xcb_atom_t atom, const std::vector<uint32_t>& values, xcb_atom_t type = XCB_ATOM_CARDINAL)
    {
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, atom, type, 32,
                            values.size(), values.data());
    }
    void flush()
    {
        auto* reply = xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr);
        assert(reply);
        free(reply);
    }
    void focus(uint32_t id)
    {
        set(input, {id});
        flush();
    }
    void initialize(bool pidIdentity)
    {
        set(input, {App});
        set(graphics, {App});
        set(windows, {1234, App, pidIdentity ? uint32_t(getpid()) : 999999});
        flush();
    }
};

