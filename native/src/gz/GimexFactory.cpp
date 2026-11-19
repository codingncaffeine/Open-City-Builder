#include "gz/GimexFactory.h"

#include <algorithm>
#include <cstring>

#include "gz/GZCOMDirector.h"
#include "gz/RZServices.h"
#include "rz/cRZFile.h"

namespace {
constexpr std::uint32_t kReaderClass = 0x2a316c02, kReaderIid = 0xca316beb;
constexpr std::uint32_t kWriterClass = 0xca316c55, kWriterIid = 0x2a316c43;
constexpr std::uint32_t kBufferClass = 0xc470d325, kBufferIid = 0x20732180;
constexpr std::uint32_t kSerialRecordIid = 0xc56b8f08;
constexpr std::uint32_t kFormatRead = 0x32, kFormatWrite = 0x31, kFormatBuffer = 0x30;
// colour type 2..9 -> bits-per-pixel in the high word of the buffer's colour description (DAT_100a1e7e0)
constexpr std::uint64_t kColorTypeBits[8] = {0x800000000ull, 0x1000000000ull, 0x1000000000ull, 0x1000000000ull,
                                             0x1000000000ull, 0x1000000000ull, 0x2000000000ull, 0x2000000000ull};

std::uint64_t ColorTypeFlags(int colorType) {
    std::uint32_t index = static_cast<std::uint32_t>(colorType) - 2;
    return index < 8 ? kColorTypeBits[index] : 0;
}

cRZAutoRefCount<cIGZGimexReader> CreateReader(cGZGimexFactory& factory) {
    cRZAutoRefCount<cIGZGimexReader> reader;
    if (cIGZCOM* com = GZCOM()) com->CreateInstance(kReaderClass, kReaderIid, reader.AsOutParam());
    if (!reader && factory.makeReader) reader = factory.makeReader();
    return reader;
}
cRZAutoRefCount<cIGZGimexWriter> CreateWriter(cGZGimexFactory& factory) {
    cRZAutoRefCount<cIGZGimexWriter> writer;
    if (cIGZCOM* com = GZCOM()) com->CreateInstance(kWriterClass, kWriterIid, writer.AsOutParam());
    if (!writer && factory.makeWriter) writer = factory.makeWriter();
    return writer;
}
}  // namespace

cIGZPersistResourceManager* cGZGimexFactory::GetResMan() {
    if (sResourceManager) return sResourceManager;
    if (cIGZFrameWork* framework = RZGetFramework()) {
        cRZAutoRefCount<cIGZUnknown> found;
        framework->GetSystemService(0x056b906e, 0x656b8efc, found.AsOutParam());
        if (found) sResourceManager = static_cast<cIGZPersistResourceManager*>(static_cast<void*>(found.get()));
    }
    return sResourceManager;
}

IGraphicSystemControl* cGZGimexFactory::GetGraphicSystem() { return sGraphicSystem; }

bool cGZGimexFactory::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 1 || iid == kGZIID_cIGZGimexFactory) {
        *out = static_cast<cIGZPersistResourceFactory*>(this);
        AddRef();
        return true;
    }
    return cGZPersistResourceFactory::QueryInterface(iid, out);
}

std::uint32_t cGZGimexFactory::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

// A buffer object from COM, else from the graphic system.
bool cGZGimexFactory::CreateBuffer(cIGZBuffer** out) {
    if (cIGZCOM* com = GZCOM()) {
        if (com->CreateInstance(kBufferClass, kBufferIid, reinterpret_cast<void**>(out)) && *out) return true;
    }
    cIGZBuffer* made = makeBuffer ? makeBuffer() : (GetGraphicSystem() ? GetGraphicSystem()->CreateBuffer() : nullptr);
    *out = made;
    return made != nullptr;
}

// ---- loading ------------------------------------------------------------------------------------------

bool cGZGimexFactory::LoadWithReader(cIGZGimexReader* reader, std::uint32_t format, cIGZBuffer* buffer, int colorType) {
    if (!reader || !buffer) return false;
    bool result = false;
    std::uint32_t imageCount = 0;
    if (reader->BeginRead(format, imageCount) && imageCount != 0) {
        cGZGimexInfo* info = reader->GetFrameInfo(0);
        if (!info) {
            result = true;
        } else {
            std::uint32_t bytes = static_cast<std::uint32_t>((info->bitsPerPixel / 8) * info->height * info->width);
            pixelScratch_.resize(bytes);
            if (reader->GetFramePixels(info, pixelScratch_.data(), static_cast<std::uint32_t>(info->bitsPerPixel / 8 * info->width))) {
                if (buffer->IsReady()) buffer->Shutdown();
                std::uint64_t flags = ColorTypeFlags(colorType);
                if (buffer->Init(info->width, info->height, cGZBufferColorType{static_cast<std::int32_t>(static_cast<std::uint32_t>(colorType) | flags), static_cast<std::uint32_t>(flags >> 32)})) {
                    result = true;
                    if (buffer->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8080))) {
                        result = TranslateGimexToGZBuffer(info, pixelScratch_.data(), buffer);
                        buffer->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8080));
                    }
                }
            }
            reader->FreeGInfo(info);
        }
        reader->EndRead();
    }
    reader->Release();
    return result;
}

bool cGZGimexFactory::LoadFromFile(const cIGZString& path, cIGZBuffer* buffer, int colorType) {
    cRZAutoRefCount<cRZFile> file(new cRZFile(cRZString(path)));
    if (!file->Open(1, 2, 1)) return false;
    bool result = LoadFromFile(static_cast<cIGZFile*>(file.get()), kFormatRead, buffer, colorType);
    if (file->IsOpen()) file->Close();
    return result;
}

bool cGZGimexFactory::LoadFromFile(const cIGZString& path, cIGZBuffer** buffer, int colorType) {
    if (!CreateBuffer(buffer)) return false;
    if (LoadFromFile(path, *buffer, colorType)) return true;
    (*buffer)->Release();
    *buffer = nullptr;
    return false;
}

bool cGZGimexFactory::CreateFromFile(cIGZFile* file, std::uint32_t format, cIGZBuffer** buffer, int colorType) {
    if (!CreateBuffer(buffer)) return false;
    if (LoadFromFile(file, format, *buffer, colorType)) return true;
    (*buffer)->Release();
    *buffer = nullptr;
    return false;
}

bool cGZGimexFactory::LoadFromFile(cIGZFile* file, std::uint32_t format, cIGZBuffer* buffer, int colorType) {
    cRZAutoRefCount<cIGZGimexReader> reader = CreateReader(*this);
    if (!reader) return false;
    if (!reader->SetInput(file)) return false;
    reader->AddRef();
    return LoadWithReader(reader.get(), format, buffer, colorType);
}

bool cGZGimexFactory::LoadFromSerialRecord(cIGZPersistDBSerialRecord* record, std::uint32_t format, cIGZBuffer* buffer, int colorType) {
    cRZAutoRefCount<cIGZGimexReader> reader = CreateReader(*this);
    if (!reader) return false;
    if (!reader->SetInput(record)) return false;
    reader->AddRef();
    return LoadWithReader(reader.get(), format, buffer, colorType);
}

bool cGZGimexFactory::CreateFromSerialRecord(cIGZPersistDBSerialRecord* record, std::uint32_t format, cIGZBuffer** buffer, int colorType) {
    if (!CreateBuffer(buffer)) return false;
    if (LoadFromSerialRecord(record, format, *buffer, colorType)) return true;
    (*buffer)->Release();
    *buffer = nullptr;
    return false;
}

// resources: the record is opened for reading, its serial view loads the image, then the record is closed again
bool cGZGimexFactory::LoadFromResource(cGZPersistResourceKey& key, std::uint32_t format, cIGZBuffer* buffer, int colorType) {
    cIGZPersistResourceManager* manager = GetResMan();
    cIGZPersistDBRecord* record = nullptr;
    if (!manager || !manager->OpenDBRecord(key, &record, false)) return false;
    bool result = true;
    cRZAutoRefCount<cIGZPersistDBSerialRecord> serial;
    if (record->QueryInterface(kSerialRecordIid, serial.AsOutParam())) result = LoadFromSerialRecord(serial.get(), format, buffer, colorType);
    record->Close();
    record->Release();
    return result;
}

bool cGZGimexFactory::CreateFromResource(cGZPersistResourceKey& key, std::uint32_t format, cIGZBuffer** buffer, int colorType) {
    cIGZPersistResourceManager* manager = GetResMan();
    cIGZPersistDBRecord* record = nullptr;
    if (!manager || !manager->OpenDBRecord(key, &record, false)) return false;
    bool result = true;
    cRZAutoRefCount<cIGZPersistDBSerialRecord> serial;
    if (record->QueryInterface(kSerialRecordIid, serial.AsOutParam())) result = CreateFromSerialRecord(serial.get(), format, buffer, colorType);
    record->Close();
    record->Release();
    return result;
}

// ---- saving -------------------------------------------------------------------------------------------

bool cGZGimexFactory::SaveWithWriter(cIGZGimexWriter* writer, std::uint32_t format, cIGZBuffer* buffer, const cRZRect& area, std::uint32_t flags) {
    bool result = false;
    if (writer->BeginWrite(format, 1u)) {
        // the description (gimex.md section 6): size, version, area, 32 bits, quality, data size, channel bits
        cGZGimexInfo info;
        std::memset(&info, 0, sizeof info);
        std::int32_t width = area.right - area.left, height = area.bottom - area.top;
        auto put = [&info](std::size_t offset, std::uint32_t value) { std::memcpy(reinterpret_cast<std::uint8_t*>(&info) + offset, &value, 4); };
        put(0, sizeof(cGZGimexInfo));
        put(4, 0x157);
        info.width = width;
        info.height = height;
        info.bitsPerPixel = 32;
        put(0x1c, 32);
        std::uint32_t bytes = static_cast<std::uint32_t>(height * width * 4);
        put(kGimexInfoQuality, flags);
        put(kGimexInfoDataSize, bytes);
        const std::int32_t colorType = buffer->GetColorType().type;
        put(kGimexInfoAlphaBits, colorType == 6 || colorType == 7 || colorType == 9 ? 8u : 0u);
        put(kGimexInfoAlphaBits + 4, 8);
        put(kGimexInfoAlphaBits + 8, 8);
        put(kGimexInfoAlphaBits + 12, 8);
        pixelScratch_.assign(bytes, 0);
        if (TranslateGZBufferToGimex(buffer, area, &info, pixelScratch_.data()))
            result = writer->PutFramePixels(&info, pixelScratch_.data(), static_cast<std::uint32_t>(info.bitsPerPixel * width / 8));
    }
    writer->EndWrite();
    writer->Release();
    return result;
}

bool cGZGimexFactory::SaveToFile(const cIGZString& path, cIGZBuffer* buffer, std::uint32_t flags) {
    cRZRect area = *buffer->GetBufferArea();
    return SaveToFile(path, buffer, area, flags);
}

bool cGZGimexFactory::SaveToFile(const cIGZString& path, cIGZBuffer* buffer, const cRZRect& area, std::uint32_t flags) {
    cRZAutoRefCount<cRZFile> file(new cRZFile(cRZString(path)));
    if (!file->Open(2, 1, 1)) return false;
    bool result = SaveToFile(static_cast<cIGZFile*>(file.get()), kFormatWrite, buffer, area, flags);
    file->Close();
    return result;
}

bool cGZGimexFactory::SaveToFile(cIGZFile* file, std::uint32_t format, cIGZBuffer* buffer, std::uint32_t flags) {
    cRZRect area = *buffer->GetBufferArea();
    return SaveToFile(file, format, buffer, area, flags);
}

bool cGZGimexFactory::SaveToFile(cIGZFile* file, std::uint32_t format, cIGZBuffer* buffer, const cRZRect& area, std::uint32_t flags) {
    cRZAutoRefCount<cIGZGimexWriter> writer = CreateWriter(*this);
    if (!writer || !writer->SetOutput(file)) return false;
    writer->AddRef();
    return SaveWithWriter(writer.get(), format, buffer, area, flags);
}

bool cGZGimexFactory::SaveToSerialRecord(cIGZPersistDBSerialRecord* record, std::uint32_t format, cIGZBuffer* buffer, std::uint32_t flags) {
    cRZRect area = *buffer->GetBufferArea();
    return SaveToSerialRecord(record, format, buffer, area, flags);
}

bool cGZGimexFactory::SaveToSerialRecord(cIGZPersistDBSerialRecord* record, std::uint32_t format, cIGZBuffer* buffer, const cRZRect& area, std::uint32_t flags) {
    cRZAutoRefCount<cIGZGimexWriter> writer = CreateWriter(*this);
    if (!writer || !writer->SetOutput(record)) return false;
    writer->AddRef();
    return SaveWithWriter(writer.get(), format, buffer, area, flags);
}

bool cGZGimexFactory::SaveToResource(cGZPersistResourceKey& key, cIGZPersistDBSegment* segment, std::uint32_t format, cIGZBuffer* buffer, std::uint32_t flags) {
    cRZRect area = *buffer->GetBufferArea();
    return SaveToResource(key, segment, format, buffer, area, flags);
}

bool cGZGimexFactory::SaveToResource(cGZPersistResourceKey& key, cIGZPersistDBSegment* segment, std::uint32_t format, cIGZBuffer* buffer, const cRZRect& area, std::uint32_t flags) {
    cIGZPersistDBRecord* record = nullptr;
    bool opened;
    if (!segment) {
        cIGZPersistResourceManager* manager = GetResMan();
        opened = manager && manager->OpenDBRecord(key, &record, true);
    } else {
        opened = segment->OpenRecord(key, &record, 2);
    }
    if (!opened) return false;
    bool result = true;
    cRZAutoRefCount<cIGZPersistDBSerialRecord> serial;
    if (record->QueryInterface(kSerialRecordIid, serial.AsOutParam())) result = SaveToSerialRecord(serial.get(), format, buffer, area, flags);
    record->Close();
    record->Release();
    return result;
}

// ---- pixel translation --------------------------------------------------------------------------------

// 8-bit images are expanded through the palette; 32-bit images (A8R8G8B8 dwords) are converted to the buffer's color
// type (gimex.md section 5, 0x0090C1D4); other depths fail.
bool cGZGimexFactory::TranslateGimexToGZBuffer(cGZGimexInfo* info, void* pixels, cIGZBuffer* buffer) {
    if (!pixels || !info || !buffer) return false;
    if (info->bitsPerPixel == 8) {
        std::uint32_t bits = buffer->GetBitsPerPixel();
        if (bits == 0x20) TranslateGimex8ToGZBuffer32(info, pixels, buffer);
        else if (bits == 0x10) TranslateGimex8ToGZBuffer16(info, pixels, buffer);
        else if (bits == 8) TranslateGimex8ToGZBuffer8(info, pixels, buffer);
        else return false;
        return true;
    }
    if (info->bitsPerPixel != 0x20) return false;
    const std::uint32_t stride = buffer->GetColorSurfaceStride();
    const std::int32_t colorType = buffer->GetColorType().type;
    if (!buffer->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8040))) return false;
    auto* destination = static_cast<std::uint8_t*>(buffer->GetColorSurfaceBits());
    const auto* source = static_cast<const std::uint32_t*>(pixels);
    const int width = info->width, height = info->height;
    bool result = destination != nullptr;
    if (result && colorType == 7 && flag30_ && dither_) {
        result = DitherToARGB4444(info, pixels, destination, stride);
    } else if (result) {
        for (int y = 0; y < height; ++y, source += width, destination += stride) {
            auto* row16 = reinterpret_cast<std::uint16_t*>(destination);
            auto* row32 = reinterpret_cast<std::uint32_t*>(destination);
            for (int x = 0; x < width; ++x) {
                const std::uint32_t v = source[x];
                switch (colorType) {
                    case 4: row16[x] = static_cast<std::uint16_t>(0x8000 | ((v >> 9) & 0xfc00) | ((v >> 6) & 0x3e0) | ((v >> 3) & 0x1f)); break;
                    // quirk kept (0x0090B0C0): green gets only its top 5 bits, one bit too low, so it is halved
                    case 5: row16[x] = static_cast<std::uint16_t>(((v >> 8) & 0xf800) | ((v >> 6) & 0x3e0) | ((v >> 3) & 0x1f)); break;
                    case 6: row16[x] = static_cast<std::uint16_t>(((v >> 9) & 0x7c00) | ((v >> 6) & 0x3e0) | ((v >> 3) & 0x1f) | ((v >> 16) & 0x8000)); break;
                    case 7: row16[x] = static_cast<std::uint16_t>(((v >> 16) & 0xf000) | ((v >> 12) & 0xf00) | ((v >> 8) & 0xf0) | ((v >> 4) & 0xf)); break;
                    case 8: case 9: row32[x] = v; break;
                    default: result = false; break;
                }
                if (!result) break;
            }
            if (!result) break;
        }
    }
    buffer->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
    return result;
}

// The area of the buffer as 32-bit A8R8G8B8 pixels (gimex.md section 6, 0x0090BA37). Channels widen as c * 255 / max.
bool cGZGimexFactory::TranslateGZBufferToGimex(cIGZBuffer* buffer, const cRZRect& area, cGZGimexInfo* info, void* pixels) {
    if (!buffer || !info || !pixels) return false;
    const std::uint32_t stride = buffer->GetColorSurfaceStride();
    const std::int32_t colorType = buffer->GetColorType().type;
    if (!buffer->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8040))) return false;
    const auto* base = static_cast<const std::uint8_t*>(buffer->GetColorSurfaceBits());
    const int width = area.right - area.left, height = area.bottom - area.top;
    const int bytesPerPixel = static_cast<int>(buffer->GetBitsPerPixel() / 8);
    auto* out = static_cast<std::uint32_t*>(pixels);
    bool result = base != nullptr;
    auto widen = [](std::uint32_t c, std::uint32_t max) { return c * 255 / max; };
    for (int y = 0; result && y < height; ++y) {
        const std::uint8_t* row = base + static_cast<std::size_t>(area.top + y) * stride + static_cast<std::size_t>(area.left) * bytesPerPixel;
        for (int x = 0; x < width; ++x, ++out) {
            std::uint16_t p16 = 0;
            std::uint32_t p32 = 0;
            switch (colorType) {
                case 4: case 6: {
                    std::memcpy(&p16, row + x * 2, 2);
                    const std::uint32_t alpha = colorType == 4 || (p16 & 0x8000) ? 0xffu : 0u;
                    *out = alpha << 24 | widen((p16 >> 10) & 0x1f, 31) << 16 | widen((p16 >> 5) & 0x1f, 31) << 8 | widen(p16 & 0x1f, 31);
                    break;
                }
                case 5:
                    std::memcpy(&p16, row + x * 2, 2);
                    *out = 0xffu << 24 | widen((p16 >> 11) & 0x1f, 31) << 16 | widen((p16 >> 5) & 0x3f, 63) << 8 | widen(p16 & 0x1f, 31);
                    break;
                case 7:   // quirk kept (0x0090B408): the source advances one byte per pixel
                    std::memcpy(&p16, row + x, 2);
                    *out = ((p16 & 0xf000u) >> 4) | (p16 & 0xf0u);
                    break;
                case 8: case 9:
                    std::memcpy(&p32, row + x * 4, 4);
                    *out = p32;
                    break;
                default: result = false; break;
            }
            if (!result) break;
        }
    }
    buffer->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
    return result;
}

bool cGZGimexFactory::TranslateGimex8ToGZBuffer8(const cGZGimexInfo* info, const void* pixels, cIGZBuffer* buffer) {
    int width = info->width, height = info->height;
    int stride = static_cast<int>(buffer->GetColorSurfaceStride());
    auto* destination = static_cast<std::uint8_t*>(buffer->GetColorSurfaceBits());
    auto* source = static_cast<const std::uint8_t*>(pixels);
    for (int y = 0; y < height; ++y, source += width, destination += stride) std::memmove(destination, source, static_cast<std::size_t>(width));
    return true;
}

// Palette entry bytes are blue, green, red; the packing depends on the buffer's 16-bit colour type.
bool cGZGimexFactory::TranslateGimex8ToGZBuffer16(const cGZGimexInfo* info, const void* pixels, cIGZBuffer* buffer) {
    std::uint32_t cache[256];
    std::memset(cache, 0xff, sizeof cache);
    int width = info->width, height = info->height;
    std::uint32_t stride = buffer->GetColorSurfaceStride();
    std::int32_t colorType = buffer->GetColorType().type;
    auto* destination = static_cast<std::uint8_t*>(buffer->GetColorSurfaceBits());
    auto* source = static_cast<const std::uint8_t*>(pixels);
    for (int y = 0; y < height; ++y, destination += stride & ~1u) {
        auto* row = reinterpret_cast<std::uint16_t*>(destination);
        for (int x = 0; x < width; ++x) {
            std::uint8_t index = source[x];
            std::uint32_t value = cache[index];
            if (static_cast<std::int32_t>(value) < 0) {
                const std::uint8_t* entry = &info->palette[index * 4];
                std::uint8_t b = entry[0], g = entry[1], r = entry[2];
                switch (colorType) {
                    case 4: case 6: value = (r & 0xf8) * 0x80 + (g & 0xf8) * 4; value = (b >> 3) | value; cache[index] = value; break;
                    case 5: value = (r & 0xf8) * 0x100 + (g & 0xfc) * 8; value = (b >> 3) | value; cache[index] = value; break;
                    case 7: value = (b >> 4) + 0xf000 + ((g & 0xf0) | ((r & 0xf0) << 4)); cache[index] = value; break;
                    default: break;
                }
            }
            row[x] = static_cast<std::uint16_t>(value);
        }
        source += width;
    }
    return true;
}

bool cGZGimexFactory::TranslateGimex8ToGZBuffer32(const cGZGimexInfo* info, const void* pixels, cIGZBuffer* buffer) {
    int width = info->width, height = info->height;
    std::uint32_t stride = buffer->GetColorSurfaceStride();
    std::int32_t colorType = buffer->GetColorType().type;
    auto* destination = static_cast<std::uint8_t*>(buffer->GetColorSurfaceBits());
    auto* source = static_cast<const std::uint8_t*>(pixels);
    for (int y = 0; y < height; ++y, destination += stride & ~3u) {
        auto* row = reinterpret_cast<std::uint32_t*>(destination);
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* entry = &info->palette[source[x] * 4];
            std::uint32_t b = entry[0], g = entry[1], r = entry[2];
            row[x] = colorType == 9 ? (g << 8 | r << 16 | b) : (g << 16 | r << 24 | b << 8);
        }
        source += width;
    }
    return true;
}

// ---- resource factory ---------------------------------------------------------------------------------

bool cGZGimexFactory::CreateInstance(std::uint32_t type, std::uint32_t iid, void** out) {
    if (type == kGZResourceTypeBuffer) return CreateInstance(type, iid, out, defaultColorType_, nullptr);
    return false;
}

bool cGZGimexFactory::CreateInstance(std::uint32_t type, std::uint32_t iid, void** out, std::uint32_t colorType, cIGZUnknown*) {
    if (type != kGZResourceTypeBuffer || !makeBufferResource) return false;
    std::uint32_t index = (colorType - 2) & 0xffff;
    std::uint64_t flags = index < 8 ? kColorTypeBits[index] : 0;
    cIGZUnknown* resource = makeBufferResource((colorType & 0xffffull) | flags);
    if (!resource) return false;
    return resource->QueryInterface(iid, out);
}

bool cGZGimexFactory::SerialRead(cIGZPersistResource& resource, cIGZPersistDBSerialRecord& record) {
    if (resource.GetType() != kGZResourceTypeBuffer) return false;
    cIGZBuffer* buffer = nullptr;
    // the resource's buffer (slot 0x78 of the buffer resource, not recovered) loads from the record
    if (!bufferOf) return false;
    buffer = bufferOf(resource);
    if (!buffer) return false;
    bool result = LoadFromSerialRecord(&record, kFormatBuffer, buffer, colorTypeOf ? colorTypeOf(resource) : 0);
    buffer->Release();
    return result;
}

bool cGZGimexFactory::SerialWrite(cIGZPersistResource& resource, cIGZPersistDBSerialRecord& record) {
    if (resource.GetType() != kGZResourceTypeBuffer || !bufferOf) return false;
    cIGZBuffer* buffer = bufferOf(resource);
    if (!buffer) return false;
    bool result = SaveToSerialRecord(&record, 3, buffer, 100);
    buffer->Release();
    return result;
}

bool cGZGimexFactory::TranslateGimex24ToGZBufferARGB4444Dither(const cGZGimexInfo* info, const void* pixels, cIGZBuffer* buffer) {
    auto* bits = static_cast<std::uint8_t*>(buffer->GetColorSurfaceBits());
    const std::uint32_t stride = buffer->GetColorSurfaceStride();
    return DitherToARGB4444(info, pixels, bits, stride);
}

// The error buffers hold four 4-bit channel errors packed one per nibble pair; the "current" row starts at 0x48484848
// and the two rows swap places after every scan line. The scan starts at column 0, runs to the end, then comes back.
bool cGZGimexFactory::DitherToARGB4444(const cGZGimexInfo* info, const void* pixels, std::uint8_t* destination,
                                       std::uint32_t destinationStride) {
    std::uint32_t end = static_cast<std::uint32_t>(info->width);
    const int height = info->height;
    const int bitsPerPixel = info->bitsPerPixel;
    const std::uint32_t width = end;
    ditherScratch_.resize(static_cast<std::size_t>(width) * 2 + 4);
    std::uint32_t* current = ditherScratch_.data() + 1;
    std::uint32_t* next = ditherScratch_.data() + 1 + (width + 2);
    std::fill(current, current + width, 0x48484848u);
    std::fill(next, next + width, 0u);
    if (0 < height) {
        const long rowBytes = static_cast<long>(static_cast<int>(((bitsPerPixel + (static_cast<std::uint32_t>(bitsPerPixel >> 31) >> 29)) >> 3) * width));
        std::uint32_t carry = *current;
        const std::uint8_t* row = static_cast<const std::uint8_t*>(pixels);
        std::uint8_t* out = destination;
        int step = 1;
        std::uint32_t start = 0;
        std::uint32_t last = 0;
        for (int y = 0; y != height; ++y) {
            std::uint32_t* here = current;
            long x = static_cast<int>(start);
            last = row[x * 4] & 0xf;
            here[x] = last * 7 + carry;
            next[x] += last;
            if (start != end) {
                const std::uint8_t* p = row + 1 + static_cast<int>(start * 4);
                do {
                    std::uint32_t error = here[x];
                    auto channel = [](std::uint32_t value) {
                        return value > 0xff ? (~(static_cast<std::int32_t>(value) >> 31) & 0xffu) : value;
                    };
                    std::uint32_t c0 = channel((p[-1] - 8u) + ((error >> 4) & 0xf));
                    std::uint32_t c1 = channel((((error >> 12) & 0xf) - 8u) + p[0]);
                    std::uint32_t c2 = channel((((error >> 20) & 0xf) - 8u) + p[1]);
                    std::uint32_t c3 = channel(((error >> 28) - 8u) + p[2]);
                    const std::uint16_t pixel = static_cast<std::uint16_t>((c3 >> 4) | (c2 & 0xf0) | ((c1 & 0xf0) << 4) | ((c0 & 0xf0) << 8));
                    std::memcpy(out + x * 2, &pixel, 2);
                    last = (c0 & 0xf) | ((c1 & 0xf) << 8) | ((c2 & 0xf) << 16) | ((c3 & 0xf) << 24);
                    here[x + step] += last * 7;
                    next[x + step] = last;
                    next[x] += last * 5;
                    next[x - step] += last * 3;
                    x += step;
                    p += step * 4;
                } while (end != static_cast<std::uint32_t>(x));
            }
            const std::uint32_t newStart = end - static_cast<std::uint32_t>(step);
            carry = last * 3 + next[static_cast<int>(newStart)];
            next[static_cast<int>(newStart)] = carry;
            row += rowBytes;
            end = start - static_cast<std::uint32_t>(step);
            step = -step;
            out += destinationStride & ~1u;
            std::swap(current, next);
            start = newStart;
        }
    }
    return true;
}

