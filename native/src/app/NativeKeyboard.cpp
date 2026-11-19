#include "app/NativeKeyboard.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeUi.h"
#include "gz/CommandServer.h"
#include "gz/WinKeyAccelerator.h"
#include "gz/WinKeyAcceleratorRes.h"
#include "sc4/ServiceLocator.h"

namespace native {

cIGZWinKeyAccelerator* CreateGameKeyAccelerator(std::uint32_t instance) {
    auto* accelerator = new gz::cRZWinKeyAccelerator();
    accelerator->AddRef();
    accelerator->Init();
    std::vector<std::uint8_t> bytes;
    if (LoadResourceBytes(gz::kKeyAcceleratorResType, gz::kKeyAcceleratorResGroup, instance, bytes)) {
        gz::cRZWinKeyAcceleratorRes table;
        table.SerialRead(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        table.ApplyTo(accelerator);
        if (std::getenv("SC4_KEYDEBUG"))
            std::printf("keys: table %08x: %zu bindings\n", instance, table.Bindings().size());
    } else if (std::getenv("SC4_KEYDEBUG")) {
        std::printf("keys: table %08x: not in the data\n", instance);
    }
    return accelerator;
}

cIGZCommandServer* GameCommandServer() {
    return static_cast<cIGZCommandServer*>(SL::spCommandServer);   // the 0xcb90253a interface pointer
}

std::uint32_t ExecuteGameCommand(std::uint32_t command) {
    cIGZCommandServer* server = GameCommandServer();
    const std::uint32_t status = server ? server->ExecuteCommand(command, nullptr, nullptr) : 2;
    if (std::getenv("SC4_KEYDEBUG"))
        std::printf("keys: command %08x -> status %u (server %p, dispatcher %p)\n", command, status, static_cast<void*>(server),
                    server ? static_cast<void*>(server->GetCommandDispatcher()) : nullptr);
    return status;
}

std::uint32_t KeyModifiers() {
    const SDL_Keymod mod = SDL_GetModState();
    return ((mod & SDL_KMOD_SHIFT) ? 1u : 0u) | ((mod & SDL_KMOD_CTRL) ? 2u : 0u) | ((mod & SDL_KMOD_ALT) ? 4u : 0u);
}

}  // namespace native
