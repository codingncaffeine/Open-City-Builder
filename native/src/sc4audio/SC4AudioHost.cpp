#include "sc4audio/SC4AudioHost.h"

namespace SC4Audio {
namespace {
cIMasterVolume* g_soundManager = nullptr;
cIViewport* g_viewport = nullptr;
cIViewport* (*g_viewportProvider)() = nullptr;
float g_zoomDistances[5] = {};
}  // namespace

cIMasterVolume* SoundManager() { return g_soundManager; }
void SetSoundManager(cIMasterVolume* manager) { g_soundManager = manager; }
cIViewport* ViewUtilities() { return g_viewport ? g_viewport : g_viewportProvider ? g_viewportProvider() : nullptr; }
void SetViewUtilitiesProvider(cIViewport* (*provider)()) { g_viewportProvider = provider; }
void SetViewUtilities(cIViewport* viewport) { g_viewport = viewport; }
float* ZoomDistances() { return g_zoomDistances; }

Services& GetServices() {
    static Services services;
    return services;
}

}  // namespace SC4Audio
