// city_probe_plain - probe tooling, not part of the game: wraps a recovered "plain" serializable
// (a class that kept Write/Read/GetGZCLSID but dropped the cIGZUnknown base) in a real cIGZSerializable
// so the probe's factory can return it. Shared by the probe creators translation units, which cannot all
// include the same sc4 headers (LotConfigurationManager.h and BuildingDevelopmentSimulator.h define the
// same constant name).
#pragma once
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "gz/cIGZSerializable.h"
#include "gz/cIGZUnknown.h"
#include "sc4/CommonIIDs.h"
#include "sc4/PropRequesterInterface.h"

namespace sc4 {

// A plain class that is a prop requester in the original (cSC4CrimeSimulator: interface 0x89c498be at +8). The
// adapter answers that interface with a view that forwards to the object and shares the adapter's count.
template <class T>
concept PlainPropRequester = requires(T* object, const T* constObject, const std::vector<std::uint32_t>& candidates) {
    { object->SelectProp(candidates, 0, 0) } -> std::convertible_to<std::uint32_t>;
    { constObject->GetRequestedShare(0, 0, 0, 0) } -> std::convertible_to<int>;
    { T::GetRequesterSatisfactionId() } -> std::convertible_to<std::uint32_t>;
    { T::UsesCustomPropSelector() } -> std::convertible_to<bool>;
};

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
        if constexpr (PlainPropRequester<T>) {
            if (iid == kGZIID_cISC4PropRequester) {
                *out = static_cast<cISC4PropRequester*>(&requester_);
                AddRef();
                return true;
            }
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
    class RequesterView final : public cISC4PropRequester {
    public:
        explicit RequesterView(PlainSerializableAdapter* owner) : owner_(owner) {}
        bool QueryInterface(std::uint32_t iid, void** out) override { return owner_->QueryInterface(iid, out); }
        std::uint32_t AddRef() override { return owner_->AddRef(); }
        std::uint32_t Release() override { return owner_->Release(); }
        std::uint32_t SelectProp(const std::vector<std::uint32_t>& candidates, int x, int z) override {
            return owner_->object_->SelectProp(candidates, x, z);
        }
        int GetRequestedShare(const void* rect) const override {   // rect: SC4Rect<int> x0, z0, x1, z1 (inclusive)
            const auto* r = static_cast<const std::int32_t*>(rect);
            return owner_->object_->GetRequestedShare(r[0], r[1], r[2], r[3]);
        }
        std::uint32_t GetRequesterSatisfactionId() const override { return T::GetRequesterSatisfactionId(); }
        bool UsesCustomPropSelector() const override { return T::UsesCustomPropSelector(); }

    private:
        PlainSerializableAdapter* owner_;
    };
    struct NoRequesterView {
        explicit NoRequesterView(PlainSerializableAdapter*) {}
    };

    T* object_;
    std::uint32_t refs_ = 0;
    [[no_unique_address]] std::conditional_t<PlainPropRequester<T>, RequesterView, NoRequesterView> requester_{this};
};

template <class T>
cIGZSerializable* WrapPlain(T* object) {
    auto* adapter = new PlainSerializableAdapter<T>(object);
    adapter->AddRef();
    return adapter;
}

}  // namespace sc4
