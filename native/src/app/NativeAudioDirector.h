// The game's audio on the native application (see NativeAudioDirector.cpp).
#pragma once
#include <string>

class cGZFramework;
class cSC4App;

namespace native {

// Before the application's PreFrameWorkInit: opens the platform's sound output and puts the game's audio director behind
// cSC4App::AddCOMDirectors (which adds it when the player's audio option is on). `dataDirectory` is GameData/ with the
// trailing slash. Without a sound output nothing is installed and the game runs silent.
void InstallAudioDirector(cSC4App& app, const std::string& dataDirectory);
// After the application's PreFrameWorkInit, where the framework's PreAppInit runs: starts the sound services the directors
// added and runs the directors' PreAppInit. Does nothing when the director was not installed or not added.
void AudioPreAppInit(cGZFramework& framework);
// After the application's PostFrameWorkInit (and the exemplar factories): the directors' PostAppInit, i.e. the sound
// configuration and the game's own sound services. True when they all started.
bool AudioPostAppInit(cGZFramework& framework);
// One frame: the framework's tick of the radio, the soundscape manager and the ambience manager.
void TickAudio();
// The directors' PreAppShutdown, the sound services' Shutdown and the sound output's end (before the platform's audio closes).
void AudioPreAppShutdown();
bool AudioRunning();   // the sound services are up
void NativeAudioReport();   // kit command `audio`: the radio, the sound manager and the listener

}  // namespace native
