#include "pckeyboard.h"

#include "path.h"
#include "streaming/session.h"

#include <cstring>

namespace {

// The keyboard is drawn into a surface this wide and scaled to the window's width; its height
// is a little under half of a 16:9 window, header included
constexpr int k_Width = 1920;
constexpr int k_Padding = 16;
constexpr int k_HeaderHeight = 50;
constexpr int k_RowHeight = 68;
constexpr int k_Gap = 6;
constexpr int k_RowCount = 6;
constexpr int k_Height = k_Padding / 2 + k_HeaderHeight + k_RowCount * (k_RowHeight + k_Gap) + k_Padding / 2;

// A tap: all three fingers lift within this long of the third landing
constexpr Uint32 k_TapMs = 300;

struct KeyDef {
    const char* label;
    SDL_Scancode scancode;
    float weight;
    const char* shifted;
};

const KeyDef k_Rows[k_RowCount][20] = {
    {
        {"Esc", SDL_SCANCODE_ESCAPE, 1, nullptr}, {"F1", SDL_SCANCODE_F1, 1, nullptr},
        {"F2", SDL_SCANCODE_F2, 1, nullptr}, {"F3", SDL_SCANCODE_F3, 1, nullptr},
        {"F4", SDL_SCANCODE_F4, 1, nullptr}, {"F5", SDL_SCANCODE_F5, 1, nullptr},
        {"F6", SDL_SCANCODE_F6, 1, nullptr}, {"F7", SDL_SCANCODE_F7, 1, nullptr},
        {"F8", SDL_SCANCODE_F8, 1, nullptr}, {"F9", SDL_SCANCODE_F9, 1, nullptr},
        {"F10", SDL_SCANCODE_F10, 1, nullptr}, {"F11", SDL_SCANCODE_F11, 1, nullptr},
        {"F12", SDL_SCANCODE_F12, 1, nullptr}, {"Ins", SDL_SCANCODE_INSERT, 1, nullptr},
        {"Del", SDL_SCANCODE_DELETE, 1, nullptr}, {"Home", SDL_SCANCODE_HOME, 1, nullptr},
        {"End", SDL_SCANCODE_END, 1, nullptr}, {"PgUp", SDL_SCANCODE_PAGEUP, 1, nullptr},
        {"PgDn", SDL_SCANCODE_PAGEDOWN, 1, nullptr},
    },
    {
        {"`", SDL_SCANCODE_GRAVE, 1, "~"}, {"1", SDL_SCANCODE_1, 1, "!"}, {"2", SDL_SCANCODE_2, 1, "@"},
        {"3", SDL_SCANCODE_3, 1, "#"}, {"4", SDL_SCANCODE_4, 1, "$"}, {"5", SDL_SCANCODE_5, 1, "%"},
        {"6", SDL_SCANCODE_6, 1, "^"}, {"7", SDL_SCANCODE_7, 1, "&"}, {"8", SDL_SCANCODE_8, 1, "*"},
        {"9", SDL_SCANCODE_9, 1, "("}, {"0", SDL_SCANCODE_0, 1, ")"}, {"-", SDL_SCANCODE_MINUS, 1, "_"},
        {"=", SDL_SCANCODE_EQUALS, 1, "+"}, {"Backspace", SDL_SCANCODE_BACKSPACE, 2, nullptr},
    },
    {
        {"Tab", SDL_SCANCODE_TAB, 1.5f, nullptr}, {"q", SDL_SCANCODE_Q, 1, nullptr},
        {"w", SDL_SCANCODE_W, 1, nullptr}, {"e", SDL_SCANCODE_E, 1, nullptr},
        {"r", SDL_SCANCODE_R, 1, nullptr}, {"t", SDL_SCANCODE_T, 1, nullptr},
        {"y", SDL_SCANCODE_Y, 1, nullptr}, {"u", SDL_SCANCODE_U, 1, nullptr},
        {"i", SDL_SCANCODE_I, 1, nullptr}, {"o", SDL_SCANCODE_O, 1, nullptr},
        {"p", SDL_SCANCODE_P, 1, nullptr}, {"[", SDL_SCANCODE_LEFTBRACKET, 1, "{"},
        {"]", SDL_SCANCODE_RIGHTBRACKET, 1, "}"}, {"\\", SDL_SCANCODE_BACKSLASH, 1.5f, "|"},
    },
    {
        {"Caps", SDL_SCANCODE_CAPSLOCK, 1.75f, nullptr}, {"a", SDL_SCANCODE_A, 1, nullptr},
        {"s", SDL_SCANCODE_S, 1, nullptr}, {"d", SDL_SCANCODE_D, 1, nullptr},
        {"f", SDL_SCANCODE_F, 1, nullptr}, {"g", SDL_SCANCODE_G, 1, nullptr},
        {"h", SDL_SCANCODE_H, 1, nullptr}, {"j", SDL_SCANCODE_J, 1, nullptr},
        {"k", SDL_SCANCODE_K, 1, nullptr}, {"l", SDL_SCANCODE_L, 1, nullptr},
        {";", SDL_SCANCODE_SEMICOLON, 1, ":"}, {"'", SDL_SCANCODE_APOSTROPHE, 1, "\""},
        {"Enter", SDL_SCANCODE_RETURN, 2.25f, nullptr},
    },
    {
        {"Shift", SDL_SCANCODE_LSHIFT, 2.25f, nullptr}, {"z", SDL_SCANCODE_Z, 1, nullptr},
        {"x", SDL_SCANCODE_X, 1, nullptr}, {"c", SDL_SCANCODE_C, 1, nullptr},
        {"v", SDL_SCANCODE_V, 1, nullptr}, {"b", SDL_SCANCODE_B, 1, nullptr},
        {"n", SDL_SCANCODE_N, 1, nullptr}, {"m", SDL_SCANCODE_M, 1, nullptr},
        {",", SDL_SCANCODE_COMMA, 1, "<"}, {".", SDL_SCANCODE_PERIOD, 1, ">"},
        {"/", SDL_SCANCODE_SLASH, 1, "?"}, {"Shift", SDL_SCANCODE_RSHIFT, 1.75f, nullptr},
        {"\xE2\x86\x91", SDL_SCANCODE_UP, 1, nullptr},
    },
    {
        {"Ctrl", SDL_SCANCODE_LCTRL, 1.5f, nullptr}, {"Win", SDL_SCANCODE_LGUI, 1.25f, nullptr},
        {"Alt", SDL_SCANCODE_LALT, 1.25f, nullptr}, {"Space", SDL_SCANCODE_SPACE, 6.25f, nullptr},
        {"AltGr", SDL_SCANCODE_RALT, 1.25f, nullptr}, {"Ctrl", SDL_SCANCODE_RCTRL, 1.25f, nullptr},
        {"\xE2\x86\x90", SDL_SCANCODE_LEFT, 1, nullptr}, {"\xE2\x86\x93", SDL_SCANCODE_DOWN, 1, nullptr},
        {"\xE2\x86\x92", SDL_SCANCODE_RIGHT, 1, nullptr},
    },
};

// Labels for the arrows when the font has no arrow glyphs
const char* asciiArrow(SDL_Scancode scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_UP: return "Up";
    case SDL_SCANCODE_DOWN: return "Dn";
    case SDL_SCANCODE_LEFT: return "Lt";
    case SDL_SCANCODE_RIGHT: return "Rt";
    default: return nullptr;
    }
}

bool isModifier(SDL_Scancode scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT:
    case SDL_SCANCODE_LGUI:
        return true;
    default:
        return false;
    }
}

Uint16 modifierFlag(SDL_Scancode scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_LSHIFT: return KMOD_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return KMOD_RSHIFT;
    case SDL_SCANCODE_LCTRL: return KMOD_LCTRL;
    case SDL_SCANCODE_RCTRL: return KMOD_RCTRL;
    case SDL_SCANCODE_LALT: return KMOD_LALT;
    case SDL_SCANCODE_RALT: return KMOD_RALT;
    case SDL_SCANCODE_LGUI: return KMOD_LGUI;
    default: return KMOD_NONE;
    }
}

// Colors: a dark translucent panel, keys a shade lighter, held keys in the accent blue
struct Rgba { Uint8 r, g, b, a; };
const Rgba k_Panel = {0x10, 0x14, 0x18, 0xE6};
const Rgba k_KeyFill = {0x22, 0x26, 0x2A, 0xF0};
const Rgba k_KeyBorder = {0x34, 0x38, 0x3C, 0xF0};
const Rgba k_HeldFill = {0x1F, 0x3A, 0x58, 0xF0};
const Rgba k_LockedFill = {0x2A, 0x52, 0x80, 0xF0};
const Rgba k_DownFill = {0x31, 0x60, 0x95, 0xF8};
const Rgba k_AccentBorder = {0x37, 0x76, 0xB8, 0xF8};
const SDL_Color k_Text = {0xFF, 0xFF, 0xFF, 0xFF};
const SDL_Color k_TitleText = {0xB3, 0xB3, 0xB3, 0xFF};

void fill(SDL_Surface* surface, const SDL_Rect& rect, const Rgba& color)
{
    SDL_FillRect(surface, &rect, SDL_MapRGBA(surface->format, color.r, color.g, color.b, color.a));
}

void fillKey(SDL_Surface* surface, const SDL_Rect& rect, const Rgba& body, const Rgba& border)
{
    fill(surface, rect, border);
    SDL_Rect inner = {rect.x + 2, rect.y + 2, rect.w - 4, rect.h - 4};
    fill(surface, inner, body);
}

void drawText(SDL_Surface* surface, TTF_Font* font, const char* text, SDL_Color color, const SDL_Rect& box)
{
    if (font == nullptr || text == nullptr || text[0] == '\0') {
        return;
    }
    SDL_Surface* glyphs = TTF_RenderUTF8_Blended(font, text, color);
    if (glyphs == nullptr) {
        return;
    }
    SDL_Rect dst = {box.x + (box.w - glyphs->w) / 2, box.y + (box.h - glyphs->h) / 2, glyphs->w, glyphs->h};
    SDL_SetSurfaceBlendMode(glyphs, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(glyphs, nullptr, surface, &dst);
    SDL_FreeSurface(glyphs);
}

}

PcKeyboard::PcKeyboard(KeySender sender) :
    m_Send(std::move(sender)),
    m_HideButton{},
    m_CapsOn(false),
    m_Visible(false),
    m_GestureMaxFingers(0),
    m_GestureThirdFingerTicks(0),
    m_GestureOnKeyboard(false),
    m_KeyFont(nullptr),
    m_SmallFont(nullptr),
    m_TitleFont(nullptr),
    m_HasArrows(false)
{
    layOut();
}

PcKeyboard::~PcKeyboard()
{
    hide();
    for (TTF_Font* font : {m_KeyFont, m_SmallFont, m_TitleFont}) {
        if (font != nullptr) {
            TTF_CloseFont(font);
        }
    }
}

void PcKeyboard::layOut()
{
    m_HideButton = {k_Width - k_Padding - 160, k_Padding / 2 + 6, 160, k_HeaderHeight - 12};

    int y = k_Padding / 2 + k_HeaderHeight;
    for (const auto& row : k_Rows) {
        int count = 0;
        float weights = 0;
        while (count < 20 && row[count].label != nullptr) {
            weights += row[count].weight;
            count++;
        }

        float unit = (k_Width - 2 * k_Padding - (count - 1) * k_Gap) / weights;
        float x = k_Padding;
        for (int i = 0; i < count; i++) {
            int left = (int)(x + 0.5f);
            x += row[i].weight * unit;
            // The last key ends exactly at the right padding
            int right = i == count - 1 ? k_Width - k_Padding : (int)(x + 0.5f);
            m_Keys.push_back({row[i].label, row[i].scancode, row[i].weight, row[i].shifted,
                              {left, y, right - left, k_RowHeight}, false});
            x += k_Gap;
        }
        y += k_RowHeight + k_Gap;
    }
}

bool PcKeyboard::openFonts()
{
    if (m_KeyFont != nullptr) {
        return true;
    }

    // A clear sans font with arrows from the system, else the one Pyrolight ships with
    const char* const candidates[] = {
        "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    };
    for (const char* path : candidates) {
        m_KeyFont = TTF_OpenFont(path, 30);
        if (m_KeyFont != nullptr) {
            m_SmallFont = TTF_OpenFont(path, 22);
            m_TitleFont = TTF_OpenFont(path, 22);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PC keyboard font: %s", path);
            break;
        }
    }

    if (m_KeyFont == nullptr) {
        m_FallbackFontData = Path::readDataFile("ModeSeven.ttf");
        if (!m_FallbackFontData.isEmpty()) {
            m_KeyFont = TTF_OpenFontRW(SDL_RWFromConstMem(m_FallbackFontData.constData(), m_FallbackFontData.size()), 1, 30);
            m_SmallFont = TTF_OpenFontRW(SDL_RWFromConstMem(m_FallbackFontData.constData(), m_FallbackFontData.size()), 1, 22);
            m_TitleFont = TTF_OpenFontRW(SDL_RWFromConstMem(m_FallbackFontData.constData(), m_FallbackFontData.size()), 1, 22);
        }
    }

    if (m_KeyFont == nullptr || m_SmallFont == nullptr || m_TitleFont == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PC keyboard: no usable font: %s", TTF_GetError());
        return false;
    }

    m_HasArrows = TTF_GlyphIsProvided32(m_KeyFont, 0x2190) && TTF_GlyphIsProvided32(m_KeyFont, 0x2193);
    return true;
}

TTF_Font* PcKeyboard::fontFor(const char* label)
{
    return strlen(label) > 2 && (unsigned char)label[0] < 0x80 ? m_SmallFont : m_KeyFont;
}

void PcKeyboard::toggle()
{
    if (m_Visible) {
        hide();
        return;
    }
    if (!openFonts()) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PC keyboard shown");
    m_Visible = true;
    render();
}

void PcKeyboard::hide()
{
    if (!m_Visible) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "PC keyboard hidden");
    releaseAll();
    for (auto it = m_Fingers.constBegin(); it != m_Fingers.constEnd(); ++it) {
        m_SwallowedFingers.insert(it.key());
    }
    m_Fingers.clear();
    m_Visible = false;

    Session* session = Session::get();
    if (session != nullptr) {
        session->getOverlayManager().setKeyboardSurface(nullptr);
    }
}

void PcKeyboard::render()
{
    if (!m_Visible) {
        return;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, k_Width, k_Height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        return;
    }
    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);
    fill(surface, {0, 0, k_Width, k_Height}, k_Panel);
    fill(surface, {0, 0, k_Width, 2}, k_KeyBorder);

    drawText(surface, m_TitleFont, "PC keyboard  -  three finger tap to hide", k_TitleText,
             {k_Padding, k_Padding / 2, 600, k_HeaderHeight});
    fillKey(surface, m_HideButton, k_KeyFill, k_KeyBorder);
    drawText(surface, m_TitleFont, "Hide", k_Text, m_HideButton);

    bool shifted = modState(SDL_SCANCODE_LSHIFT) != ModOff || modState(SDL_SCANCODE_RSHIFT) != ModOff;
    for (const Key& key : m_Keys) {
        ModState state = modState(key.scancode);
        bool lit = state != ModOff || (key.scancode == SDL_SCANCODE_CAPSLOCK && m_CapsOn);
        if (key.down) {
            fillKey(surface, key.rect, k_DownFill, k_AccentBorder);
        }
        else if (lit) {
            fillKey(surface, key.rect, state == ModLocked ? k_LockedFill : k_HeldFill, k_AccentBorder);
        }
        else {
            fillKey(surface, key.rect, k_KeyFill, k_KeyBorder);
        }

        const char* label = key.label;
        char upper[2] = {};
        if (shifted && key.shifted != nullptr) {
            label = key.shifted;
        }
        else if (label[1] == '\0' && label[0] >= 'a' && label[0] <= 'z' && (shifted != m_CapsOn)) {
            upper[0] = (char)(label[0] - 'a' + 'A');
            label = upper;
        }
        else if (!m_HasArrows && asciiArrow(key.scancode) != nullptr) {
            label = asciiArrow(key.scancode);
        }
        drawText(surface, fontFor(label), label, k_Text, key.rect);
    }

    Session* session = Session::get();
    if (session != nullptr) {
        session->getOverlayManager().setKeyboardSurface(surface);
    }
    else {
        SDL_FreeSurface(surface);
    }
}

PcKeyboard::ModState PcKeyboard::modState(SDL_Scancode scancode) const
{
    return m_Mods.value(scancode, ModOff);
}

Uint16 PcKeyboard::heldModifiers() const
{
    Uint16 mod = KMOD_NONE;
    for (auto it = m_Mods.constBegin(); it != m_Mods.constEnd(); ++it) {
        if (it.value() != ModOff) {
            mod |= modifierFlag((SDL_Scancode)it.key());
        }
    }
    return mod;
}

void PcKeyboard::press(Key& key)
{
    if (isModifier(key.scancode)) {
        switch (modState(key.scancode)) {
        case ModOff:
            m_Mods[key.scancode] = ModOnce;
            m_Send(key.scancode, true, heldModifiers());
            break;
        case ModOnce:
            m_Mods[key.scancode] = ModLocked;
            break;
        case ModLocked:
            m_Mods[key.scancode] = ModOff;
            m_Send(key.scancode, false, heldModifiers());
            break;
        }
        return;
    }

    key.down = true;
    m_Send(key.scancode, true, heldModifiers());
}

void PcKeyboard::release(Key& key)
{
    if (isModifier(key.scancode) || !key.down) {
        return;
    }

    key.down = false;
    m_Send(key.scancode, false, heldModifiers());
    if (key.scancode == SDL_SCANCODE_CAPSLOCK) {
        m_CapsOn = !m_CapsOn;
    }

    // A modifier held for one key lets go after it
    for (auto it = m_Mods.begin(); it != m_Mods.end(); ++it) {
        if (it.value() == ModOnce) {
            it.value() = ModOff;
            m_Send((SDL_Scancode)it.key(), false, heldModifiers());
        }
    }
}

void PcKeyboard::releaseAll()
{
    for (Key& key : m_Keys) {
        if (key.down) {
            key.down = false;
            m_Send(key.scancode, false, heldModifiers());
        }
    }
    for (auto it = m_Mods.begin(); it != m_Mods.end(); ++it) {
        if (it.value() != ModOff) {
            it.value() = ModOff;
            m_Send((SDL_Scancode)it.key(), false, heldModifiers());
        }
    }
    m_Mods.clear();
}

int PcKeyboard::keyAt(int x, int y) const
{
    SDL_Point point = {x, y};
    if (SDL_PointInRect(&point, &m_HideButton)) {
        return TargetHide;
    }
    for (int i = 0; i < (int)m_Keys.size(); i++) {
        if (SDL_PointInRect(&point, &m_Keys[i].rect)) {
            return i;
        }
    }
    return TargetNone;
}

bool PcKeyboard::handleTouch(const SDL_TouchFingerEvent* event, float windowAspect)
{
    if (event->type == SDL_FINGERUP && m_SwallowedFingers.remove(event->fingerId)) {
        return true;
    }
    if (event->type != SDL_FINGERDOWN) {
        // A finger that landed on the keyboard stays the keyboard's until it lifts
        auto it = m_Fingers.find(event->fingerId);
        if (it == m_Fingers.end()) {
            return event->type == SDL_FINGERMOTION && m_SwallowedFingers.contains(event->fingerId);
        }
        if (event->type == SDL_FINGERUP) {
            int target = it.value();
            m_Fingers.erase(it);
            if (target == TargetHide) {
                hide();
            }
            else if (target >= 0) {
                release(m_Keys[target]);
                render();
            }
        }
        return true;
    }

    if (!m_Visible || windowAspect <= 0) {
        return false;
    }

    // The keyboard spans the window's width, so its share of the height follows the aspect
    float heightShare = (float)k_Height / k_Width * windowAspect;
    float top = 1.0f - heightShare;
    if (event->y < top) {
        return false;
    }

    int x = (int)(event->x * k_Width);
    int y = (int)((event->y - top) / heightShare * k_Height);
    int target = keyAt(x, y);
    m_Fingers.insert(event->fingerId, target);
    if (target >= 0) {
        press(m_Keys[target]);
        render();
    }
    return true;
}

bool PcKeyboard::observeToggleGesture(const SDL_TouchFingerEvent* event, bool usedByKeyboard)
{
    switch (event->type) {
    case SDL_FINGERDOWN:
        if (m_GestureFingers.isEmpty()) {
            m_GestureMaxFingers = 0;
            m_GestureOnKeyboard = false;
        }
        m_GestureFingers.insert(event->fingerId);
        m_GestureOnKeyboard |= usedByKeyboard;
        if (m_GestureFingers.size() > m_GestureMaxFingers) {
            m_GestureMaxFingers = m_GestureFingers.size();
            if (m_GestureMaxFingers == 3) {
                m_GestureThirdFingerTicks = event->timestamp;
            }
        }
        return false;

    case SDL_FINGERUP:
        m_GestureFingers.remove(event->fingerId);
        return m_GestureFingers.isEmpty() && m_GestureMaxFingers == 3 && !m_GestureOnKeyboard &&
               event->timestamp - m_GestureThirdFingerTicks <= k_TapMs;

    default:
        return false;
    }
}
