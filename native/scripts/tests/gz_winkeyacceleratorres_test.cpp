#include <cstdio>
#include <cstring>

#include "gz/WinKeyAccelerator.h"
#include "gz/WinKeyAcceleratorRes.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace gz;

namespace {
bool Bound(const cRZWinKeyAcceleratorRes& res, std::uint32_t code, std::uint32_t type, std::uint32_t d1 = 0, std::uint32_t d2 = 0,
           std::uint32_t d3 = 0) {
    auto it = res.Bindings().find(code);
    return it != res.Bindings().end() && it->second.type == type && it->second.param1 == d1 && it->second.param2 == d2 &&
           it->second.param3 == d3;
}
}  // namespace

int main() {
    // The cases of the format: modifiers, key up, extra values, a lone modifier, the "down" quirk, '=' as a key.
    {
        cRZWinKeyAcceleratorRes res;
        CHECK(res.ParseLine("control shift s = 0x8A2F0E1C 0x1 \"save\""));
        CHECK(Bound(res, 0x00030053, 0x8a2f0e1c, 1));
        CHECK(res.ParseLine("f5 up = 0x1234 0x5 0x6 0x7 0x8"));
        CHECK(Bound(res, 0x10000074, 0x1234, 5, 6, 7));
        CHECK(res.ParseLine("alt = 0x99"));
        CHECK(Bound(res, 0x12, 0x99));
        CHECK(res.ParseLine("s down = 0x5"));
        CHECK(Bound(res, 0x28, 0x5));
        CHECK(!res.Bindings().count(0x53));
        CHECK(res.ParseLine("control = = 0x77"));
        CHECK(Bound(res, 0x200bb, 0x77));
        CHECK(!res.ParseLine("Down = 0x88"));        // 0x28 is taken: the first line wins
        CHECK(Bound(res, 0x28, 0x5));
        CHECK(!res.ParseLine("; F1 = 0x1"));
        CHECK(!res.ParseLine("// F1 = 0x1"));
        CHECK(!res.ParseLine("F1 = 12"));             // no 0x
        CHECK(!res.ParseLine("F1 0x1"));              // no '='
        CHECK(!res.ParseLine("blorp = 0x1"));         // no key, no modifier
        CHECK(res.ParseLine("F2 = 0x6a935ca4 \"Mayor 0x99\""));   // the quote ends the values
        CHECK(Bound(res, 0x71, 0x6a935ca4));
    }
    // Key names, as the game's own tables spell them.
    {
        struct { const char* keys; std::uint32_t code; } names[] = {
            {"Control X", 0x20058}, {"Control Shift Alt X", 0x70058}, {"Control Break", 0x20003}, {"Control `", 0x200c0},
            {"Numpad+", 0x6b}, {"+", 0xbb}, {"-", 0xbd}, {"Numpad-", 0x6d}, {"6", 0x36}, {"PageUp", 0x21}, {"PageDown", 0x22},
            {"Home", 0x24}, {"End", 0x23}, {"Left up", 0x10000025}, {"Control Up", 0x20026}, {"Space", 0x20}, {"?", 0xbf},
            {"Alt ?", 0x400bf}, {"Escape", 0x1b}, {"F11", 0x7a}, {"F12", 0x7b}, {"Numlock", 0x90}, {"Control /", 0x200bf},
            {"Control Shift Alt Pause", 0x70013}, {"Control Shift Alt F11", 0x7007a}, {"Numpad8", 0x68}, {"Return", 0x0d},
            {"numpadenter", 0x0d}, {"Backspace", 0x08}, {".", 0xbe}, {",", 0xbc}, {"0xFFFF", 0xffff}, {"75", 0x4b},
            {"Shift", 0x10}, {"Semicolon", 0xba}, {"[", 0xdb}, {"\\", 0xdc}, {"'", 0xde}, {"Insert", 0x2d}, {"Delete", 0x2e},
        };
        for (const auto& n : names) {
            cRZWinKeyAcceleratorRes res;
            const bool parsed = res.ParseLine(std::string(n.keys) + "   = 0x1234");
            if (!parsed || !Bound(res, n.code, 0x1234)) {
                std::printf("FAIL key \"%s\": expected %08x, got %08x\n", n.keys, n.code,
                            res.Bindings().empty() ? 0u : res.Bindings().begin()->first);
                ++failures;
            }
        }
    }
    // A whole table (CR LF lines, comments, blank lines) applied to an accelerator: the keys find their commands.
    {
        const char* table =
            ";;;;;;;;;;;;;;;;\r\n; SimCity 4 keyboard accelerators -- General View\r\n;\r\n\r\n"
            "Control X               = 0x2a9496c9   \"Open Cheat Code Dialog\"\r\n"
            "Control Shift Alt X     = 0x2a9496c9   \"Open Cheat Code Dialog\"\r\n"
            "Control M               = 0xec3e82f8   0x8bd88057 \"Build Passenger Ferry Terminal\"\r\n"
            "Left up                 = 0x2a948275   \"Scroll left stop\"";   // no line end at the end of the text
        cRZWinKeyAcceleratorRes res;
        CHECK(res.SerialRead(table, std::strlen(table)));
        CHECK(res.Bindings().size() == 4);
        auto* accelerator = new cRZWinKeyAccelerator();
        accelerator->AddRef();
        res.ApplyTo(accelerator);
        std::uint32_t command = 0;
        CHECK(accelerator->TestForAccelerator(0x58, 2, true, command) && command == 0x2a9496c9);          // Ctrl+X
        CHECK(accelerator->TestForAccelerator(0x58, 7, true, command) && command == 0x2a9496c9);          // Ctrl+Shift+Alt+X
        CHECK(!accelerator->TestForAccelerator(0x58, 0, true, command));                                  // plain X
        CHECK(!accelerator->TestForAccelerator(0x58, 2, false, command));                                 // Ctrl+X going up
        CHECK(accelerator->TestForAccelerator(0x58, 2 | 0x70, true, command) && command == 0x2a9496c9);   // lock bits ignored
        cGZMessage message;
        CHECK(accelerator->TestForAccelerator(0x4d, 2, true, message) && message.type == 0xec3e82f8 && message.param1 == 0x8bd88057);
        CHECK(accelerator->TestForAccelerator(0x25, 0, false, command) && command == 0x2a948275);         // Left released
        accelerator->Release();
    }
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("gz_winkeyacceleratorres: all checks passed\n");
    return failures ? 1 : 0;
}
