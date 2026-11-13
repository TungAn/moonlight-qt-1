#include "input.h"

#include <Limelight.h>
#include "SDL_compat.h"

#include <QtMath>

// How long the mouse button will be pressed for a tap to click gesture
#define TAP_BUTTON_RELEASE_DELAY 100

// How long the fingers must be stationary to start a drag
#define DRAG_ACTIVATION_DELAY 650

// How far the finger can move before it cancels a drag or tap
#define DEAD_ZONE_DELTA 0.01f

// Two-finger scroll: wheel units (120 = one notch) for dragging both fingers
// the full height of the window, so content roughly follows the fingers.
#define TOUCH_SCROLL_UNITS_PER_HEIGHT 2400.0f

// Flick momentum after the fingers lift: velocity is measured over the last
// TOUCH_FLICK_WINDOW_MS of movement, and a flick needs the fingers to still be
// moving within TOUCH_FLICK_MAX_IDLE_MS of lifting.
#define TOUCH_FLICK_WINDOW_MS 80
#define TOUCH_FLICK_MAX_IDLE_MS 60
#define TOUCH_FLICK_MIN_VELOCITY 0.25f  // wheel units per ms
#define TOUCH_FLICK_MAX_VELOCITY 25.0f
#define TOUCH_FLICK_TICK_MS 8
#define TOUCH_FLICK_FRICTION 0.965f     // per tick: coasts for about a second
#define TOUCH_FLICK_STOP_VELOCITY 0.03f
#define TOUCH_FLICK_MAX_TICKS 400

Uint32 SdlInputHandler::releaseLeftButtonTimerCallback(Uint32, void*)
{
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    return 0;
}

Uint32 SdlInputHandler::releaseRightButtonTimerCallback(Uint32, void*)
{
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
    return 0;
}

void SdlInputHandler::sendTouchScroll(float unitsX, float unitsY)
{
    // Send whole wheel units and keep the remainder, so slow scrolls aren't lost
    m_TouchScrollAccumX += unitsX;
    m_TouchScrollAccumY += unitsY;

    short sendX = static_cast<short>(qBound(-32000.0f, m_TouchScrollAccumX, 32000.0f));
    short sendY = static_cast<short>(qBound(-32000.0f, m_TouchScrollAccumY, 32000.0f));
    if (sendY != 0) {
        LiSendHighResScrollEvent(sendY);
        m_TouchScrollAccumY -= sendY;
    }
    if (sendX != 0) {
        LiSendHighResHScrollEvent(sendX);
        m_TouchScrollAccumX -= sendX;
    }
}

Uint32 SdlInputHandler::touchScrollMomentumCallback(Uint32 interval, void* param)
{
    auto me = reinterpret_cast<SdlInputHandler*>(param);

    if (!me->m_TouchScrollMomentumActive.load()) {
        return 0;
    }

    me->m_TouchScrollMomentumAccumX += me->m_TouchScrollVelX * TOUCH_FLICK_TICK_MS;
    me->m_TouchScrollMomentumAccumY += me->m_TouchScrollVelY * TOUCH_FLICK_TICK_MS;

    short sendX = static_cast<short>(me->m_TouchScrollMomentumAccumX);
    short sendY = static_cast<short>(me->m_TouchScrollMomentumAccumY);
    if (sendY != 0) {
        LiSendHighResScrollEvent(sendY);
        me->m_TouchScrollMomentumAccumY -= sendY;
    }
    if (sendX != 0) {
        LiSendHighResHScrollEvent(sendX);
        me->m_TouchScrollMomentumAccumX -= sendX;
    }

    me->m_TouchScrollVelX *= TOUCH_FLICK_FRICTION;
    me->m_TouchScrollVelY *= TOUCH_FLICK_FRICTION;

    if (++me->m_TouchScrollMomentumTicks >= TOUCH_FLICK_MAX_TICKS ||
            (qAbs(me->m_TouchScrollVelX) < TOUCH_FLICK_STOP_VELOCITY &&
             qAbs(me->m_TouchScrollVelY) < TOUCH_FLICK_STOP_VELOCITY)) {
        me->m_TouchScrollMomentumActive.store(false);
        return 0;
    }

    return interval;
}

void SdlInputHandler::startTouchScrollMomentum(Uint32 liftTimestamp)
{
    if (m_TouchScrollSampleCount < 2) {
        return;
    }

    auto sampleAt = [this](int age) -> const TouchScrollSample& {
        int index = (m_TouchScrollSampleHead - 1 - age + TOUCH_SCROLL_SAMPLES) % TOUCH_SCROLL_SAMPLES;
        return m_TouchScrollSamples[index];
    };

    const TouchScrollSample& newest = sampleAt(0);
    if (liftTimestamp - newest.timestamp > TOUCH_FLICK_MAX_IDLE_MS) {
        // The fingers stopped before lifting: no flick
        return;
    }

    // Oldest sample still inside the velocity window
    int oldestAge = 0;
    for (int age = 1; age < m_TouchScrollSampleCount; age++) {
        if (newest.timestamp - sampleAt(age).timestamp > TOUCH_FLICK_WINDOW_MS) {
            break;
        }
        oldestAge = age;
    }
    if (oldestAge == 0) {
        oldestAge = 1;
    }
    const TouchScrollSample& oldest = sampleAt(oldestAge);
    Uint32 elapsed = newest.timestamp - oldest.timestamp;
    if (elapsed == 0) {
        return;
    }

    float velX = (newest.totalX - oldest.totalX) / elapsed;
    float velY = (newest.totalY - oldest.totalY) / elapsed;
    if (qAbs(velX) < TOUCH_FLICK_MIN_VELOCITY && qAbs(velY) < TOUCH_FLICK_MIN_VELOCITY) {
        return;
    }

    // Coast along the main direction only, like a phone does
    if (qAbs(velX) < qAbs(velY) * 0.5f) {
        velX = 0;
    }
    else if (qAbs(velY) < qAbs(velX) * 0.5f) {
        velY = 0;
    }

    stopTouchScrollMomentum();
    m_TouchScrollVelX = qBound(-TOUCH_FLICK_MAX_VELOCITY, velX, TOUCH_FLICK_MAX_VELOCITY);
    m_TouchScrollVelY = qBound(-TOUCH_FLICK_MAX_VELOCITY, velY, TOUCH_FLICK_MAX_VELOCITY);
    m_TouchScrollMomentumAccumX = m_TouchScrollAccumX;
    m_TouchScrollMomentumAccumY = m_TouchScrollAccumY;
    m_TouchScrollMomentumTicks = 0;
    m_TouchScrollMomentumActive.store(true);
    m_TouchScrollMomentumTimer = SDL_AddTimer(TOUCH_FLICK_TICK_MS, touchScrollMomentumCallback, this);
}

void SdlInputHandler::stopTouchScrollMomentum()
{
    m_TouchScrollMomentumActive.store(false);
    if (m_TouchScrollMomentumTimer != 0) {
        SDL_RemoveTimer(m_TouchScrollMomentumTimer);
        m_TouchScrollMomentumTimer = 0;
    }
}

Uint32 SdlInputHandler::dragTimerCallback(Uint32, void *param)
{
    auto me = reinterpret_cast<SdlInputHandler*>(param);

    // Check how many fingers are down now to decide
    // which button to hold down
    if (me->m_NumFingersDown == 2) {
        me->m_DragButton = BUTTON_RIGHT;
    }
    else if (me->m_NumFingersDown == 1) {
        me->m_DragButton = BUTTON_LEFT;
    }

    LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, me->m_DragButton);

    return 0;
}

void SdlInputHandler::handleRelativeFingerEvent(SDL_TouchFingerEvent* event)
{
    int fingerIndex = -1;

    // A new touch stops any flick that is still coasting
    if (event->type == SDL_FINGERDOWN) {
        stopTouchScrollMomentum();
    }

    // Observations on Windows 10: x and y appear to be relative to 0,0 of the window client area.
    // Although SDL documentation states they are 0.0 - 1.0 float values, they can actually be higher
    // or lower than those values as touch events continue for touches started within the client area that
    // leave the client area during a drag motion.
    // dx and dy are deltas from the last touch event, not the first touch down.

    // Determine the index of this finger using our list
    // of fingers that are currently active on screen.
    // This is also required to handle finger up which
    // where the finger will not be in SDL_GetTouchFinger()
    // anymore.
    if (event->type != SDL_FINGERDOWN) {
        for (int i = 0; i < MAX_FINGERS; i++) {
            if (event->fingerId == m_TouchDownEvent[i].fingerId) {
                fingerIndex = i;
                break;
            }
        }
    }
    else {
        // Resolve the new finger by determining the ID of each
        // finger on the display.
        int numTouchFingers = SDL_GetNumTouchFingers(event->touchId);
        for (int i = 0; i < numTouchFingers; i++) {
            SDL_Finger* finger = SDL_GetTouchFinger(event->touchId, i);
            SDL_assert(finger != nullptr);
            if (finger != nullptr) {
                if (finger->id == event->fingerId) {
                    fingerIndex = i;
                    break;
                }
            }
        }
    }

    if (fingerIndex < 0 || fingerIndex >= MAX_FINGERS) {
        // Too many fingers
        return;
    }

    // Two fingers down without a held drag means a scroll (or a two-finger tap),
    // so the cursor stays put.
    bool twoFingerGesture = m_TouchScrolling || (m_NumFingersDown >= 2 && m_DragButton == 0);

    // Handle cursor motion based on the position of the
    // primary finger on screen
    if (fingerIndex == 0 && !twoFingerGesture) {
        // The event x and y values are relative to our window width
        // and height. However, we want to scale them to be relative
        // to the host resolution. Fortunately this is easy since we
        // already have normalized values. We'll just multiply them
        // by the stream dimensions to get real X and Y values rather
        // than the client window dimensions.
        short deltaX = static_cast<short>(event->dx * m_StreamWidth);
        short deltaY = static_cast<short>(event->dy * m_StreamHeight);
        if (deltaX != 0 || deltaY != 0) {
            LiSendMouseMoveEvent(deltaX, deltaY);
        }
    }

    // Start a drag timer when primary or secondary
    // fingers go down
    if (event->type == SDL_FINGERDOWN &&
            (fingerIndex == 0 || fingerIndex == 1)) {
        SDL_RemoveTimer(m_DragTimer);
        m_DragTimer = SDL_AddTimer(DRAG_ACTIVATION_DELAY,
                                   dragTimerCallback,
                                   this);
    }

    // Two-finger scroll: each of the two fingers reports its own motion, so
    // half of each event's motion is the average of the two.
    if (event->type == SDL_FINGERMOTION && m_NumFingersDown == 2 && m_DragButton == 0) {
        float aspect = m_StreamHeight > 0 ? (float)m_StreamWidth / m_StreamHeight : 1.0f;
        float unitsPerHeight = TOUCH_SCROLL_UNITS_PER_HEIGHT * m_TouchScrollScale;
        // Content follows the fingers: moving them down scrolls up (positive wheel),
        // moving them right scrolls left (negative horizontal wheel).
        float unitsY = event->dy * unitsPerHeight / 2;
        float unitsX = -event->dx * aspect * unitsPerHeight / 2;

        if (!m_TouchScrolling) {
            m_TouchScrollMoved += qSqrt(qPow(event->dx * aspect, 2) + qPow(event->dy, 2)) / 2;
            if (m_TouchScrollMoved > DEAD_ZONE_DELTA) {
                m_TouchScrolling = true;
                SDL_RemoveTimer(m_DragTimer);
                m_DragTimer = 0;
                // No two-finger tap (right click) after a scroll
                m_TouchDownEvent[0].timestamp = 0;
                m_TouchDownEvent[1].timestamp = 0;
                m_TouchScrollAccumX = m_TouchScrollAccumY = 0;
                m_TouchScrollTotalX = m_TouchScrollTotalY = 0;
                m_TouchScrollSampleCount = m_TouchScrollSampleHead = 0;
                if (!m_LoggedTouchScroll) {
                    m_LoggedTouchScroll = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "Trackpad two-finger scroll started (scale %.2f)",
                                m_TouchScrollScale);
                }
            }
        }

        if (m_TouchScrolling) {
            sendTouchScroll(unitsX, unitsY);

            m_TouchScrollTotalX += unitsX;
            m_TouchScrollTotalY += unitsY;
            TouchScrollSample& sample = m_TouchScrollSamples[m_TouchScrollSampleHead];
            sample.timestamp = static_cast<Uint32>(event->timestamp);
            sample.totalX = m_TouchScrollTotalX;
            sample.totalY = m_TouchScrollTotalY;
            m_TouchScrollSampleHead = (m_TouchScrollSampleHead + 1) % TOUCH_SCROLL_SAMPLES;
            if (m_TouchScrollSampleCount < TOUCH_SCROLL_SAMPLES) {
                m_TouchScrollSampleCount++;
            }
        }
    }

    if (event->type == SDL_FINGERMOTION) {
        // If it's outside the deadzone delta, cancel drags and taps
        if (qSqrt(qPow(event->x - m_TouchDownEvent[fingerIndex].x, 2) +
                  qPow(event->y - m_TouchDownEvent[fingerIndex].y, 2)) > DEAD_ZONE_DELTA) {
            SDL_RemoveTimer(m_DragTimer);
            m_DragTimer = 0;

            // This effectively cancels the tap logic below
            m_TouchDownEvent[fingerIndex].timestamp = 0;
        }
    }

    if (event->type == SDL_FINGERUP) {
        // Cancel the drag timer on finger up
        SDL_RemoveTimer(m_DragTimer);
        m_DragTimer = 0;

        // Release any drag
        if (m_DragButton != 0) {
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, m_DragButton);
            m_DragButton = 0;
        }
        // 2 finger tap
        else if (event->timestamp - m_TouchDownEvent[1].timestamp < 250) {
            // Zero timestamp of the primary finger to ensure we won't
            // generate a left click if the primary finger comes up soon.
            m_TouchDownEvent[0].timestamp = 0;

            // Press down the right mouse button
            LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);

            // Queue a timer to release it in 100 ms
            SDL_RemoveTimer(m_RightButtonReleaseTimer);
            m_RightButtonReleaseTimer = SDL_AddTimer(TAP_BUTTON_RELEASE_DELAY,
                                                     releaseRightButtonTimerCallback,
                                                     nullptr);
        }
        // 1 finger tap
        else if (event->timestamp - m_TouchDownEvent[0].timestamp < 250) {
            // Press down the left mouse button
            LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);

            // Queue a timer to release it in 100 ms
            SDL_RemoveTimer(m_LeftButtonReleaseTimer);
            m_LeftButtonReleaseTimer = SDL_AddTimer(TAP_BUTTON_RELEASE_DELAY,
                                                    releaseLeftButtonTimerCallback,
                                                    nullptr);
        }
    }

    m_NumFingersDown = SDL_GetNumTouchFingers(event->touchId);

    if (event->type == SDL_FINGERDOWN) {
        // A new gesture starts measuring from here
        m_TouchScrollMoved = 0;
    }
    else if (event->type == SDL_FINGERUP && m_NumFingersDown == 0 && m_TouchScrolling) {
        // Both fingers are up: coast if they were flicked
        startTouchScrollMomentum(static_cast<Uint32>(event->timestamp));
        m_TouchScrolling = false;
    }

    if (event->type == SDL_FINGERDOWN) {
        m_TouchDownEvent[fingerIndex] = *event;
    }
    else if (event->type == SDL_FINGERUP) {
        m_TouchDownEvent[fingerIndex] = {};
    }
}
