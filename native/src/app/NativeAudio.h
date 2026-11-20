// The game's sound ids on the native application: every `PlaySound(id)` of the recovered UI and tools ends here.
#pragma once
#include <cstdint>

namespace native {

// Starts the sound `soundId` (a Hit track id of Sound.dat) the way the game's own callers do: the start-sound event of
// the game's sound manager, posted to the sound system (SL::spSS PostEventIds(1, id, 0, 0, 0)). Silent without the sound
// services.
void PlayGameSound(std::uint32_t soundId);
// The effects volume as a gain 0..1 (0 when the player's sound is off or there is no sound output): the intro movies' sound.
float EffectsGain();
void InstallButtonSounds();                   // the window buttons' sound service

}  // namespace native
