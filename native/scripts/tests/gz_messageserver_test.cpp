#include <cstdio>
#include <functional>
#include <vector>

#include "gz/cGZMessageServer.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

struct Target final : cIGZMessageTarget {
    int refs = 1;
    std::vector<std::uint32_t> got;
    std::vector<std::uint64_t> words;
    std::function<void()> onMessage;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool DoMessage(cGZMessage& m) override {
        got.push_back(m.type);
        words.push_back(m.param1);
        if (onMessage) onMessage();
        return true;
    }
};

int main() {
    auto* server = new cGZMessageServer();
    server->AddRef();
    CHECK(server->GetServiceID() == 0x64063387 && server->GetServicePriority() == 5000000);
    void* raw = nullptr;
    CHECK(server->QueryInterface(0xc1085722, &raw) && server->QueryInterface(0x287fb697, &raw) && !server->QueryInterface(5, &raw));
    auto* iface = static_cast<cIGZMessageServer*>(server);

    Target a, b, zero;
    CHECK(iface->AddNotification(&a, 10) && iface->AddNotification(&a, 10) && iface->AddNotification(&b, 10) && a.refs == 2);
    CHECK(iface->AddNotification(&zero, 0));
    cGZMessage ten{10, 5, 0, 0}, eleven{11, 0, 0, 0}, none{0, 0, 0, 0};
    CHECK(iface->MessageSend(ten) && a.got == std::vector<std::uint32_t>{10} && a.words[0] == 5 && b.got.size() == 1);
    iface->MessageSend(eleven);
    iface->MessageSend(none);
    CHECK(zero.got.empty() && a.got.size() == 1);        // type 0 is never delivered, and type-0 listeners hear nothing

    iface->MessagePost(ten, false);
    iface->MessagePost(cGZMessage{10, 6, 0, 0}, true);
    CHECK(iface->GetMessageQueueSize() == 2);
    a.words.clear();
    iface->OnTick(16);
    CHECK(iface->GetMessageQueueSize() == 0 && a.words == (std::vector<std::uint64_t>{6, 5}));

    iface->SetAlwaysClearQueueOnTick(false);
    b.onMessage = [&] { iface->MessagePost(ten, false); b.onMessage = nullptr; };
    a.got.clear();
    iface->MessagePost(ten, false);
    iface->OnTick(16);
    CHECK(a.got.size() == 1 && iface->GetMessageQueueSize() == 1);
    iface->OnTick(16);
    CHECK(a.got.size() == 2);
    iface->SetAlwaysClearQueueOnTick(true);

    Target late;
    a.onMessage = [&] { iface->AddNotification(&late, 10); iface->RemoveNotification(&b, 10); a.onMessage = nullptr; };
    b.got.clear();
    iface->MessageSend(ten);
    CHECK(late.got.empty() && b.got.size() == 1);
    iface->MessageSend(ten);
    CHECK(late.got.size() == 1 && b.got.size() == 1 && b.refs == 1);

    Target solo;
    iface->GeneralMessagePostToTarget(ten, &solo);
    iface->GeneralMessagePostToTarget(eleven, &solo);
    iface->GeneralMessagePostToTarget(eleven, &late);
    iface->CancelGeneralMessagePostsToTarget(&late);
    size_t lateBefore = late.got.size();
    iface->OnTick(16);
    CHECK(solo.got == (std::vector<std::uint32_t>{10, 11}) && late.got.size() == lateBefore);
    server->Release();   // the two successful QueryInterface calls above each took a reference
    server->Release();
    server->Release();
    std::printf(failures ? "FAILED\n" : "OK\n");
    return failures != 0;
}
