// The game's keyboard shortcuts on the native build. The original keeps them as text tables in its data (type 0xa2e3d533,
// group 0x6a231eaa: the city view, the region view and the application's general table); each window that takes keys
// builds a cRZWinKeyAccelerator from its table, and a matched key runs its command through the framework command server
// (SL::spCommandServer, service 0xeb903a32), whose dispatcher sends the command as a message on message server 2 to the
// windows that listen for it (the city view, the region screen, the application window for the cheat code dialog).
#pragma once
#include <cstdint>

class cIGZWinKeyAccelerator;
class cIGZCommandServer;

namespace native {

// A new, initialised accelerator holding the bindings of table `instance` (gz::kKeyAccelerator*), one reference; an
// accelerator without bindings when the data has no such table.
cIGZWinKeyAccelerator* CreateGameKeyAccelerator(std::uint32_t instance);
// The framework command server (borrowed), or null before the framework is up.
cIGZCommandServer* GameCommandServer();
// SL::spCommandServer vslot 0x98 ExecuteCommand(command, null, null): the command's status (2 = nobody ran it).
std::uint32_t ExecuteGameCommand(std::uint32_t command);
// The GZ key modifier word of the keyboard right now: 1 shift, 2 control, 4 alt.
std::uint32_t KeyModifiers();

}  // namespace native
