#pragma once

#include "SDL_compat.h"
#include <SDL_ttf.h>

#include <QByteArray>
#include <QHash>
#include <QSet>

#include <functional>
#include <vector>

// A PC keyboard on screen, after Odin's (and DroidDeck's): a three finger tap shows or hides it.
// Every key is a physical key, pressed while a finger is on it and released when the finger
// lifts, so holding one repeats it the way the host's own key repeat does. Esc, the function
// row, the editing block, Ctrl, Alt, Win and the arrows are all there. Ctrl, Shift, Alt and Win
// are sticky: one tap holds the modifier for the next key, a second tap locks it, a third lets
// it go. Nothing stays held when the keyboard hides.
//
// It is drawn along the bottom of the stream window by the libplacebo Vulkan renderer.
class PcKeyboard
{
public:
    // Sends a key press or release; mod is the SDL modifier state that goes with it
    using KeySender = std::function<void(SDL_Scancode scancode, bool down, Uint16 mod)>;

    explicit PcKeyboard(KeySender sender);
    ~PcKeyboard();

    bool isVisible() const { return m_Visible; }
    void toggle();
    void hide();

    // Takes a touch landing on, moving over or lifting from the keyboard. Returns true when the
    // keyboard used it, so the stream must not see it. windowAspect is the stream window's
    // width over its height.
    bool handleTouch(const SDL_TouchFingerEvent* event, float windowAspect);

    // Watches every touch for the three finger tap; usedByKeyboard is handleTouch()'s answer.
    // Returns true when the tap just completed.
    bool observeToggleGesture(const SDL_TouchFingerEvent* event, bool usedByKeyboard);

private:
    struct Key {
        const char* label;
        SDL_Scancode scancode;
        float weight;
        const char* shifted;
        SDL_Rect rect;
        bool down;
    };

    enum ModState { ModOff, ModOnce, ModLocked };

    // What a finger on the keyboard is holding
    enum : int { TargetNone = -1, TargetHide = -2 };

    void layOut();
    bool openFonts();
    void render();
    void press(Key& key);
    void release(Key& key);
    void releaseAll();
    Uint16 heldModifiers() const;
    ModState modState(SDL_Scancode scancode) const;
    int keyAt(int x, int y) const;
    TTF_Font* fontFor(const char* label);

    KeySender m_Send;
    std::vector<Key> m_Keys;
    SDL_Rect m_HideButton;
    QHash<int, ModState> m_Mods;
    bool m_CapsOn;
    bool m_Visible;

    // Fingers on the keyboard and what each holds; fingers still down when it hid are kept
    // so their lift does not reach the stream either
    QHash<SDL_FingerID, int> m_Fingers;
    QSet<SDL_FingerID> m_SwallowedFingers;

    // Three finger tap
    QSet<SDL_FingerID> m_GestureFingers;
    int m_GestureMaxFingers;
    Uint32 m_GestureThirdFingerTicks;
    bool m_GestureOnKeyboard;

    QByteArray m_FallbackFontData;
    TTF_Font* m_KeyFont;
    TTF_Font* m_SmallFont;
    TTF_Font* m_TitleFont;
    bool m_HasArrows;
};
