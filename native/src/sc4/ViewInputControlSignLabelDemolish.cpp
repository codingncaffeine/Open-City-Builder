// cSC4ViewInputControlSignLabelDemolish (Mac i386 0x004933c4..0x00493884; spec OpenSC4 view-input-controls.md section 6).
#include "sc4/ViewInputControlSignLabelDemolish.h"

#include "sc4/OccupantFilters.h"
#include "sc4/PropOccupant.h"
#include "sc4/SignpostOccupant.h"
#include "sc4/TextDecalOccupant.h"

namespace sc4 {

namespace {
ISignLabelDemolishHost* g_demolishHost = nullptr;
constexpr std::uint32_t kSoundGroup = 0xea451903;

void Assign(cISC4OccupantFilter*& slot, cISC4OccupantFilter* value) {
    if (value) value->AddRef();
    cISC4OccupantFilter* old = slot;
    slot = value;
    if (old) old->Release();
}

// What hovering does to the prop: a label's alpha (0.7 selected, 0.5 not), a sign's dynamic state.
void Highlight(cISC4PropOccupant* prop, bool on) {
    void* raw = nullptr;
    if (prop->GetPropType() == kPropTypeTextDecal) {
        if (prop->QueryInterface(kGZIID_cISC4TextDecalOccupant, &raw) && raw) {
            static_cast<cISC4TextDecalOccupant*>(raw)->SetAlpha(on ? 0.7f : 0.5f);
            static_cast<cISC4TextDecalOccupant*>(raw)->Release();
        }
    } else if (prop->GetPropType() == kSignpostPropType) {
        if (prop->QueryInterface(kGZIID_cISC4SignpostOccupant, &raw) && raw) {
            static_cast<cISC4SignpostOccupant*>(raw)->SetDynamic(on);
            static_cast<cISC4SignpostOccupant*>(raw)->Release();
        }
    }
}
}  // namespace

ISignLabelDemolishHost* SignLabelDemolishHost() { return g_demolishHost; }
void SetSignLabelDemolishHost(ISignLabelDemolishHost* host) { g_demolishHost = host; }

cSC4ViewInputControlSignLabelDemolish::~cSC4ViewInputControlSignLabelDemolish() {
    if (selection_) {
        cISC4PropOccupant* old = selection_;
        selection_ = nullptr;
        old->Release();
    }
    Assign(filter_, nullptr);
}

bool cSC4ViewInputControlSignLabelDemolish::Init() {
    if (!initialized_) {
        id_ = kSignLabelDemolishId;
        cursorId_ = kSignLabelDemolishCursor;
        auto* filter = new cSC4SignOrLabelFilter();
        Assign(filter_, filter);
        if (g_demolishHost) g_demolishHost->ShowSignsAndLabels();
        cSC4ViewInputControl::Init();
    }
    return true;
}

bool cSC4ViewInputControlSignLabelDemolish::Shutdown() {
    if (initialized_) {
        Assign(filter_, nullptr);
        cSC4ViewInputControl::Shutdown();
    }
    return true;
}

// 0x0049347e: a click removes the prop under the cursor.
bool cSC4ViewInputControlSignLabelDemolish::OnMouseUpL(int, int, std::uint32_t) {
    if (selection_) {
        if (g_demolishHost) g_demolishHost->RemoveProp(selection_->AsOccupant());
        cISC4PropOccupant* old = selection_;
        selection_ = nullptr;
        old->Release();
        if (g_demolishHost) g_demolishHost->PlaySound(kSoundGroup, kSignLabelDemolishSound);
    }
    return true;
}

// 0x004936dc: the renderer's pick through the filter, else the first filtered occupant at the terrain point.
cISC4PropOccupant* cSC4ViewInputControlSignLabelDemolish::PickSignOrLabel(int x, int y) {
    if (!g_demolishHost) return nullptr;
    cISC4PropOccupant* prop = nullptr;
    auto asProp = [&](cISC4Occupant* occupant) {
        void* raw = nullptr;
        if (occupant && occupant->QueryInterface(kGZIID_cISC4PropOccupant, &raw) && raw) prop = static_cast<cISC4PropOccupant*>(raw);
        if (occupant) occupant->Release();
    };
    asProp(g_demolishHost->PickOccupant(x, y, filter_));
    if (!prop) {
        float hit[3] = {0, 0, 0};
        if (g_demolishHost->PickTerrain(x, y, hit)) asProp(g_demolishHost->FirstOccupantAt(hit[0], hit[2], filter_));
    }
    if (prop) prop->Release();   // the city keeps it
    return prop;
}

// 0x0049350c
void cSC4ViewInputControlSignLabelDemolish::SetSelection(cISC4PropOccupant* prop) {
    if (selection_) Highlight(selection_, false);
    if (prop) prop->AddRef();
    cISC4PropOccupant* old = selection_;
    selection_ = prop;
    if (old) old->Release();
    if (selection_) Highlight(selection_, true);
}

// 0x00493884
bool cSC4ViewInputControlSignLabelDemolish::OnMouseMove(int x, int y, std::uint32_t) {
    SetSelection(PickSignOrLabel(x, y));
    return true;
}

}  // namespace sc4
