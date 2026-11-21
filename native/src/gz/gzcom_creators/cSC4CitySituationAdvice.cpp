// GZCOM creator for sc4::cSC4CitySituationAdvice. In the original the class derives from cSC4Advice (the creator allocates
// 0x290 bytes and hands out the cSC4Advice view): the advisor holds the advice, its Init / Shutdown / GetTrigger /
// GetTextTitle / GetTextMessage are the situation's overrides, QueryInterface(0x6beb1e8a) answers the situation view and both
// views share one reference count. The port keeps the situation as its own class reached through ISituationAdviceBase, so the
// object made here is a cSC4Advice that owns the situation and routes those overrides to it.
#include "gz/GZCOM.h"
#include "sc4/Advice.h"
#include "sc4/CitySituationAdvice.h"

namespace gzcom {
namespace {

class CitySituationAdviceObject final : public sc4::cSC4Advice {
public:
    CitySituationAdviceObject() : base_(this), situation_(&base_) {}

    // The situation's overrides of the cSC4Advice virtuals.
    bool Init(int adviceId) override { return situation_.Init(adviceId); }
    bool Shutdown() override { return situation_.Shutdown(); }
    bool GetTrigger() const override { return situation_.GetTrigger() != 0; }
    const cIGZString& GetTextTitle() const override { return situation_.GetTextTitle(); }
    const cIGZString& GetTextMessage() const override { return situation_.GetTextMessage(); }
    std::uint32_t GetGZCLSID() const override { return sc4::kGZCLSID_cSC4CitySituationAdvice; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == sc4::kGZIID_cISC4CitySituationAdvice) return situation_.QueryInterface(iid, out);
        return sc4::cSC4Advice::QueryInterface(iid, out);
    }

private:
    // ISituationAdviceBase: the cSC4Advice base class as the situation reaches it (qualified calls: the virtuals above route
    // back to the situation).
    struct BaseView final : sc4::ISituationAdviceBase {
        explicit BaseView(CitySituationAdviceObject* o) : o_(o) {}
        bool Init(int adviceId) override { return o_->cSC4Advice::Init(adviceId); }
        void Shutdown() override { o_->cSC4Advice::Shutdown(); }
        int AdviceId() override { return o_->cSC4Advice::AdviceId(); }
        int AdvisorId() override { return o_->cSC4Advice::GetAdvisorID(); }
        void SetFlags(int flags, bool on) override { o_->cSC4Advice::SetFlags(flags, on); }
        int Mood() override { return o_->cSC4Advice::GetMood(); }
        int Priority() override { return o_->cSC4Advice::GetPriority(); }
        std::uint32_t Trigger() override { return o_->cSC4Advice::GetTrigger() ? 1u : 0u; }
        bool QueryBaseInterface(std::uint32_t iid, void** out) override { return o_->cSC4Advice::QueryInterface(iid, out); }
        cIGZUnknown* Outer() override { return static_cast<sc4::cISC4Advice*>(o_); }
        CitySituationAdviceObject* o_;
    };

    BaseView base_;
    sc4::cSC4CitySituationAdvice situation_;
};

}  // namespace

void* GZCOM_CreatecSC4CitySituationAdvice() {
    return static_cast<void*>(static_cast<cIGZUnknown*>(static_cast<sc4::cISC4Advice*>(new CitySituationAdviceObject())));
}

}  // namespace gzcom
