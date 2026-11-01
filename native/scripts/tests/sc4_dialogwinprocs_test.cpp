#include <cstdio>

#include "sc4/DialogWinProcs.h"
#include "sc4/ISC4App.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;
namespace SL { std::uint8_t* spPreferences = nullptr; cISC4App* spSC4 = nullptr; }

namespace {
struct Dialog final : cSC4DialogWinProcBase {
    using cSC4DialogWinProcBase::cSC4DialogWinProcBase;
    int attached = 0, detached = 0, ok = 0, cancel = 0;
    void GZOnAttach() override { ++attached; }
    void GZOnDetach() override { ++detached; }
    void GZOnOK() override { ++ok; }
    void GZOnCancel() override { ++cancel; }
};
}  // namespace

int main() {
    auto* dialog = new Dialog(0x1234, 0x55, 0);
    dialog->AddRef();
    void* iface = nullptr;
    CHECK(dialog->QueryInterface(0x22e85d8e, &iface) && !dialog->QueryInterface(7, &iface));
    cGZMessage init; init.type = gz::kWinProcInit;
    cGZMessage done; done.type = gz::kWinProcShutdown;
    CHECK(dialog->DoWinProcMessage(nullptr, init) && dialog->attached == 1);
    CHECK(dialog->DoWinProcMessage(nullptr, done) && dialog->detached == 1 && dialog->Window() == nullptr);
    cGZMessage command; command.type = kGZMsgCommand; command.param1 = 0x287259f6; command.param2 = 0x8a5b9959;   // OK (Mac i386 0x002eb4a8)
    CHECK(!dialog->DoWinProcMessage(nullptr, command) && dialog->ok == 1);
    command.param2 = 0xca5b9966;                              // Cancel
    dialog->DoWinProcMessage(nullptr, command);
    CHECK(dialog->cancel == 1);
    command.param2 = 0x1;
    CHECK(!dialog->GZOnButtonActivate(1));
    command.param1 = 0x999;                                  // another sender: ignored
    command.param2 = 0x8a5b9959;
    dialog->DoWinProcMessage(nullptr, command);
    CHECK(dialog->ok == 1);
    dialog->Release();                                       // count 1 -> deleted

    auto* credits = new cSC4CreditsWinProc;
    credits->AddRef();
    CHECK(!credits->ShowDialog(cSC4CreditsWinProc::kStandard));    // no UI script service
    CHECK(credits->QueryInterface(0x22e85d8e, &iface));
    credits->Release();
    credits->Release();
    if (failures == 0) std::puts("sc4_dialogwinprocs: all passed");
    return failures ? 1 : 0;
}
