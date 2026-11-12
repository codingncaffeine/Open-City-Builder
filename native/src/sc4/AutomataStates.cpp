#include "sc4/AutomataStates.h"

#include "sc4/Automaton.h"

#include "sc4/DBSegment.h"

namespace sc4 {

namespace {
template <class T>
void ReleaseAndClear(T*& pointer) {
    if (pointer) {
        T* old = pointer;
        pointer = nullptr;
        old->Release();
    }
}
}  // namespace

// ---- tAutomataState ---------------------------------------------------------------------------------------------------
void tAutomataState::Shutdown(IStateAutomaton* automaton) {
    if (controller) {
        controller->Release();
        controller = nullptr;
    }
    if (soundHandle != 0) automaton->StopSound(soundHandle);
}

bool tAutomataState::Write(cIGZOStream& stream) const {
    stream.SetUint16(kAutomataStateVersion);
    const cIGZUnknown* saved = nullptr;
    if (controller && !controller->CheckFlag(kControllerFlagTransient)) saved = static_cast<const cIGZUnknown*>(static_cast<const cIGZSerializable*>(controller));
    SC4WriteGZSerializable(stream, saved);
    stream.SetUint32(stateId);
    stream.SetFloat32(timer);
    stream << flag;
    stream.SetUint32(soundHandle);
    stream.SetUint32(value28);
    stream.SetUint32(value2c);
    return !stream.GetError();
}

bool tAutomataState::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version >= 4) return false;
    void* read = nullptr;
    SC4ReadGZSerializable(stream, kGZIID_cISC4AutomataController, &read, false);
    controller = static_cast<cSC4AutomataController*>(read);   // the previous controller is not released (kept)
    stream.GetUint32(stateId);
    stream.GetFloat32(timer);
    stream >> flag;
    stream.GetUint32(soundHandle);
    stream.GetUint32(value28);
    if (version > 2) stream.GetUint32(value2c);
    runtime30 = 0;
    return !stream.GetError();
}

// ---- subclasses -------------------------------------------------------------------------------------------------------
bool tAutomataStateRandomWalk::Write(cIGZOStream& stream) const {
    if (!tAutomataState::Write(stream)) return false;
    stream << direction;
    stream.SetFloat32(turnTimer);
    stream.SetFloat32(walkTimer);
    return !stream.GetError();
}
bool tAutomataStateRandomWalk::Read(cIGZIStream& stream) {
    if (!tAutomataState::Read(stream)) return false;
    stream >> direction;
    stream.GetFloat32(turnTimer);
    stream.GetFloat32(walkTimer);
    return !stream.GetError();
}

bool tAutomataStateRiotLeader::Write(cIGZOStream& stream) const {
    if (!tAutomataState::Write(stream)) return false;
    stream.SetSint32(value38);
    stream.SetSint32(value3c);
    stream.SetUint8(value40);
    return !stream.GetError();
}
bool tAutomataStateRiotLeader::Read(cIGZIStream& stream) {
    if (!tAutomataState::Read(stream)) return false;
    stream.GetSint32(value38);
    stream.GetSint32(value3c);
    stream.GetUint8(value40);
    return !stream.GetError();
}

bool tAutomataStatePlopJump::Write(cIGZOStream& stream) const {
    if (!tAutomataState::Write(stream)) return false;
    stream.SetFloat32(value38);
    stream.SetFloat32(value3c);
    return !stream.GetError();
}
bool tAutomataStatePlopJump::Read(cIGZIStream& stream) {
    if (!tAutomataState::Read(stream)) return false;
    stream.GetFloat32(value38);
    stream.GetFloat32(value3c);
    return !stream.GetError();
}

bool tAutomataStatePlummet::Write(cIGZOStream& stream) const {
    if (!tAutomataState::Write(stream)) return false;
    stream.SetFloat32(value38);
    stream.SetFloat32(value44);
    stream.SetFloat32(value40);
    stream.SetFloat32(value3c);
    stream.SetFloat32(value48);
    return !stream.GetError();
}
bool tAutomataStatePlummet::Read(cIGZIStream& stream) {
    if (!tAutomataState::Read(stream)) return false;
    stream.GetFloat32(value38);
    stream.GetFloat32(value44);
    stream.GetFloat32(value40);
    stream.GetFloat32(value3c);
    stream.GetFloat32(value48);
    runtime50 = 0;
    return !stream.GetError();
}

bool tAutomataStatePullOver::Write(cIGZOStream& stream) const {
    if (!tAutomataState::Write(stream)) return false;
    stream.SetFloat32(value38);
    return !stream.GetError();
}
bool tAutomataStatePullOver::Read(cIGZIStream& stream) {
    if (!tAutomataState::Read(stream)) return false;
    stream.GetFloat32(value38);
    return !stream.GetError();
}

void tAutomataStatePlayerDrive::Shutdown(IStateAutomaton* automaton) { tAutomataState::Shutdown(automaton); }
bool tAutomataStatePlayerDrive::Write(cIGZOStream& stream) const {
    if (!tAutomataState::Write(stream)) return false;
    stream.SetFloat32(value38);
    return !stream.GetError();
}
bool tAutomataStatePlayerDrive::Read(cIGZIStream& stream) {
    if (!tAutomataState::Read(stream)) return false;
    stream.GetFloat32(value38);
    for (float& v : history) v = -1.0f;
    return !stream.GetError();
}

bool tAutomataStateBumped::Write(cIGZOStream& stream) const {
    if (!tAutomataState::Write(stream)) return false;
    stream << v38;
    stream << v44;
    stream << v50;
    stream << v5c;
    return !stream.GetError();
}
bool tAutomataStateBumped::Read(cIGZIStream& stream) {
    if (!tAutomataState::Read(stream)) return false;
    stream >> v38;
    stream >> v44;
    stream >> v50;
    stream >> v5c;
    return !stream.GetError();
}

void tAutomataStateCollide::Shutdown(IStateAutomaton* automaton) {
    if (attachment) {
        attachment->Detach(true);
        ReleaseAndClear(attachment);
    }
    tAutomataState::Shutdown(automaton);
}
bool tAutomataStateCollide::Write(cIGZOStream& stream) const {
    if (!tAutomataState::Write(stream)) return false;
    return !stream.GetError();
}
bool tAutomataStateCollide::Read(cIGZIStream& stream) {
    if (!tAutomataState::Read(stream)) return false;
    return !stream.GetError();
}

tAutomataStateAttachAnim::~tAutomataStateAttachAnim() {
    if (model) model->Release();
}

bool tAutomataStateAttachAnim::Write(cIGZOStream& stream) const {
    if (!tAutomataState::Write(stream)) return false;
    SC4WriteGZSerializable(stream, animation);
    stream.SetUint16(static_cast<std::uint16_t>(value44));
    stream << flag40;
    return !stream.GetError();
}
bool tAutomataStateAttachAnim::Read(cIGZIStream& stream) {
    if (!tAutomataState::Read(stream)) return false;
    ReleaseAndClear(animation);
    SC4ReadGZSerializable(stream, kAttachAnimIid, reinterpret_cast<void**>(&animation), false);
    std::uint16_t value = 0;
    stream.GetUint16(value);
    value44 = value;
    stream >> flag40;
    return !stream.GetError();
}

bool tAutomataStateChase::Write(cIGZOStream& stream) const {
    if (!tAutomataState::Write(stream)) return false;
    SC4WriteGZSerializable(stream, target);
    return !stream.GetError();
}
bool tAutomataStateChase::Read(cIGZIStream& stream) {
    if (!tAutomataState::Read(stream)) return false;
    ReleaseAndClear(target);
    SC4ReadGZSerializable(stream, kAutomatonIid, reinterpret_cast<void**>(&target), false);
    return !stream.GetError();
}

}  // namespace sc4
