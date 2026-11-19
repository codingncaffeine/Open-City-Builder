// cRZWinKeyAcceleratorRes — the keyboard accelerator text resource (type 0xa2e3d533, iid 0x42e3ea4b; PC 1.1.641
// 0x009841fc..0x00984587, 17 functions; factory cGZWinKeyAcceleratorResFactory::CreateInstance 0x0098446e). The game keeps
// its keyboard shortcuts as text tables in group 0x6a231eaa: 0x6a9362ef the city view, 0x6a9362f0 the region view,
// 0x6a9362f1 the application ("general") and 0x0c0eaad0 player drive. SerialRead parses one table into an ordered map of
// key code -> message, and ApplyTo registers every binding with an accelerator, in key-code order.
// Line format: `<keys> = <type> [<data1> [<data2> [<data3>]]] ["description"]`. Keys are space separated: the modifiers
// control / shift / alt, `up` or `down` after a key for the key direction (down is the default), and one key name (a
// letter or digit, 0xNN, a decimal number, F1..F12, NumpadN, a named key, or a punctuation character).
// Key code: vk | key up << 28 | shift 0x10000 | control 0x20000 | alt 0x40000 (cRZWinKeyAccelerator's composite).
// Quirks kept: `down` after a key also makes the key the Down arrow (0x28); key names are matched by "contains", in a
// fixed order (so `numpadenter` is Return); a `"` anywhere in the message starts the comment; a binding over a key code
// already bound is dropped (the first line wins); a line holding only modifiers binds the first modifier as the key.
#pragma once
#include <cstdint>
#include <map>
#include <string>

#include "gz/WinInterfaces.h"

namespace gz {

constexpr std::uint32_t kGZIID_cIGZWinKeyAcceleratorRes = 0x42e3ea4b;
constexpr std::uint32_t kKeyAcceleratorResType = 0xa2e3d533, kKeyAcceleratorResGroup = 0x6a231eaa;
constexpr std::uint32_t kKeyAcceleratorCityView = 0x6a9362ef, kKeyAcceleratorRegionView = 0x6a9362f0,
                        kKeyAcceleratorGeneral = 0x6a9362f1, kKeyAcceleratorPlayerDrive = 0x0c0eaad0;

class cRZWinKeyAcceleratorRes {
public:
    // Parses the whole text (CR / LF separated lines); always true, like the original.   @ 0x00984587
    bool SerialRead(const char* text, std::size_t length);
    // Registers every binding with `accelerator` (RegisterAccelerator(code, message), slot 5), in key-code order.   @ 0x009842ee
    void ApplyTo(cIGZWinKeyAccelerator* accelerator) const;
    const std::map<std::uint32_t, cGZMessage>& Bindings() const { return bindings_; }

    // One line; false when the line is not a binding or its key code was already bound.
    bool ParseLine(std::string line);
    // The virtual key of one lower-case key-name token (0 = unknown).
    static std::uint32_t KeyFromName(const std::string& token);

private:
    std::map<std::uint32_t, cGZMessage> bindings_;   // +0x1c
};

}  // namespace gz
