// cGZGimexReader / cGZGimexWriter over a scripted Gimex library.
#include <cstdio>

#include "gz/GimexReaderWriter.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct Lib final : IGimexLibrary {
    int current = 0, opened = 0, closed = 0, written = 0; std::string lastExt; cGZGimexInfo info;
    int Find(const char* e) override { lastExt = e; return std::string(e) == ".bmp" ? 7 : 0; }
    int Get() override { return 7; }
    void Set(int m) override { current = m; }
    int Is(cIGZFile*) override { return current == 12 ? 90 : current == 5 ? 40 : 0; }
    std::uint32_t Open(void** s, cIGZFile*) override { ++opened; *s = &info; return 3; }
    bool Close(void*) override { ++closed; return true; }
    cGZGimexInfo* Info(void*, std::uint32_t) override { return &info; }
    int Read(void*, cGZGimexInfo*, void*, std::uint32_t) override { return 1; }
    bool WriteOpen(void** s, cIGZFile*, std::uint32_t) override { *s = &info; return true; }
    int Write(void*, const cGZGimexInfo*, const void*, std::uint32_t) override { ++written; return 1; }
    bool WriteClose(void*) override { return true; }
    void FreeInfo(cGZGimexInfo*) override {}
};
}  // namespace

int main() {
    Lib lib;
    SetGimexLibrary(&lib);
    auto* reader = new cGZGimexReader;
    reader->AddRef();
    std::uint32_t count = 0;
    CHECK(!reader->BeginRead(0x32u, count));              // no file set
    CHECK(!reader->SetInput(static_cast<cIGZFile*>(nullptr)));
    CHECK(!reader->GetFramePixels(nullptr, nullptr, 0));  // not reading
    CHECK(!reader->EndRead());
    void* iface = nullptr;
    CHECK(reader->QueryInterface(kGZIID_cIGZGimexReader, &iface) && !reader->QueryInterface(5, &iface));
    auto* writer = new cGZGimexWriter;
    writer->AddRef();
    CHECK(!writer->BeginWrite(0x31u, 0) && !writer->BeginWrite(".bmp", 0));
    CHECK(!writer->PutFramePixels(nullptr, nullptr, 0) && !writer->EndWrite());
    CHECK(writer->QueryInterface(kGZIID_cIGZGimexWriter, &iface));
    writer->Release(); writer->Release(); reader->Release(); reader->Release();  // plus the QueryInterface references
    if (failures == 0) std::puts("gz_gimexreaderwriter: all passed");
    return failures ? 1 : 0;
}
