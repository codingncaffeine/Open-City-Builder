#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

#include "gz/cGZDBSegmentPackedFile.h"
#include "rz/cRZString.h"
#include "sc4/TokenRecord.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

constexpr cGZPersistTokenDef kUint32Def{10, kTokenUint32, 2};
constexpr cGZPersistTokenDef kStringDef{11, kTokenString, 1};
constexpr cGZPersistTokenDef kFloatDef{12, kTokenFloat32, 3};
constexpr cGZPersistTokenDef kKeyDef{13, kTokenResKey, 1};
constexpr cGZPersistTokenDef kSint8Def{14, kTokenSint8, 1};
constexpr cGZPersistTokenDef kInt64Def{15, kTokenSint64, 1};
constexpr cGZPersistTokenDef kDoubleDef{16, kTokenFloat64, 1};

// A resource that holds a token record: writes a fixed set of tokens, keeps what is read.
class TokenResource : public cGZPersistResource, public cISLTokenResource {
public:
    explicit TokenResource(bool text) : text_(text) {}
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISLTokenResource) {
            *out = static_cast<cISLTokenResource*>(this);
            AddRef();
            return true;
        }
        return cGZPersistResource::QueryInterface(iid, out);
    }
    std::uint32_t AddRef() override { return cGZPersistResource::AddRef(); }
    std::uint32_t Release() override { return cGZPersistResource::Release(); }
    bool SetTokenRecord(cSLTokenRecord* record) override {
        if (held) held->Release();
        held = record;
        record->AddRef();
        return true;
    }
    bool WriteTokens(cSLTokenRecord* r) override {
        bool ok = true;
        std::uint32_t a = 5, b = 7;
        ok &= r->SetFieldUint32(kUint32Def, a, 0);
        ok &= r->SetFieldUint32(kUint32Def, b, 1);
        cRZString s("hello");
        ok &= r->SetFieldString(kStringDef, s, 0);
        cRZString greeting("Greeting");
        r->SetTokenString(kStringDef, greeting);
        float f[3] = {1.5f, -2.25f, 100.0f};
        for (std::uint32_t i = 0; i < 3; ++i) ok &= r->SetFieldFloat32(kFloatDef, f[i], i);
        cGZPersistResourceKey key{0x11, 0x22, 0x33};
        ok &= r->SetFieldResKey(kKeyDef, key, 0);
        std::int8_t narrow = -3;
        sint8Result = r->SetFieldSint8(kSint8Def, narrow, 0);   // false: i8 tokens have no accessor
        if (!text_) {   // text form cannot carry 64-bit values on this platform (see the quirk test below)
            std::int64_t big = 1234567890123LL;
            ok &= r->SetFieldSint64(kInt64Def, big, 0);
        }
        double d = 0.125;
        double2Result = r->SetFieldFloat64(kDoubleDef, d, 0);   // false: double has no accessor either
        return ok;
    }
    bool IsTextFormat() override { return text_; }
    cSLTokenRecord* held = nullptr;
    bool sint8Result = true, double2Result = true;
    bool text_;
    ~TokenResource() override { if (held) held->Release(); }
};

std::vector<std::uint8_t> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
}

void RoundTrip(bool text) {
    std::string content;
    const std::string path = "/tmp/sc4_token_test_" + std::to_string(getpid()) + (text ? "_t.sc4" : "_b.sc4");
    const cGZPersistResourceKey key{0xdead0001, 0x1, 0x2};
    cSLTokenResourceFactory factory;
    {
        cGZDBSegmentPackedFile* seg = new cGZDBSegmentPackedFile();
        seg->AddRef();
        seg->Init();
        cRZString p(path.c_str());
        seg->SetPath(p);
        CHECK(seg->Open(false, true));
        cIGZPersistDBRecord* raw = nullptr;
        CHECK(seg->CreateNewRecord(key, &raw));
        void* serial = nullptr;
        CHECK(raw->QueryInterface(kGZIID_cIGZPersistDBSerialRecord, &serial));
        auto* record = static_cast<cIGZPersistDBSerialRecord*>(serial);
        auto* resource = new TokenResource(text);
        resource->AddRef();
        CHECK(factory.SerialWrite(*resource, *record));
        CHECK(!resource->sint8Result && !resource->double2Result);
        resource->Release();
        record->Release();
        CHECK(seg->CloseRecord(raw));
        raw->Release();
        CHECK(seg->Close());
        seg->Release();
    }
    const auto bytes = ReadFile(path);
    CHECK(!bytes.empty());
    {
        cGZDBSegmentPackedFile* seg = new cGZDBSegmentPackedFile();
        seg->AddRef();
        seg->Init();
        cRZString p(path.c_str());
        seg->SetPath(p);
        CHECK(seg->Open(true, false));
        cIGZPersistDBRecord* raw = nullptr;
        CHECK(seg->OpenRecord(key, &raw, 1));
        void* serial = nullptr;
        CHECK(raw->QueryInterface(kGZIID_cIGZPersistDBSerialRecord, &serial));
        auto* record = static_cast<cIGZPersistDBSerialRecord*>(serial);
        {
            std::vector<char> dump(record->GetSize());
            record->GetFieldVoid(dump.data(), static_cast<std::uint32_t>(dump.size()));
            content.assign(dump.begin(), dump.end());
            record->SeekAbsolute(0);
        }
        std::uint32_t cookie = 0;
        CHECK(record->GetFieldUint32(cookie));
        CHECK(cookie == (text ? kTokenCookieText : kTokenCookieBinary));
        record->SeekAbsolute(0);
        CHECK(factory.ReadCookie(*record) == cookie);
        record->SeekAbsolute(0);
        auto* resource = new TokenResource(text);
        resource->AddRef();
        CHECK(factory.SerialRead(*resource, *record));
        CHECK(resource->held && resource->held->IsRecordFormatText() == text);
        cSLTokenRecord* tokens = resource->held;
        std::uint32_t u = 0;
        CHECK(tokens->GetFieldUint32(kUint32Def, u, 0) && u == 5);
        CHECK(tokens->GetFieldUint32(kUint32Def, u, 1) && u == 7);
        CHECK(!tokens->GetFieldUint32(kUint32Def, u, 2));                   // out of range
        cRZString s;
        CHECK(tokens->GetFieldString(kStringDef, s, 0) && std::strcmp(s.ToChar(), "hello") == 0);
        float f = 0;
        CHECK(tokens->GetFieldFloat32(kFloatDef, f, 1) && f == -2.25f);
        cGZPersistResourceKey k;
        CHECK(tokens->GetFieldResKey(kKeyDef, k, 0) && k.type == 0x11 && k.group == 0x22 && k.instance == 0x33);
        std::int64_t big = 0;
        if (!text) CHECK(tokens->GetFieldSint64(kInt64Def, big, 0) && big == 1234567890123LL);
        CHECK(tokens->IsTokenAvailable(kSint8Def) && tokens->IsTokenAvailable(kDoubleDef));
        cGZPersistTokenDef wrongCount = kUint32Def;
        wrongCount.count = 3;
        CHECK(!tokens->IsTokenAvailable(wrongCount));
        std::int8_t i8 = 0;
        CHECK(!tokens->GetFieldSint8(kSint8Def, i8, 0));
        CHECK(!tokens->IsTokenAvailable({999, kTokenUint8, 1}));
        resource->Release();
        record->Release();
        seg->CloseRecord(raw);
        raw->Release();
        seg->Close();
        seg->Release();
    }
    (void)bytes;
    if (text) {
        CHECK(content.find("TKT1") != std::string::npos);
        CHECK(content.find("[ 10, 3, 2 ]= 5, 7, ") != std::string::npos);
        CHECK(content.find("Greeting[ 11, 7, 1 ]= hello, ") != std::string::npos);
    } else {
        CHECK(content.find("TKB1") != std::string::npos);
    }
    unlink(path.c_str());
}

}  // namespace

int main() {
    RoundTrip(false);
    RoundTrip(true);

    // direct record behaviour
    cSLTokenRecord record;
    cSLTokenFieldBase* field = nullptr;
    CHECK(!record.CreateField({1, kTokenUint8, 0}, &field));          // count 0 never creates
    CHECK(record.CreateField({2, kTokenUint8, 2}, &field) && field);
    cSLTokenFieldBase* again = nullptr;
    CHECK(!record.CreateField({2, kTokenUint8, 2}, &again) && again == field);   // identical: refused
    CHECK(record.CreateField({2, kTokenUint16, 2}, &again) && again->GetDef().type == kTokenUint16);   // a different type replaces
    CHECK(!record.CreateField({3, static_cast<std::uint16_t>(99), 1}, &again));  // unknown type
    std::printf(failures ? "FAILED (%d)\n" : "PASS sc4_tokenrecord\n", failures);
    return failures != 0;
}
