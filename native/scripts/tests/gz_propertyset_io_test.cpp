// Property-set serialisation (binary / XML / legacy XML), CRC helpers and the Hit property-set resources.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio/HitPropertySetResource.h"
#include "gz/cGZAny.h"
#include "gz/cGZPropertySet.h"
#include "gz/cGZPropertySetSerializers.h"
#include "rz/PortMemoryStreams.h"
#include "rz/nRZHash.h"

namespace {
std::uint32_t U(cGZPropertySet& s, std::uint32_t key) {
    AnyUint32 a(0xdeadbeef);
    return s.Value(key, &a) ? a.get() : 0xdeadbeefu;
}
std::vector<std::uint8_t> Bytes(const std::string& s) { return {s.begin(), s.end()}; }
}  // namespace

int main() {
    // ---- CRCs: standard check values for the "123456789" string ----
    assert(nRZHash::CRC24("123456789", false) == 0x21cf02);       // CRC-24/OpenPGP
    assert(nRZHash::CRC24("", false) == 0xb704ce);
    assert(nRZHash::CRC24("ABC", true) == nRZHash::CRC24("abc", false));
    assert(nRZHash::CRC32(reinterpret_cast<const unsigned char*>("123456789"), 9) == 0x0376e6e7);  // CRC-32/MPEG-2

    // ---- binary round trip ----
    cGZPropertySet a;
    a.SetValue(0xff000001, AnyUint32(7));
    a.SetValue(0xff000002, AnyFloat32(2.5f));
    a.SetValue(0xff000003, AnyBoolean(true));
    a.SetValue(0xff000004, AnyString("hello"));
    PortMemoryOStream out;
    assert(PropertySetIO::WriteBinary(out, a));
    cGZPropertySet b;
    assert(PropertySetIO::ReadBinary(out.bytes().data(), out.bytes().size(), b));
    assert(b.NumValues() == 4 && U(b, 0xff000001) == 7);
    AnyFloat32 f; AnyBoolean bo; AnyString st;
    assert(b.Value(0xff000002, &f) && f.get() == 2.5f);
    assert(b.Value(0xff000003, &bo) && bo.value());
    assert(b.Value(0xff000004, &st) && st.CompareTo(AnyString("hello")) == 0);
    // empty sets and Sint32 values cannot be written; a bad magic cannot be read
    cGZPropertySet empty;
    PortMemoryOStream sink;
    assert(!PropertySetIO::WriteBinary(sink, empty));
    cGZPropertySet withSigned;
    withSigned.SetValue(1, AnySint32(-1));
    assert(!PropertySetIO::WriteBinary(sink, withSigned));
    const std::uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    assert(!PropertySetIO::ReadBinary(junk, sizeof junk, b));
    // unknown entry types are skipped with no payload consumed
    {
        PortMemoryOStream o;
        o.SetUint32(PropertySetIO::kBinaryMagic); o.SetUint16(1); o.SetUint32(2);
        o.SetUint32(0x0c264712); o.SetUint32(0xff000010);          // Sint32: ignored, no payload read
        o.SetUint32(0xeb61e4f7); o.SetUint32(0xff000011); o.SetUint32(33);
        cGZPropertySet c;
        assert(PropertySetIO::ReadBinary(o.bytes().data(), o.bytes().size(), c));
        assert(c.NumValues() == 1 && U(c, 0xff000011) == 33 && !c.HasValue(0xff000010));
    }

    // ---- XML write -> read ----
    cGZPropertySet x;
    x.SetValue(0xff000001, AnyUint32(5));
    x.SetValue(0xff000002, AnyFloat32(1.25f));
    x.SetValue(0xff000003, AnyBoolean(false));
    x.SetValue(0xff000004, AnyString("a&b"));
    x.SetValue(0xff000005, AnySint32(-4));
    PortMemoryOStream xo;
    assert(PropertySetIO::WriteXml(xo, x));
    const std::string xml(xo.bytes().begin(), xo.bytes().end());
    assert(xml.find("<?xml version=\"1.0\"?>") == 0 && xml.find("<cGZPropertySetUint32>") != std::string::npos);
    assert(xml.find("<AnyUint32 key=\"0xff000001\" type=\"0xeb61e4f7\">5</AnyUint32>") != std::string::npos);
    cGZPropertySet y;
    assert(PropertySetIO::ReadXml(xo.bytes().data(), xo.bytes().size(), y));
    assert(U(y, 0xff000001) == 5);
    AnySint32 sg; AnyString ys;
    assert(y.Value(0xff000005, &sg) && static_cast<std::int32_t>(sg.get()) == -4);
    assert(y.Value(0xff000004, &ys) && ys.CompareTo(AnyString("a&b")) == 0);
    cGZPropertySet auto1;
    assert(PropertySetIO::ReadAuto(out.bytes().data(), out.bytes().size(), auto1) && auto1.NumValues() == 4);  // binary
    cGZPropertySet auto2;
    assert(PropertySetIO::ReadAuto(xo.bytes().data(), xo.bytes().size(), auto2) && auto2.NumValues() >= 5);   // xml
    assert(!PropertySetIO::ReadXml(Bytes("<wrong/>").data(), 8, auto2));                                    // bad root

    // ---- legacy XML: keys are CRC24(name)|0xff000000 ----
    {
        const std::string doc =
            "<Props>\n<PropertyUint32Uint32 Name=\"Volume\" Value=\"55\"/>\n"
            "<PropertyUint32Float32 Name=\"Rate\" Value=\"1.5\"/>\n"
            "<PropertyUint32String Name=\"Label\" Value=\"hi\"/>\n"
            "<PropertyStringString Name=\"ignored\" Value=\"x\"/></Props>";
        cGZPropertySet l;
        assert(PropertySetIO::ReadLegacyXml(Bytes(doc).data(), doc.size(), l));
        assert(l.NumValues() == 3);
        assert(U(l, nRZHash::CRC24("Volume", false) | 0xff000000u) == 55);
        const std::string bad = "<Props><PropertyUint32Uint32 Name=\"X\" Value=\"abc\"/></Props>";
        cGZPropertySet l2;
        assert(!PropertySetIO::ReadLegacyXml(Bytes(bad).data(), bad.size(), l2));   // non-digit value rejected
    }

    // ---- resources ----
    {
        cHitPropertySetTrackResource* r = new cHitPropertySetTrackResource(cGZPersistResourceKey{0x0b8d821a, 0xfc45caf7, 0x77});
        r->AddRef();
        void* p = nullptr;
        assert(r->QueryInterface(0x456b8f1d, &p) && p == static_cast<cIHitPersistResource*>(r));
        assert(r->QueryInterface(0xebb10cdd, &p) && p == static_cast<cIGZPropertySet*>(r));
        assert(!r->QueryInterface(0x1234, &p));
        r->Release(); r->Release();

        // legacy text: magic, 8 filler bytes, then tokens (tag,version,skipped,soundId,entry,mode,prio,group,
        // [v!=0] 0xff706616, use3D, minDist, semId, duck, loop, vol, hitlist, d7ee, dep, 2d7c, delay, fadeIn,
        // fadeOut, semi, cents, [v>1] freq, scale, pan, fadeTgt, head, 303b)
        std::string text = "TKDT,2,x,0x10,0x20,1,50,3,9,1,4,0,6,7,80,0x30,0,0,0,11,12,13,14,15,16,17,18,19,20,21,22,";
        std::vector<std::uint8_t> blob(12, 0);
        const std::uint32_t magic = 0x544b4432;
        std::memcpy(blob.data(), &magic, 4);
        blob.insert(blob.end(), text.begin(), text.end());
        assert(r->ReadBytes(blob.data(), blob.size()));
        assert(U(*r, 0xff6f9546) == 0x77);                    // own instance recorded
        assert(U(*r, 0xffaec905) == 0x10 && U(*r, 0xff5f15e1) == 0x20);
        assert(U(*r, 0xff4dcd0d) == 1 && U(*r, 0xff050a94) == 50 && U(*r, 0xffb0333d) == 3);
        assert(U(*r, 0xff706616) == 9 && U(*r, 0xffcc156f) == 1 && U(*r, 0xffcc3b29) == 4);
        assert(U(*r, 0xff891eaa) == 0x7dcded3d);              // token 0 -> default 3D sem
        assert(U(*r, 0xffae9ddb) == 6 && U(*r, 0xfff7b8df) == 7);
        assert(U(*r, 0xff90aac8) == 80);
        // sound id equal to the resource's own instance is a self reference -> rejected
        cHitPropertySetTrackResource self(cGZPersistResourceKey{0x0b8d821a, 0, 0x10});
        assert(!self.ReadBytes(blob.data(), blob.size()));

        PortMemoryOStream o;
        assert(r->WriteOldFormat(o));
        const std::string written(o.bytes().begin(), o.bytes().end());
        assert(written.rfind("TKDT,2,0x00000010,0x00000020,1,50,3,9,1,4,0x7dcded3d,", 0) == 0);
        assert(written.size() > 10 && written.substr(written.size() - 4) == "ETKD");

        // private copy: same contents and key, independent object
        void* cp = nullptr;
        assert(r->CreatePrivateCopy(0x456b8f1d, &cp) && cp != nullptr);
        auto* copy = static_cast<cIHitPersistResource*>(cp);
        assert(copy->GetInstance() == 0x77 && copy != static_cast<cIHitPersistResource*>(r));
        copy->Release();
        copy->Release();   // the copy inherits the source count (1), then QueryInterface added one
        r->Release();
    }
    {
        cHitPropertySetResource r(cGZPersistResourceKey{0xebfee345, 0xfc45caf7, 5});
        r.SetValue(0xff000001, AnyUint32(42));
        PortMemoryOStream o;
        assert(r.Write(o, 0, nullptr));                        // type 0xebfee345 -> XML
        assert(std::string(o.bytes().begin(), o.bytes().end()).find("<AnyUint32") != std::string::npos);
        PortMemoryIStream in(o.bytes().data(), o.bytes().size());
        cHitPropertySetResource r2(cGZPersistResourceKey{0xebfee349, 0, 6});
        assert(r2.Read(in, 0, nullptr) && U(r2, 0xff000001) == 42);   // either encoding accepted
        cHitPropertySetResource r3(cGZPersistResourceKey{0x1234, 0, 7});
        PortMemoryOStream o3;
        assert(!r3.Write(o3, 0, nullptr));                      // unknown type: refused
    }
    assert(PropertySetIO::LegacyReaderIgnoredCallback() && !PropertySetIO::SerializerIgnoredCallback());
    assert(std::string(PropertySetIO::GetPropertySetTypeString()) == "cGZPropertySetUint32");
    std::puts("propertyset io ok");
}
