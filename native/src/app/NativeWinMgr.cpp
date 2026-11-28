#include "app/NativeWinMgr.h"

#include "gz/WinInterfaces.h"

namespace native {

namespace {
WinMgr* g_active = nullptr;
}  // namespace
WinMgr* ActiveWinMgr() { return g_active; }
void SetActiveWinMgr(WinMgr* manager) { g_active = manager; }

WinMgr::WinMgr() : gz::cGZWinMgrBase(this) {}
WinMgr::~WinMgr() = default;

void WinMgr::PostWinMessage(std::uint32_t type, std::uint64_t p1, std::uint64_t p2, std::uint64_t p3) {
    cGZMessage message;
    message.type = type;
    if (type == gz::kWinMgrMsgEndModal) {   // EndModal(window, result): the posted words are (window, result, result)
        auto* target = reinterpret_cast<cIGZWin*>(static_cast<std::uintptr_t>(p1));
        for (auto it = modalStack_.rbegin(); it != modalStack_.rend(); ++it) {
            if (target && (*it)->window != target) continue;
            (*it)->done = true;
            (*it)->result = p2;
            break;
        }
        return;
    }
    if (type >= kGZMsgMouseDownL && type <= kGZMsgMouseWheel) {
        // GZWinMsgPost_Mouse packs (flags, x | y << 16, flags): unpack to the (x, y, flags << 16) form the manager reads.
        const std::int32_t packed = std::int32_t(p2);
        message.param1 = std::uint64_t(std::int64_t(std::int16_t(packed & 0xffff)));
        message.param2 = std::uint64_t(std::int64_t(std::int16_t(packed >> 16)));
        message.param3 = std::uint64_t(p1) << 16;
        // The wheel posts the Windows wParam shape (delta in the high word, key state in the low one); the window reads
        // the key state from param3's high half and the signed delta from its low half.
        if (type == kGZMsgMouseWheel) message.param3 = ((std::uint64_t(p1) & 0xffffu) << 16) | ((std::uint64_t(p1) >> 16) & 0xffffu);
        postedMouse_.push_back(message);   // the window system's queue: seen at the next pump
        return;
    }
    message.param1 = p1; message.param2 = p2; message.param3 = p3;
    if (type == kGZMsgKeyDown || type == kGZMsgKeyUp || type == kGZMsgCharacter) { postedMouse_.push_back(message); return; }   // same queue
    PostMessageToTarget(GetMainWindow(), message);
}

bool WinMgr::ProcessInputMouse(std::uint32_t type, int x, int y, std::uint32_t flags) {
    DispatchPostedMouse();   // what was posted before this input comes first, as in the window system's queue
    cGZMessage message;
    message.type = type;
    message.param1 = std::uint64_t(std::int64_t(std::int16_t(x & 0xffff)));
    message.param2 = std::uint64_t(std::int64_t(std::int16_t(y & 0xffff)));
    message.param3 = std::uint64_t(flags) << 16;
    if (type == kGZMsgMouseWheel) message.param3 = ((std::uint64_t(flags) & 0xffffu) << 16) | ((std::uint64_t(flags) >> 16) & 0xffffu);
    return ProcessMouseMessage(message);
}

bool WinMgr::ProcessInputKeyboard(std::uint32_t type, std::uint32_t key, std::uint32_t modifiers) {
    DispatchPostedMouse();
    cGZMessage message;
    message.type = type;
    message.param1 = key;
    message.param2 = modifiers;
    message.param3 = 0;
    return ProcessKeyboardMessage(message);
}

void WinMgr::DispatchPostedMouse() {
    // Messages posted while these are processed join the end and are seen in the same pump; the cap stops a handler that
    // posts on every message from spinning the frame.
    for (std::size_t i = 0, limit = 256; !postedMouse_.empty() && i < limit; ++i) {
        cGZMessage message = postedMouse_.front();
        postedMouse_.erase(postedMouse_.begin());
        if (message.type == kGZMsgKeyDown || message.type == kGZMsgKeyUp || message.type == kGZMsgCharacter) ProcessKeyboardMessage(message);
        else ProcessMouseMessage(message);
    }
}

std::uint64_t WinMgr::RunModalLoop(cIGZWin* window, cIGZFrameWork*, std::uint32_t noResult, std::uint32_t abortCode) {
    if (!modalPump) return abortCode;   // nothing can end the dialog
    ++modalCount;
    ModalFrame frame;
    frame.window = window;
    frame.result = noResult;
    modalStack_.push_back(&frame);
    while (!frame.done) modalPump();
    modalStack_.pop_back();
    return frame.result;
}

}  // namespace native
