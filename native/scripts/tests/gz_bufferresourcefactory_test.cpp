// cGZBufferResourceFactory: type check, serial read/write with the write-version stack, file round trip, QueryInterface.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "gz/BufferResourceFactory.h"
#include "gz/cGZPersistDBSerialRecord.h"
#include "rz/cRZString.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
class MemoryRecord final : public cGZPersistDBSerialRecord {
public:
    MemoryRecord() : cGZPersistDBSerialRecord(nullptr, true) {}
    std::vector<std::uint8_t> bytes;
    std::size_t position = 0;
    bool GetFieldVoid(void* d, std::uint32_t n) override {
        if (position + n > bytes.size()) return false;
        std::memcpy(d, bytes.data() + position, n);
        position += n;
        return true;
    }
    bool SetFieldVoid(const void* s, std::uint32_t n) override {
        const auto* p = static_cast<const std::uint8_t*>(s);
        bytes.insert(bytes.end(), p, p + n);
        return true;
    }
    std::uint32_t GetSize() override { return static_cast<std::uint32_t>(bytes.size()); }
    bool SetSize(std::uint32_t) override { return true; }
    std::uint32_t GetPosition() override { return static_cast<std::uint32_t>(position); }
    bool SeekAbsolute(std::uint32_t p) override { position = p; return true; }
    bool SeekRelative(std::int32_t d) override { position += d; return true; }
    void DoPostClose() override {}
};
cIGZBuffer* MakeBuffer() { auto* b = new cGZBuffer(); b->AddRef(); return b; }

cIGZBuffer* Filled(std::uint32_t w, std::uint32_t h) {
    cIGZBuffer* b = MakeBuffer();
    b->Init(w, h, {kBufferColorRGB565, 16});
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) b->SetPixel(x, y, 0x100 + x * 5 + y * 3);
    return b;
}
bool Same(cIGZBuffer* a, cIGZBuffer* b) {
    if (a->Width() != b->Width() || a->Height() != b->Height()) return false;
    for (std::uint32_t y = 0; y < a->Height(); ++y)
        for (std::uint32_t x = 0; x < a->Width(); ++x)
            if (a->GetPixel(x, y) != b->GetPixel(x, y)) return false;
    return true;
}
}  // namespace

int main() {
    SetBufferMaker(MakeBuffer);
    SetBufferFormatConverter([](cIGZBuffer*, std::uint32_t) { return true; });

    auto* f = new cGZBufferResourceFactory();
    f->AddRef();

    // interfaces
    void* q = nullptr;
    CHECK(f->QueryInterface(0xa56b8f17, &q) && q == static_cast<cIGZPersistResourceFactory*>(f));
    void* s = nullptr;
    CHECK(f->QueryInterface(0xa2c72982, &s) && s == f->AsBufferResourceFactory());
    CHECK(!f->QueryInterface(0x1234, &q));
    f->Release(); f->Release();   // drop the two counted references from the queries

    // write versions: starts with {2}
    CHECK(f->GetWriteVersion() == 2 && !f->PopWriteVersion());
    CHECK(f->PushWriteVersion(1) && f->GetWriteVersion() == 1);
    CHECK(f->PushWriteVersion(0x62b9da24) && f->GetWriteVersion() == 0x62b9da24);
    CHECK(f->PopWriteVersion() && f->GetWriteVersion() == 1 && f->PopWriteVersion() && f->GetWriteVersion() == 2 && !f->PopWriteVersion());
    CHECK(static_cast<cIGZBufferResourceFactory*>(f->AsBufferResourceFactory())->GetWriteVersion() == 2);

    // CreateInstance only knows the buffer resource type
    void* inst = nullptr;
    CHECK(!f->CreateInstance(0x1234u, kGZIID_cIGZBufferResource, &inst));
    CHECK(f->CreateInstance(kGZBufferResourceV0, kGZIID_cIGZBufferResource, &inst) && inst);
    auto* res = static_cast<cIGZBufferResource*>(inst);

    // serial write / read through the resource interface (version 2 by default)
    cIGZBuffer* src = Filled(6, 3);
    res->SetBuffer(src);
    MemoryRecord rec;
    auto* resource = new cGZBufferResource();
    resource->AddRef();
    resource->SetType(kGZBufferResourceV0);
    resource->SetBuffer(src);
    CHECK(f->SerialWrite(*static_cast<cGZPersistResource*>(resource), rec));
    std::uint32_t version = 0;
    std::memcpy(&version, rec.bytes.data(), 4);
    CHECK(version == 2);
    f->PushWriteVersion(2);
    auto* wrongType = new cGZBufferResource();
    wrongType->AddRef();
    wrongType->SetType(0x1111);
    CHECK(!f->SerialWrite(*static_cast<cGZPersistResource*>(wrongType), rec) && !f->SerialRead(*static_cast<cGZPersistResource*>(wrongType), rec));

    f->SetColorType({kBufferColorRGB565, 16});
    auto* target = new cGZBufferResource();
    target->AddRef();
    target->SetType(kGZBufferResourceV0);
    CHECK(f->SerialRead(*static_cast<cGZPersistResource*>(target), rec));
    cIGZBuffer* got = nullptr;
    CHECK(target->GetBuffer(&got) && got && Same(got, src));

    // buffer <-> serial record helpers
    MemoryRecord rec2;
    CHECK(f->SaveToSerialRecord(src, &rec2) && !f->SaveToSerialRecord(nullptr, &rec2) && !f->SaveToSerialRecord(src, nullptr));
    cIGZBuffer* back = nullptr;
    CHECK(f->LoadFromSerialRecord(&back, &rec2, false) && back && Same(back, src));
    CHECK(!f->LoadFromResourceId(3, &back, false, {3, 1}, false) && !f->LoadFromResourceId(nullptr, 1, &back));

    // file round trip (SaveToFile writes into an existing empty file)
    const char* tmp = std::getenv("TMPDIR");
    std::string path = std::string(tmp ? tmp : "/tmp") + "/sc4_bufresfactory_" + std::to_string(getpid()) + ".bin";
    std::remove(path.c_str());
    { std::FILE* touch = std::fopen(path.c_str(), "wb"); if (touch) std::fclose(touch); }   // mode 3 opens an existing empty file
    cRZString p(path.c_str());
    CHECK(!f->SaveToFile(p, nullptr));
    CHECK(f->SaveToFile(p, src));
    cIGZBuffer* fromFile = nullptr;
    CHECK(f->LoadFromFile(p, &fromFile, false, {kBufferColorRGB565, 16}, false) && fromFile && Same(fromFile, src));
    std::remove(path.c_str());
    cRZString missing("/definitely/not/here.bin");
    cIGZBuffer* none = nullptr;
    CHECK(!f->LoadFromFile(missing, &none, false, {kBufferColorRGB565, 16}, false) && none == nullptr);

    for (cIGZBuffer* b : {got, back, fromFile, src}) if (b) b->Release();
    for (cIGZBufferResource* r : {res}) r->Release();
    resource->Release(); wrongType->Release(); target->Release();
    f->Release();

    if (failures == 0) std::printf("gz_bufferresourcefactory OK\n");
    return failures == 0 ? 0 : 1;
}
