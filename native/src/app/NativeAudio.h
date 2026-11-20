// The game's sound ids on the native application: every `PlaySound(id)` of the recovered UI and tools ends here.
#pragma once
#include <cstdint>

namespace native {

// The sound system as the game's callers use it (SL::spSS / RZ::SS()): whether it is there, an event queued for its handlers
// (vslot 0x50, PostEventIds) and one dispatched at once (vslot 0x30, DoEventIds). The game's sound manager handles the
// sound events: 0xea451903 or 1 start (sound id, instance id), 0x6a451908 stop (instance id), 0xca45190f position
// (instance id, x, y, z). Nothing happens without the sound services.
bool SoundSystemAvailable();
void PostSoundSystemEvent(std::uint32_t event, std::uint32_t a, std::uint32_t b = 0, std::uint32_t c = 0, std::uint32_t d = 0);
void DoSoundSystemEvent(std::uint32_t event, std::uint32_t a, std::uint32_t b = 0, std::uint32_t c = 0, std::uint32_t d = 0);
// An event made with its data and posted (vslot 0x70 EventCreateWithData, then 0x48 PostEvent): the disaster tools' messages.
void PostSoundSystemEventObject(std::uint32_t event, std::uint32_t a, std::uint32_t b, std::uint32_t c);
// cSC4AudioSoundManager::GenerateNewInstanceId(): a new instance id for a sound a caller will move or stop.
std::uint32_t NewSoundInstanceId();
// Starts the sound `soundId` (a Hit track id of Sound.dat) the way most of the game's callers do: the start-sound event
// posted to the sound system (PostEventIds(1, id, 0, 0, 0)).
void PlayGameSound(std::uint32_t soundId);
// The effects volume as a gain 0..1 (0 when the player's sound is off or there is no sound output): the intro movies' sound.
float EffectsGain();
void InstallButtonSounds();                   // the window buttons' sound service

}  // namespace native
