// Engine key names (EKeys as this build's engine spells them) with the Windows virtual key polled
// for each. Shared by the binding editor (gowj_coalesced.cpp) and the native input pump.
// Escape and the console key are deliberately absent: Escape stays the pad Start button (pause),
// and the tilde console is not offered.
#pragma once

#include <cstddef>
#include <cstring>

namespace rex::glue {

struct KeyName {
  const char* name;
  int vk;
};

// Windows virtual-key constants are written out so this header does not need <windows.h>.
inline constexpr KeyName kKeyTable[] = {
    {"A", 'A'}, {"B", 'B'}, {"C", 'C'}, {"D", 'D'}, {"E", 'E'}, {"F", 'F'}, {"G", 'G'},
    {"H", 'H'}, {"I", 'I'}, {"J", 'J'}, {"K", 'K'}, {"L", 'L'}, {"M", 'M'}, {"N", 'N'},
    {"O", 'O'}, {"P", 'P'}, {"Q", 'Q'}, {"R", 'R'}, {"S", 'S'}, {"T", 'T'}, {"U", 'U'},
    {"V", 'V'}, {"W", 'W'}, {"X", 'X'}, {"Y", 'Y'}, {"Z", 'Z'},
    {"One", '1'}, {"Two", '2'}, {"Three", '3'}, {"Four", '4'}, {"Five", '5'}, {"Six", '6'},
    {"Seven", '7'}, {"Eight", '8'}, {"Nine", '9'}, {"Zero", '0'},
    {"F1", 0x70}, {"F2", 0x71}, {"F3", 0x72}, {"F4", 0x73}, {"F5", 0x74}, {"F6", 0x75},
    {"F7", 0x76}, {"F8", 0x77}, {"F9", 0x78}, {"F10", 0x79}, {"F11", 0x7A}, {"F12", 0x7B},
    {"SpaceBar", 0x20}, {"LeftShift", 0xA0}, {"RightShift", 0xA1}, {"LeftControl", 0xA2},
    {"RightControl", 0xA3}, {"LeftAlt", 0xA4}, {"RightAlt", 0xA5}, {"Tab", 0x09},
    {"Enter", 0x0D}, {"BackSpace", 0x08}, {"CapsLock", 0x14}, {"Pause", 0x13},
    {"Up", 0x26}, {"Down", 0x28}, {"Left", 0x25}, {"Right", 0x27},
    {"Insert", 0x2D}, {"Delete", 0x2E}, {"Home", 0x24}, {"End", 0x23}, {"PageUp", 0x21},
    {"PageDown", 0x22},
    {"LeftMouseButton", 0x01}, {"RightMouseButton", 0x02}, {"MiddleMouseButton", 0x04},
    {"ThumbMouseButton", 0x05}, {"ThumbMouseButton2", 0x06},
    {"Slash", 0xBF}, {"Backslash", 0xDC}, {"Comma", 0xBC}, {"Period", 0xBE}, {"Semicolon", 0xBA},
    {"Quote", 0xDE}, {"LeftBracket", 0xDB}, {"RightBracket", 0xDD}, {"Equals", 0xBB},
    {"Underscore", 0xBD},
    {"NumPadZero", 0x60}, {"NumPadOne", 0x61}, {"NumPadTwo", 0x62}, {"NumPadThree", 0x63},
    {"NumPadFour", 0x64}, {"NumPadFive", 0x65}, {"NumPadSix", 0x66}, {"NumPadSeven", 0x67},
    {"NumPadEight", 0x68}, {"NumPadNine", 0x69}, {"Multiply", 0x6A}, {"Add", 0x6B},
    {"Subtract", 0x6D}, {"Decimal", 0x6E}, {"Divide", 0x6F},
};
inline constexpr size_t kKeyTableSize = sizeof(kKeyTable) / sizeof(kKeyTable[0]);

// The wheel has no virtual key; the driver turns notches into these two key names.
inline constexpr const char* kWheelUp = "MouseScrollUp";
inline constexpr const char* kWheelDown = "MouseScrollDown";

inline bool IsKnownKeyName(const char* n) {
  if (!std::strcmp(n, kWheelUp) || !std::strcmp(n, kWheelDown)) return true;
  for (const KeyName& k : kKeyTable)
    if (!std::strcmp(k.name, n)) return true;
  return false;
}

}  // namespace rex::glue
