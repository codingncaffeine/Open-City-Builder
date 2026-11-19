// cGZGimexFactory — image file <-> cIGZBuffer conversion through the Gimex readers / writers (47 functions,
// 0x1003b4e70..0x1003b7xxx; resource factory for buffer resources, type 0x856ddbac, iid 0x02ffbb60). A reader / writer
// object comes from COM (0x2a316c02 / 0xca316c55); the factory pulls the first image, converts the palette-indexed
// pixels into the buffer's colour format and hands the bytes over. Only 8-bit Gimex images are converted; 32-bit
// images do nothing and report failure (kept), other depths fail.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "gz/Buffer.h"
#include "gz/IGraphicSystemControl.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZFile.h"
#include "gz/cIGZPersistDBSerialRecord.h"
#include "gz/cIGZPersistResourceManager.h"
#include "rz/cRZPoint.h"

constexpr std::uint32_t kGZIID_cIGZGimexFactory = 0x02ffbb60;
constexpr std::uint32_t kGZResourceTypeBuffer = 0x856ddbac;

// The Gimex image description (cGZGimexInfo, 0x2a70 bytes): width +0x10, height +0x14, bits per pixel +0x18, palette at
// +0x28 as four bytes per entry (blue, green, red, unused).
struct cGZGimexInfo {
    std::uint8_t header[0x10];
    std::int32_t width;          // +0x10
    std::int32_t height;         // +0x14
    std::int32_t bitsPerPixel;   // +0x18
    std::uint8_t reserved[0xc];
    std::uint8_t palette[0x400]; // +0x28 .. (256 entries)
    std::uint8_t rest[0x2a70 - 0x428];
};
static_assert(sizeof(cGZGimexInfo) == 0x2a70);
// Save description fields (gimex.md section 6): the quality, the data size, then alpha / red / green / blue bits.
constexpr std::size_t kGimexInfoQuality = 0x430, kGimexInfoDataSize = 0x434, kGimexInfoAlphaBits = 0x438;

class cIGZGimexReader : public cIGZUnknown {
public:
    virtual bool SetInput(cIGZFile* file) = 0;                                     // +0x18
    virtual bool SetInput(cIGZPersistDBSerialRecord* record) = 0;                  // +0x20
    virtual bool BeginRead(std::uint32_t format, std::uint32_t& imageCount) = 0;   // +0x28 (0x30 auto, 0x31 by extension, 0x32 both)
    virtual bool BeginRead(const char* extension, std::uint32_t& imageCount) = 0;  // +0x30
    virtual cGZGimexInfo* GetFrameInfo(std::uint32_t index) = 0;                   // +0x38
    virtual bool GetFramePixels(cGZGimexInfo* info, void* pixels, std::uint32_t bytes) = 0;   // +0x40
    virtual void FreeGInfo(cGZGimexInfo* info) = 0;                                // +0x48
    virtual bool EndRead() = 0;                                                    // +0x50
};
class cIGZGimexWriter : public cIGZUnknown {
public:
    virtual bool SetOutput(cIGZFile* file) = 0;                                    // +0x18
    virtual bool SetOutput(cIGZPersistDBSerialRecord* record) = 0;                 // +0x20
    virtual bool BeginWrite(std::uint32_t format, std::uint32_t flags) = 0;        // +0x28 (0x31 / 0x32: by the file's extension)
    virtual bool BeginWrite(const char* extension, std::uint32_t flags) = 0;       // +0x30
    virtual bool PutFramePixels(const cGZGimexInfo* info, const void* pixels, std::uint32_t bytes) = 0;   // +0x38
    virtual bool EndWrite() = 0;                                                   // +0x40
};

class cGZGimexFactory : public cGZPersistResourceFactory {
public:
    cGZGimexFactory() = default;                                    // @ 0x1003b4f58

    bool QueryInterface(std::uint32_t iid, void** out) override;    // @ 0x1003b507a
    std::uint32_t AddRef() override { return ++refCount_; }         // @ 0x1003b50b0
    std::uint32_t Release() override;                               // @ 0x1003b50cc

    // buffers
    bool CreateBuffer(cIGZBuffer** out);                            // @ 0x1003b5110
    // loading (format 0x32 = read, 0x31 = write are the file-format codes handed to the reader / writer)
    bool LoadFromFile(const cIGZString& path, cIGZBuffer* buffer, int colorType);                  // @ 0x1003b56d8
    bool LoadFromFile(const cIGZString& path, cIGZBuffer** buffer, int colorType);                 // @ 0x1003b57ac
    bool LoadFromFile(cIGZFile* file, std::uint32_t format, cIGZBuffer* buffer, int colorType);    // @ 0x1003b5872
    bool CreateFromFile(cIGZFile* file, std::uint32_t format, cIGZBuffer** buffer, int colorType); // @ 0x1003b5808
    bool LoadFromResource(cGZPersistResourceKey& key, std::uint32_t format, cIGZBuffer* buffer, int colorType);    // @ 0x1003b59b8
    bool CreateFromResource(cGZPersistResourceKey& key, std::uint32_t format, cIGZBuffer** buffer, int colorType); // @ 0x1003b5908
    bool LoadFromSerialRecord(cIGZPersistDBSerialRecord* record, std::uint32_t format, cIGZBuffer* buffer, int colorType);   // @ 0x1003b5ad2
    bool CreateFromSerialRecord(cIGZPersistDBSerialRecord* record, std::uint32_t format, cIGZBuffer** buffer, int colorType);   // @ 0x1003b5a68
    bool LoadWithReader(cIGZGimexReader* reader, std::uint32_t format, cIGZBuffer* buffer, int colorType);   // @ 0x1003b5160
    // saving
    bool SaveToFile(const cIGZString& path, cIGZBuffer* buffer, const cRZRect& area, std::uint32_t flags);   // @ 0x1003b621e
    bool SaveToFile(const cIGZString& path, cIGZBuffer* buffer, std::uint32_t flags);                         // @ 0x1003b61c8
    bool SaveToFile(cIGZFile* file, std::uint32_t format, cIGZBuffer* buffer, const cRZRect& area, std::uint32_t flags);   // @ 0x1003b6334
    bool SaveToFile(cIGZFile* file, std::uint32_t format, cIGZBuffer* buffer, std::uint32_t flags);           // @ 0x1003b62d4
    bool SaveToResource(cGZPersistResourceKey& key, cIGZPersistDBSegment* segment, std::uint32_t format, cIGZBuffer* buffer, const cRZRect& area, std::uint32_t flags);   // @ 0x1003b6438
    bool SaveToResource(cGZPersistResourceKey& key, cIGZPersistDBSegment* segment, std::uint32_t format, cIGZBuffer* buffer, std::uint32_t flags);   // @ 0x1003b63d2
    bool SaveToSerialRecord(cIGZPersistDBSerialRecord* record, std::uint32_t format, cIGZBuffer* buffer, const cRZRect& area, std::uint32_t flags);   // @ 0x1003b6578
    bool SaveToSerialRecord(cIGZPersistDBSerialRecord* record, std::uint32_t format, cIGZBuffer* buffer, std::uint32_t flags);   // @ 0x1003b6514
    bool SaveWithWriter(cIGZGimexWriter* writer, std::uint32_t format, cIGZBuffer* buffer, const cRZRect& area, std::uint32_t flags);   // @ 0x1003b5b68

    // pixel translation
    bool TranslateGimexToGZBuffer(cGZGimexInfo* info, void* pixels, cIGZBuffer* buffer);   // @ 0x1003b531c
    bool TranslateGZBufferToGimex(cIGZBuffer* buffer, const cRZRect& area, cGZGimexInfo* info, void* pixels);   // @ 0x1003b5d94
    // @ 0x1003b680c: 32-bit source -> ARGB4444 with error diffusion, rows scanned alternately left-to-right and
    // right-to-left. The core works on raw pointers (the member fetches them from the buffer).
    bool TranslateGimex24ToGZBufferARGB4444Dither(const cGZGimexInfo* info, const void* pixels, cIGZBuffer* buffer);
    bool DitherToARGB4444(const cGZGimexInfo* info, const void* pixels, std::uint8_t* destination, std::uint32_t destinationStride);
    bool TranslateGimex8ToGZBuffer8(const cGZGimexInfo* info, const void* pixels, cIGZBuffer* buffer);    // @ 0x1003b6b1a
    bool TranslateGimex8ToGZBuffer16(const cGZGimexInfo* info, const void* pixels, cIGZBuffer* buffer);   // @ 0x1003b6b94
    bool TranslateGimex8ToGZBuffer32(const cGZGimexInfo* info, const void* pixels, cIGZBuffer* buffer);   // @ 0x1003b6d78

    // resource factory
    bool CreateInstance(std::uint32_t type, std::uint32_t iid, void** out) override;                      // @ 0x1003b6616
    bool CreateInstance(std::uint32_t type, std::uint32_t iid, void** out, std::uint32_t colorType, cIGZUnknown* owner) override;   // @ 0x1003b666e
    bool SerialRead(cIGZPersistResource& resource, cIGZPersistDBSerialRecord& record) override;           // @ 0x1003b6718
    bool SerialWrite(cIGZPersistResource& resource, cIGZPersistDBSerialRecord& record) override;          // @ 0x1003b678e
    using cGZPersistResourceFactory::CreateInstance;

    // host seams
    static cIGZPersistResourceManager* GetResMan();                 // @ 0x1003b4e70
    static IGraphicSystemControl* GetGraphicSystem();               // @ 0x1003b4eec
    static void SetResourceManager(cIGZPersistResourceManager* manager) { sResourceManager = manager; }
    static void SetGraphicSystem(IGraphicSystemControl* graphics) { sGraphicSystem = graphics; }
    // the buffer resource a resource-factory request makes (cGZPersistBufferResource, not recovered)
    std::function<cIGZUnknown*(std::uint64_t colorTypeBits)> makeBufferResource;
    // how a reader / writer is obtained when COM does not supply one (class 0x2a316c02 / 0xca316c55)
    std::function<cIGZGimexReader*()> makeReader;
    std::function<cIGZGimexWriter*()> makeWriter;
    std::function<cIGZBuffer*()> makeBuffer;
    // the buffer / colour type held by a buffer resource (cGZPersistBufferResource slot 0x78 / field +0x38)
    std::function<cIGZBuffer*(cIGZPersistResource&)> bufferOf;
    std::function<int(cIGZPersistResource&)> colorTypeOf;

private:
    std::uint32_t refCount_ = 0;                                    // +0x28
    std::uint32_t defaultColorType_ = 1;                            // +0x2c
    bool flag30_ = true;                                            // +0x30 (dither setting 1)
    bool dither_ = true;                                            // dither setting 2 (Windows +0x1c)
    std::vector<std::uint8_t> pixelScratch_;                        // +0x38
    std::vector<std::uint32_t> ditherScratch_;                      // +0x50
    static inline cIGZPersistResourceManager* sResourceManager = nullptr;
    static inline IGraphicSystemControl* sGraphicSystem = nullptr;
};
