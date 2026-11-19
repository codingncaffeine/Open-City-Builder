// The image codecs behind cGZGimexReader / cGZGimexWriter. The original links EA's Gimex library; that library is
// third-party code outside the specs (gimex.md: "FreeCity4 uses its own decoders and encoders for the formats it
// needs"), so the port answers the IGimexLibrary calls with libpng for PNG and a small reader / writer for BMP.
//
// Pixels cross the seam as Gimex 32-bit frames: little-endian A8R8G8B8 dwords (bytes B, G, R, A), rows top down, the
// row pitch given by the caller. The save description's alpha bits (+0x438) decide whether a PNG keeps its alpha.
#include "app/NativeGimex.h"

#include <png.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "gz/GimexReaderWriter.h"
#include "gz/cIGZFile.h"
#include "rz/COMDirectors.h"

namespace native {
namespace {

enum Module : int { kNone = 0, kPng = 1, kBmp = 2 };

bool ReadAll(cIGZFile* file, std::vector<std::uint8_t>& out) {
    if (!file || !file->IsOpen()) return false;
    const std::uint32_t length = file->Length();
    if (length == 0xffffffffu || length == 0) return false;
    out.resize(length);
    file->SeekToBegin();
    return file->Read(out.data(), length);
}

// What a stream holds: a decoded image (reading) or the file to write.
struct Stream {
    int module = kNone;
    int width = 0, height = 0;
    std::vector<std::uint8_t> bgra;   // decoded, top down, width * 4 per row
    cIGZFile* output = nullptr;
    bool wrote = false;
};

std::uint32_t Le32(const std::uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<std::uint32_t>(p[3]) << 24; }
std::uint16_t Le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

bool DecodePng(const std::vector<std::uint8_t>& data, Stream& s) {
    png_image image;
    std::memset(&image, 0, sizeof image);
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, data.data(), data.size())) return false;
    image.format = PNG_FORMAT_BGRA;
    s.width = static_cast<int>(image.width);
    s.height = static_cast<int>(image.height);
    s.bgra.resize(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, s.bgra.data(), 0, nullptr)) {
        png_image_free(&image);
        return false;
    }
    return true;
}

// Uncompressed BMP: 8-bit (palette), 24-bit and 32-bit, bottom-up or top-down.
bool DecodeBmp(const std::vector<std::uint8_t>& data, Stream& s) {
    if (data.size() < 54 || data[0] != 'B' || data[1] != 'M') return false;
    const std::uint32_t offset = Le32(&data[10]);
    const std::uint32_t headerSize = Le32(&data[14]);
    const std::int32_t width = static_cast<std::int32_t>(Le32(&data[18]));
    std::int32_t height = static_cast<std::int32_t>(Le32(&data[22]));
    const std::uint16_t bits = Le16(&data[28]);
    const std::uint32_t compression = Le32(&data[30]);
    if (width <= 0 || height == 0 || (compression != 0 && compression != 3) || (bits != 8 && bits != 24 && bits != 32)) return false;
    const bool topDown = height < 0;
    if (topDown) height = -height;
    const std::size_t pitch = (static_cast<std::size_t>(width) * bits / 8 + 3) & ~std::size_t{3};
    if (offset + pitch * static_cast<std::size_t>(height) > data.size()) return false;
    const std::uint8_t* palette = data.data() + 14 + headerSize;
    std::uint32_t colors = Le32(&data[46]);
    if (bits == 8 && colors == 0) colors = 256;
    if (bits == 8 && 14 + headerSize + colors * 4 > data.size()) return false;
    s.width = width;
    s.height = height;
    s.bgra.assign(static_cast<std::size_t>(width) * height * 4, 0);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = data.data() + offset + pitch * static_cast<std::size_t>(topDown ? y : height - 1 - y);
        std::uint8_t* out = s.bgra.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x, out += 4) {
            if (bits == 8) {
                const std::uint32_t index = row[x] < colors ? row[x] : 0;
                std::memcpy(out, palette + index * 4, 3);
                out[3] = 0xff;
            } else if (bits == 24) {
                std::memcpy(out, row + x * 3, 3);
                out[3] = 0xff;
            } else {
                std::memcpy(out, row + x * 4, 4);
                out[3] = 0xff;   // BI_RGB 32-bit: the fourth byte is unused
            }
        }
    }
    return true;
}

bool EncodePng(const Stream& s, const std::uint8_t* pixels, std::size_t pitch, bool alpha, std::vector<std::uint8_t>& out) {
    png_image image;
    std::memset(&image, 0, sizeof image);
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(s.width);
    image.height = static_cast<png_uint_32>(s.height);
    std::vector<std::uint8_t> rows;
    const void* source = pixels;
    png_int_32 stride = static_cast<png_int_32>(pitch);
    if (alpha) {
        image.format = PNG_FORMAT_BGRA;
    } else {
        image.format = PNG_FORMAT_BGR;
        rows.resize(static_cast<std::size_t>(s.width) * s.height * 3);
        for (int y = 0; y < s.height; ++y)
            for (int x = 0; x < s.width; ++x)
                std::memcpy(&rows[(static_cast<std::size_t>(y) * s.width + x) * 3], pixels + pitch * y + x * 4, 3);
        source = rows.data();
        stride = s.width * 3;
    }
    png_alloc_size_t size = 0;
    if (!png_image_write_to_memory(&image, nullptr, &size, 0, source, stride, nullptr)) return false;
    out.resize(size);
    if (!png_image_write_to_memory(&image, out.data(), &size, 0, source, stride, nullptr)) return false;
    out.resize(size);
    return true;
}

void Put16(std::vector<std::uint8_t>& v, std::uint16_t x) { v.push_back(static_cast<std::uint8_t>(x)); v.push_back(static_cast<std::uint8_t>(x >> 8)); }
void Put32(std::vector<std::uint8_t>& v, std::uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i))); }

// A 24-bit bottom-up BMP.
void EncodeBmp(const Stream& s, const std::uint8_t* pixels, std::size_t pitch, std::vector<std::uint8_t>& out) {
    const std::size_t rowBytes = (static_cast<std::size_t>(s.width) * 3 + 3) & ~std::size_t{3};
    const std::uint32_t imageBytes = static_cast<std::uint32_t>(rowBytes * s.height);
    out.clear();
    out.push_back('B');
    out.push_back('M');
    Put32(out, 54 + imageBytes);
    Put32(out, 0);
    Put32(out, 54);
    Put32(out, 40);
    Put32(out, static_cast<std::uint32_t>(s.width));
    Put32(out, static_cast<std::uint32_t>(s.height));
    Put16(out, 1);
    Put16(out, 24);
    Put32(out, 0);
    Put32(out, imageBytes);
    Put32(out, 2835);
    Put32(out, 2835);
    Put32(out, 0);
    Put32(out, 0);
    for (int y = s.height - 1; y >= 0; --y) {
        const std::size_t start = out.size();
        for (int x = 0; x < s.width; ++x) out.insert(out.end(), pixels + pitch * y + x * 4, pixels + pitch * y + x * 4 + 3);
        out.resize(start + rowBytes, 0);
    }
}

class Codecs final : public IGimexLibrary {
public:
    int Find(const char* extension) override {
        std::string e = extension ? extension : "";
        if (!e.empty() && e[0] == '.') e.erase(0, 1);
        std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const int module = e == "png" ? kPng : e == "bmp" ? kBmp : kNone;
        if (module != kNone) current_ = module;
        return module;
    }
    int Get() override { return current_; }
    void Set(int module) override { current_ = module; }
    int Is(cIGZFile* file) override {
        if (!file || !file->IsOpen()) return 0;
        const int position = file->Position();
        std::uint8_t head[8] = {};
        file->SeekToBegin();
        const bool read = file->Read(head, sizeof head);
        file->SeekToPosition(position < 0 ? 0u : static_cast<std::uint32_t>(position));
        if (!read) return 0;
        if (current_ == kPng && png_sig_cmp(head, 0, 8) == 0) return 100;
        if (current_ == kBmp && head[0] == 'B' && head[1] == 'M') return 100;
        return 0;
    }
    std::uint32_t Open(void** stream, cIGZFile* file) override {
        *stream = nullptr;
        std::vector<std::uint8_t> data;
        if (!ReadAll(file, data)) return 0;
        auto s = std::make_unique<Stream>();
        s->module = current_;
        const bool ok = current_ == kPng ? DecodePng(data, *s) : current_ == kBmp ? DecodeBmp(data, *s) : false;
        if (!ok) return 0;
        *stream = s.release();
        return 1;
    }
    bool Close(void* stream) override {
        delete static_cast<Stream*>(stream);
        return true;
    }
    cGZGimexInfo* Info(void* stream, std::uint32_t index) override {
        auto* s = static_cast<Stream*>(stream);
        if (!s || index != 0 || s->bgra.empty()) return nullptr;
        auto* info = new cGZGimexInfo;
        std::memset(info, 0, sizeof *info);
        info->width = s->width;
        info->height = s->height;
        info->bitsPerPixel = 32;
        return info;
    }
    int Read(void* stream, cGZGimexInfo* info, void* pixels, std::uint32_t pitch) override {
        auto* s = static_cast<Stream*>(stream);
        if (!s || !info || !pixels || s->bgra.empty()) return 0;
        const std::size_t row = static_cast<std::size_t>(s->width) * 4;
        if (pitch < row) return 0;
        for (int y = 0; y < s->height; ++y)
            std::memcpy(static_cast<std::uint8_t*>(pixels) + static_cast<std::size_t>(pitch) * y, s->bgra.data() + row * y, row);
        return 1;
    }
    bool WriteOpen(void** stream, cIGZFile* file, std::uint32_t) override {
        *stream = nullptr;
        if (!file || !file->IsOpen() || (current_ != kPng && current_ != kBmp)) return false;
        auto* s = new Stream;
        s->module = current_;
        s->output = file;
        *stream = s;
        return true;
    }
    int Write(void* stream, const cGZGimexInfo* info, const void* pixels, std::uint32_t pitch) override {
        auto* s = static_cast<Stream*>(stream);
        if (!s || !info || !pixels || info->bitsPerPixel != 32 || info->width <= 0 || info->height <= 0) return 0;
        s->width = info->width;
        s->height = info->height;
        std::uint32_t alphaBits = 0;
        std::memcpy(&alphaBits, reinterpret_cast<const std::uint8_t*>(info) + kGimexInfoAlphaBits, 4);
        std::vector<std::uint8_t> bytes;
        const auto* source = static_cast<const std::uint8_t*>(pixels);
        if (s->module == kPng) {
            if (!EncodePng(*s, source, pitch, alphaBits != 0, bytes)) return 0;
        } else {
            EncodeBmp(*s, source, pitch, bytes);
        }
        if (!s->output->Write(bytes.data(), static_cast<std::uint32_t>(bytes.size()))) return 0;
        s->wrote = true;
        return 1;
    }
    bool WriteClose(void* stream) override {
        auto* s = static_cast<Stream*>(stream);
        const bool ok = s && s->wrote && s->output->Flush();
        delete s;
        return ok;
    }
    void FreeInfo(cGZGimexInfo* info) override { delete info; }

private:
    int current_ = kNone;
};

}  // namespace

void InstallImageCodecs() {
    static Codecs codecs;
    SetGimexLibrary(&codecs);
    gz::RegisterDirectorMaker("cGZGimexReader", +[]() -> cIGZUnknown* { return new cGZGimexReader(); });
    gz::RegisterDirectorMaker("cGZGimexWriter", +[]() -> cIGZUnknown* { return new cGZGimexWriter(); });
}

}  // namespace native
