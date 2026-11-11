// A game movie (EA MAD video + EA ADPCM audio, the streams of Intro.dat) decoded on the native application: NativeMovie.cpp.
// The original's player is platform code (QuickTime on the Mac, EA's own player on Windows); this one decodes through the system
// FFmpeg libraries when the build found them, and reports itself unavailable otherwise (the game then skips the intro).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace native {

class Movie {
public:
    static bool Available();   // false when the build has no movie decoder

    Movie();
    ~Movie();
    Movie(const Movie&) = delete;
    Movie& operator=(const Movie&) = delete;

    // Opens a whole movie stream held in memory (copied). False when it cannot be decoded.
    bool Open(std::vector<std::uint8_t> bytes);
    void Close();
    bool IsOpen() const;

    // Decodes up to `seconds` of play time; true when the picture changed. Finished() once the last frame was shown.
    bool AdvanceTo(double seconds);
    bool Finished() const;

    int Width() const;
    int Height() const;
    // The current picture as 32-bit pixels, bytes B, G, R, A (A = 0xff), rows of Width() pixels.
    const std::uint8_t* Pixels() const;

    // The sound decoded so far that has not been taken: interleaved signed 16-bit stereo at 22050 Hz.
    void TakeAudio(std::vector<std::int16_t>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The movie's sound on the default playback device (22050 Hz stereo, signed 16-bit), at `gain` (0..1). Only opened when the game's
// own audio runs (the interactive game, never a scenario or a shot).
class MovieSound {
public:
    MovieSound() = default;
    ~MovieSound();
    MovieSound(const MovieSound&) = delete;
    MovieSound& operator=(const MovieSound&) = delete;
    bool Open(float gain);
    void Push(const std::vector<std::int16_t>& samples);
    void Close();
private:
    void* stream_ = nullptr;   // SDL_AudioStream
};

}  // namespace native
