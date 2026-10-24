#pragma once

#include <QString>

#include "SDL_compat.h"
#include <SDL_ttf.h>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    OverlayMax
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;

    // The on-screen PC keyboard changed; renderers that draw it pick up
    // OverlayManager::getUpdatedKeyboardSurface()
    virtual void notifyKeyboardOverlayUpdated() {}
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    void setOverlayRenderer(IOverlayRenderer* renderer);

    // The on-screen PC keyboard is drawn along the bottom edge, full width, from a
    // ready-made surface. Takes ownership of surface; nullptr hides the keyboard.
    void setKeyboardSurface(SDL_Surface* surface);
    bool isKeyboardVisible();
    // Returns a new keyboard surface if there is one (the caller frees it), else nullptr
    SDL_Surface* getUpdatedKeyboardSurface();

private:
    void notifyOverlayUpdated(OverlayType type);
    SDL_Surface* RenderTextOutlinedWrapped(TTF_Font* font, const char* text, SDL_Color textColor, SDL_Color outlineColor, int outlineWidth, int wrapWidth);

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        char text[2048];

        TTF_Font* font;
        SDL_Surface* surface;
    } m_Overlays[OverlayMax];
    SDL_atomic_t m_KeyboardVisible;
    SDL_Surface* m_KeyboardSurface;
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
};

}
