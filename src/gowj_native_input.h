// Native keyboard and mouse path: key events are handed to the engine's own input layer
// (UPlayerInput.InputKey / InputAxis) instead of being faked as a gamepad.
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace rex::glue {

// Starts the low-priority name-table scan (once). base = guest memory base.
void NativeInputStartNameScan(uint8_t* base);

// Called on the game thread from the per-frame look hook (ctx = that call's context).
// Delivers queued key transitions to the engine's UPlayerInput::InputKey.
void NativeInputPump(struct ::PPCContext& ctx, uint8_t* base, uint32_t player_input);

}  // namespace rex::glue

extern "C" bool GowjNativeKeysDriving();        // the pump ran within 400 ms (native bindings drive gameplay)
extern "C" bool GowjNativePreferKbm();          // keyboard/mouse was the last-used device
extern "C" bool GowjNativeKeysLive();
extern "C" void GowjNativeClearGamepadFlag(uint8_t* base, uint32_t self);  // keep bUsingGamepad off while on keyboard+mouse           // Driving && PreferKbm
extern "C" void GowjNativeNoteKbmActivity();    // mouse moved / button / key (called by the driver)
extern "C" void GowjNativeSetActive(bool active);  // window focused + pointer captured
extern "C" void GowjNativeNoteWheel(int direction);  // one wheel notch (+up / -down)
