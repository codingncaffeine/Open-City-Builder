#include <functional>
#include <cstdio>
#include <vector>

#include "gz/cGZMessageServer2.h"
#include "rz/cRZMessage2Standard.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

struct Target final : cIGZMessageTarget2 {
    int refs = 1;
    std::vector<std::uint32_t> got;
    std::function<void()> onMessage;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool DoMessage(cIGZMessage2* m) override {
        got.push_back(m->GetType());
        if (onMessage) onMessage();
        return true;
    }
};

struct Framework final : IMessageServerFramework {
    int added = 0, removed = 0;
    void AddTickClient(cIGZSystemService*) override { ++added; }
    void RemoveTickClient(cIGZSystemService*) override { ++removed; }
};

static cRZMessage2Standard* Msg(std::uint32_t type) {
    auto* m = new cRZMessage2Standard();
    m->AddRef();
    m->SetType(type);
    return m;
}

// The queue takes its own reference: drop the one Msg() made.
static void Post(cGZMessageServer2* server, cRZMessage2Standard* message, bool toFront) {
    server->MessagePost(message, toFront);
    message->Release();
}

int main() {
    Framework framework;
    SetMessageServerFramework(&framework);
    auto* server = new cGZMessageServer2();
    server->AddRef();
    CHECK(server->GetServiceID() == 0x04fa845b && server->GetServicePriority() == 5000000);
    CHECK(server->Init() && server->Shutdown() && framework.added == 1 && framework.removed == 1);

    Target a, b, everything;
    CHECK(server->AddNotification(&a, 10) && server->AddNotification(&a, 10) && server->AddNotification(&b, 10));
    CHECK(server->AddNotification(&everything, 0) && a.refs == 2);       // a single reference despite adding twice
    auto* ten = Msg(10);
    auto* eleven = Msg(11);
    CHECK(server->MessageSend(ten));
    CHECK(a.got == std::vector<std::uint32_t>{10} && b.got.size() == 1 && everything.got == std::vector<std::uint32_t>{10});
    server->MessageSend(eleven);
    CHECK(a.got.size() == 1 && everything.got.size() == 2);

    // posting is deferred to the tick; front posts jump the queue
    Post(server, Msg(10), false);
    Post(server, Msg(11), true);
    CHECK(server->GetMessageQueueSize() == 2);
    a.got.clear();
    everything.got.clear();
    server->OnTick(16);
    CHECK(server->GetMessageQueueSize() == 0 && everything.got == (std::vector<std::uint32_t>{11, 10}) && a.got == std::vector<std::uint32_t>{10});

    // a message posted from inside a handler is delivered in the same tick when the queue is always cleared
    b.onMessage = [&] { Post(server, Msg(11), false); b.onMessage = nullptr; };
    everything.got.clear();
    Post(server, Msg(10), false);
    server->OnTick(16);
    CHECK(everything.got == (std::vector<std::uint32_t>{10, 11}));
    // ... and only on the next tick when it is not
    server->SetAlwaysClearQueueOnTick(false);
    b.onMessage = [&] { Post(server, Msg(11), false); b.onMessage = nullptr; };
    everything.got.clear();
    Post(server, Msg(10), false);
    server->OnTick(16);
    CHECK(everything.got == std::vector<std::uint32_t>{10} && server->GetMessageQueueSize() == 1);
    server->OnTick(16);
    CHECK(everything.got == (std::vector<std::uint32_t>{10, 11}));
    server->SetAlwaysClearQueueOnTick(true);

    // changes made while a message is delivered are deferred until it finishes
    Target late;
    a.onMessage = [&] { server->AddNotification(&late, 10); server->RemoveNotification(&b, 10); a.onMessage = nullptr; };
    b.got.clear();
    server->MessageSend(ten);
    CHECK(late.got.empty() && b.got.size() == 1);        // b was already in the delivery list
    server->MessageSend(ten);
    CHECK(late.got.size() == 1 && b.got.size() == 1);    // from now on late listens and b does not

    // general posts go to one target only and can be cancelled
    Target solo;
    server->GeneralMessagePostToTarget(ten, &solo);
    server->GeneralMessagePostToTarget(eleven, &solo);
    server->GeneralMessagePostToTarget(eleven, &late);
    server->CancelGeneralMessagePostsToTarget(&late);
    server->OnTick(16);
    CHECK(solo.got == (std::vector<std::uint32_t>{10, 11}));
    size_t lateBefore = late.got.size();
    server->OnTick(16);
    CHECK(late.got.size() == lateBefore && solo.got.size() == 2);

    // removing the last listener of a type forgets the type; message creation
    CHECK(server->RemoveNotification(&b, 10) && server->RemoveNotification(&everything, 0));
    void* created = nullptr;
    CHECK(server->CreateMessage(0x46df45e7, 1, &created) && created);
    static_cast<cIGZUnknown*>(created)->Release();
    CHECK(!server->CreateMessage(0x1234, 1, &created));
    void* raw = nullptr;
    CHECK(server->QueryInterface(0x652294c7, &raw) && server->QueryInterface(0x287fb697, &raw) && !server->QueryInterface(5, &raw));
    ten->Release();
    eleven->Release();
    for (int i = 0; i < 3; ++i) server->Release();   // the two QueryInterface calls above took a reference each
    SetMessageServerFramework(nullptr);
    std::printf(failures ? "FAILED\n" : "OK\n");
    return failures != 0;
}
