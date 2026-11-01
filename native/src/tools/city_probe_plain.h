// city_probe_plain - probe tooling, not part of the game: wraps a recovered "plain" serializable
// (a class that kept Write/Read/GetGZCLSID but dropped the cIGZUnknown base) in a real cIGZSerializable
// so the probe's factory can return it. Shared by the probe creators translation units, which cannot all
// include the same sc4 headers (LotConfigurationManager.h and BuildingDevelopmentSimulator.h define the
// same constant name).
#pragma once
#include <cstdint>

#include "gz/cIGZSerializable.h"
#include "gz/cIGZUnknown.h"
#include "sc4/CommonIIDs.h"

namespace sc4 {

template <class T>
class PlainSerializableAdapter final : public cIGZSerializable {
public:
    // A reference-counted plain class is owned through its count: the adapter holds one reference, so a component that
    // AddRefs and Releases it (the tract developer and the lot configuration manager) cannot delete it under the city.
    explicit PlainSerializableAdapter(T* object) : object_(object) {
        if constexpr (requires { object->AddRef(); }) object_->AddRef();
    }
    ~PlainSerializableAdapter() {
        if constexpr (requires { object_->Release(); }) object_->Release();
        else delete object_;
    }

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZSerializable) {
            *out = static_cast<cIGZSerializable*>(this);
            AddRef();
            return true;
        }
        if (iid == kGZIID_PlainObject) {
            *out = object_;
            return true;   // borrowed, no reference taken
        }
        *out = nullptr;
        return false;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ == 0) { delete this; return 0; }
        return refs_;
    }
    bool Write(cIGZOStream& stream) const override { return object_->Write(stream); }
    bool Read(cIGZIStream& stream) override { return object_->Read(stream); }
    std::uint32_t GetGZCLSID() const override { return object_->GetGZCLSID(); }

private:
    T* object_;
    std::uint32_t refs_ = 0;
};

template <class T>
cIGZSerializable* WrapPlain(T* object) {
    auto* adapter = new PlainSerializableAdapter<T>(object);
    adapter->AddRef();
    return adapter;
}

}  // namespace sc4
