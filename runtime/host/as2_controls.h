// Alien Shooter 2: Reloaded - the handheld's controls as the PC game's keyboard and mouse.
//
//   D-pad, left stick   W A S D (the game reads key positions: Z Q S D on an AZERTY keyboard)
//   right stick         mouse pointer (aim, menus)
//   R2 / L2             left click (fire, menus) / right click (move to the pointer)
//   R1 / L1             mouse wheel up / down (weapon change)
//   A B X Y             Space (leave a vehicle, skip a dialogue) / Enter / R (reload) / I (inventory)
//   L3 / R3             F (flashlight) / Tab (interface on/off)
//   Start               Escape (game menu)
// Select stays with main.cpp (Select + Start quits).
#pragma once
#include <SDL2/SDL.h>
#include <cmath>
#include <cstdlib>
#include "guest_runtime.h"

namespace as2 {

class Controls {
public:
    // DirectInput scan codes
    enum : unsigned { W = 0x11, A = 0x1e, S = 0x1f, D = 0x20, Space = 0x39, Enter = 0x1c, R = 0x13, I = 0x17,
                      F = 0x21, Tab = 0x0f, Escape = 0x01, BracketLeft = 0x1a, BracketRight = 0x1b };

    // A controller button; returns false for the ones left to main.cpp (Select).
    bool button(uint8_t b, bool down) {
        switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: dpad[0] = down; updateMove(); return true;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: dpad[1] = down; updateMove(); return true;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: dpad[2] = down; updateMove(); return true;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: dpad[3] = down; updateMove(); return true;
        case SDL_CONTROLLER_BUTTON_A: setGuestKey(Space, down); return true;
        case SDL_CONTROLLER_BUTTON_B: setGuestKey(Enter, down); return true;
        case SDL_CONTROLLER_BUTTON_X: setGuestKey(R, down); return true;
        case SDL_CONTROLLER_BUTTON_Y: setGuestKey(I, down); return true;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: if (down) turnGuestWheel(-1); return true;   // wheel down: previous weapon
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: if (down) turnGuestWheel(1); return true;   // wheel up: next weapon
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: setGuestKey(F, down); return true;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: setGuestKey(Tab, down); return true;
        case SDL_CONTROLLER_BUTTON_START: setGuestKey(Escape, down); return true;
        default: return false;
        }
    }

    // A controller axis: the left stick moves, the triggers click, the right stick is read every tick.
    void axis(uint8_t a, int16_t value) {
        switch (a) {
        case SDL_CONTROLLER_AXIS_LEFTX: stickX = value; updateMove(); break;
        case SDL_CONTROLLER_AXIS_LEFTY: stickY = value; updateMove(); break;
        case SDL_CONTROLLER_AXIS_RIGHTX: aimX = value; break;
        case SDL_CONTROLLER_AXIS_RIGHTY: aimY = value; break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: trigger(value, leftClick, [](bool d) { setGuestPointer(px(), py(), d ? 0 : 2); }); break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: trigger(value, rightClick, [](bool d) { setGuestRightButton(d); }); break;
        default: break;
        }
    }

    // Every pass of the main loop: the right stick moves the pointer at up to AS2_POINTER_SPEED (screen
    // widths per second, default 1.2), faster towards the stick's edge for both precise and quick aiming.
    void tick(float seconds) {
        static const float speed = [] { const char* e = std::getenv("AS2_POINTER_SPEED"); float v = e ? std::strtof(e, nullptr) : 0.f; return v > 0 ? v : 1.2f; }();
        // Twin-stick aim while playing (the game shows its crosshair): the stick's direction puts the pointer
        // around the middle of the view, where the camera keeps the player; released, the aim stays.
        // AS2_TWINSTICK=0 keeps the stick as a mouse everywhere.
        static const bool twinStick = [] { const char* e = std::getenv("AS2_TWINSTICK"); return !(e && e[0] == '0'); }();
        if (twinStick && guestCursorIsCrosshair()) {
            const float ax = aimX / 32767.f, ay = aimY / 32767.f, len = std::sqrt(ax * ax + ay * ay);
            if (len < kAimDeadzone / 32767.f) return;
            const float reach = 0.12f + 0.26f * std::fmin(1.f, (len - kAimDeadzone / 32767.f) / (1.f - kAimDeadzone / 32767.f));
            // the 800x600 view: the same reach in pixels across and down
            setGuestPointer(clamp01(0.5f + ax / len * reach * 0.75f), clamp01(0.5f + ay / len * reach), 1);
            return;
        }
        const float dx = shape(aimX), dy = shape(aimY);
        if (dx == 0 && dy == 0) return;
        float x, y;
        getGuestPointer(x, y);   // from where the game may have put it (SetCursorPos)
        x = clamp01(x + dx * speed * seconds);
        y = clamp01(y + dy * speed * seconds * (4.f / 3.f));   // the 800x600 view: same speed in pixels
        setGuestPointer(x, y, 1);
    }

    void releaseAll() {
        for (unsigned k : {W, A, S, D, Space, Enter, R, I, F, Tab, Escape, BracketLeft, BracketRight}) setGuestKey(k, false);
        if (leftClick) { leftClick = false; setGuestPointer(px(), py(), 2); }
        if (rightClick) { rightClick = false; setGuestRightButton(false); }
        dpad[0] = dpad[1] = dpad[2] = dpad[3] = false; stickX = stickY = aimX = aimY = 0;
    }

private:
    static constexpr int kMoveThreshold = 12000, kAimDeadzone = 4000, kTriggerOn = 12000, kTriggerOff = 6000;
    bool dpad[4] = {};   // up down left right
    int16_t stickX = 0, stickY = 0, aimX = 0, aimY = 0;
    bool leftClick = false, rightClick = false;
    bool keyW = false, keyS = false, keyA = false, keyD = false;

    static float px() { float x, y; getGuestPointer(x, y); return x; }
    static float py() { float x, y; getGuestPointer(x, y); return y; }
    static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
    // Dead zone, then a squared curve: small tilts for fine aim, full tilt for speed.
    static float shape(int16_t v) {
        const float a = std::fabs(float(v));
        if (a < kAimDeadzone) return 0;
        const float t = (a - kAimDeadzone) / (32767.f - kAimDeadzone);
        return (v < 0 ? -1.f : 1.f) * t * t;
    }
    template <class F> void trigger(int16_t value, bool& held, F send) {
        const bool now = held ? value > kTriggerOff : value > kTriggerOn;
        if (now != held) { held = now; send(now); }
    }
    void key(bool& held, bool want, unsigned scan) { if (held != want) { held = want; setGuestKey(scan, want); } }
    void updateMove() {
        key(keyW, dpad[0] || stickY < -kMoveThreshold, W);
        key(keyS, dpad[1] || stickY > kMoveThreshold, S);
        key(keyA, dpad[2] || stickX < -kMoveThreshold, A);
        key(keyD, dpad[3] || stickX > kMoveThreshold, D);
    }
};

}  // namespace as2
