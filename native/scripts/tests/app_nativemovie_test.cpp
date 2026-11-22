// The movie stream preparation (PrepareMovieChunks): end-of-audio chunks are dropped so the video after them reaches the decoder,
// the frame delay is the first MAD frame's 16.16 milliseconds, and a stream that is not a clean run of chunks is left alone.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/NativeMovie.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
void Chunk(std::vector<std::uint8_t>& out, const char* tag, std::vector<std::uint8_t> payload) {
    const std::uint32_t size = static_cast<std::uint32_t>(payload.size() + 8);
    out.insert(out.end(), tag, tag + 4);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(size >> (8 * i)));
    out.insert(out.end(), payload.begin(), payload.end());
}
// A MAD frame payload as the intro clips have it: 4 bytes, the delay, width 512, height 384, then picture bytes.
std::vector<std::uint8_t> Frame(std::uint32_t delay, std::uint8_t fill) {
    std::vector<std::uint8_t> p = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) p.push_back(static_cast<std::uint8_t>(delay >> (8 * i)));
    p.insert(p.end(), {0x00, 0x02, 0x80, 0x01, fill, fill});
    return p;
}
std::string Tags(const std::vector<std::uint8_t>& bytes) {
    std::string tags;
    for (std::size_t at = 0; at + 8 <= bytes.size();) {
        std::uint32_t size = 0;
        std::memcpy(&size, bytes.data() + at + 4, 4);
        tags.append(reinterpret_cast<const char*>(bytes.data() + at), 4).push_back(' ');
        at += size;
    }
    return tags;
}
}  // namespace

int main() {
    {   // the intro clips' shape: header chunks, frames with sound, the end of the sound, frames without
        std::vector<std::uint8_t> s;
        Chunk(s, "MADk", Frame(0x0042aaab, 1));
        Chunk(s, "SCHl", {1, 2, 3, 4});
        Chunk(s, "SCDl", {5, 6});
        Chunk(s, "MADm", Frame(0x0042aaab, 2));
        Chunk(s, "SCEl", {});
        Chunk(s, "MADk", Frame(0x0042aaab, 3));
        Chunk(s, "MADm", Frame(0x0042aaab, 4));
        const double seconds = native::PrepareMovieChunks(s);
        CHECK(Tags(s) == "MADk SCHl SCDl MADm MADk MADm ");
        CHECK(std::fabs(seconds - 0.0666667) < 1e-6);   // 15 frames a second, not the demuxer's 66 ms
        CHECK(s.size() == 4 * 22 + 12 + 10);
        CHECK(s.back() == 4);                            // the last frame's picture bytes made it through
    }
    {   // the first MAD frame decides the delay
        std::vector<std::uint8_t> s;
        Chunk(s, "SCHl", {1});
        Chunk(s, "MADk", Frame(0x00640000, 1));          // 100 ms
        Chunk(s, "MADm", Frame(0x0042aaab, 2));
        CHECK(std::fabs(native::PrepareMovieChunks(s) - 0.1) < 1e-9);
        CHECK(Tags(s) == "SCHl MADk MADm ");
    }
    {   // not a clean run of chunks (a size past the end): the stream is left as it was
        std::vector<std::uint8_t> s;
        Chunk(s, "MADk", Frame(0x0042aaab, 1));
        Chunk(s, "SCEl", {});
        Chunk(s, "MADm", Frame(0x0042aaab, 2));
        s[s.size() - 22 + 4] = 0xff;                     // the last chunk claims 255 bytes
        const std::vector<std::uint8_t> before = s;
        native::PrepareMovieChunks(s);
        CHECK(s == before);
    }
    {   // no MAD frame: no delay, nothing else touched
        std::vector<std::uint8_t> s;
        Chunk(s, "SCHl", {1, 2});
        CHECK(native::PrepareMovieChunks(s) == 0.0 && Tags(s) == "SCHl ");
    }
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("app_nativemovie: all checks passed\n");
    return failures ? 1 : 0;
}
