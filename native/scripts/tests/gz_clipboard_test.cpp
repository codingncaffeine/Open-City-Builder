#include <cstdio>
#include <vector>

#include "gz/ClipboardManager.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

struct Data final : cIGZClipboardData {
    int refs = 1;
    std::uint32_t type;
    explicit Data(std::uint32_t t) : type(t) {}
    bool QueryInterface(std::uint32_t iid, void** out) override { if (iid != 1) return false; *out = this; ++refs; return true; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    std::uint32_t GetDataType() const override { return type; }
    cIGZUnknown* GetData() override { return nullptr; }
    bool GetData(std::uint32_t iid, void** out) override { return QueryInterface(iid, out); }
};
struct Target final : cIGZMessageTarget {
    std::vector<cGZMessage> got;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool DoMessage(cGZMessage& m) override { got.push_back(m); return true; }
};

int main() {
    cGZMessageServer server;
    server.AddRef();
    SetClipboardMessageServer(&server);
    Target listener;
    server.AddNotification(&listener, kMessageClipboardChanged);

    auto* manager = new cGZClipboardManager();
    manager->AddRef();
    CHECK(manager->GetServiceID() == 0x27ebfffd && manager->Init() && manager->GetClipboardCount() == 1);
    cIGZClipboard* main = manager->GetClipboard(0);
    CHECK(main && main->GetID() == 0 && !manager->GetClipboard(5));

    // text round trip through the default clipboard
    CHECK(manager->SetClipboardText(cRZString("hello"), true, 0));
    cRZString out;
    CHECK(manager->GetClipboardText(out, 0) && std::string(out.Data()) == "hello");
    CHECK(manager->GetClipboardText(out, kClipboardAny) && !manager->GetClipboardText(out, 9));
    CHECK(main->GetChangeCount() == 1 && manager->IsClipboardDataTypeAvailable(kClipboardDataText, 0) &&
          manager->IsClipboardDataTypeAvailable(kClipboardDataText, kClipboardAny) &&
          manager->GetClipboardWithDataTypeAvailable(kClipboardDataText) == 0 && manager->GetClipboardWithDataTypeAvailable(99) == kClipboardNone);

    // data of another type; replaceAll clears the rest
    Data blob(0x77);
    CHECK(manager->SetClipboardData(&blob, false, 0) && blob.refs == 2 && main->IsClipboardDataTypeAvailable(0x77));
    std::uint32_t types[4], count = 4;
    CHECK(manager->GetAvailableClipboardDataTypes(types, count, 0) == 2 && types[0] == 0x77 && types[1] == kClipboardDataText);
    void* raw = nullptr;
    CHECK(manager->GetClipboardData(0x77, 1, &raw, kClipboardAny) && raw == &blob);
    Data other(0x78);
    CHECK(manager->SetClipboardData(&other, true, 0) && !main->IsClipboardDataTypeAvailable(0x77) && blob.refs == 2);   // released by the clear (refs back to 2: one query ref + ours)
    CHECK(main->IsClipboardDataTypeAvailable(0x78) && !main->IsClipboardDataTypeAvailable(kClipboardDataText));

    // notifications are posted, not sent
    CHECK(!main->GetChangeNotificationEnabled());
    main->SetChangeNotificationEnabled(true);
    listener.got.clear();
    manager->SetClipboardText(cRZString("again"), false, 0);
    CHECK(listener.got.empty() && server.GetMessageQueueSize() == 1);
    server.OnTick(16);
    CHECK(listener.got.size() == 1 && listener.got[0].type == kMessageClipboardChanged && listener.got[0].param3 == kClipboardDataText);

    // extra clipboards; "all" forms return false but act
    auto* second = new cGZClipboardManagerClipboard();
    second->AddRef();
    second->SetID(3);
    CHECK(manager->AddClipboard(second) && manager->GetClipboardCount() == 2 && second->QueryInterface(0x87ec000b, &raw));
    CHECK(!manager->SetClipboardText(cRZString("both"), true, kClipboardAll));
    cRZString one;
    CHECK(manager->GetClipboardText(one, 3) && std::string(one.Data()) == "both" && manager->GetClipboardText(one, 0));
    CHECK(!manager->ClearClipboard(kClipboardAll, kClipboardAll) && !manager->IsClipboardDataTypeAvailable(kClipboardDataText, kClipboardAny));
    CHECK(manager->RemoveClipboard(3) && !manager->RemoveClipboard(3) && manager->GetClipboardCount() == 1);
    cIGZClipboard* list[2];
    count = 2;
    CHECK(manager->GetClipboards(list, count) == 1 && list[0] == main);
    list[0]->Release();
    CHECK(!main->GetClipboardText(nullptr, count, 0));
    CHECK(manager->QueryInterface(0x7ec0010, &raw) && !manager->QueryInterface(2, &raw));
    manager->Shutdown();
    CHECK(manager->GetClipboardCount() == 0);
    manager->Release();
    manager->Release();
    SetClipboardMessageServer(nullptr);
    second->Release(); second->Release();  // plus the QueryInterface reference
    server.RemoveNotification(&listener, kMessageClipboardChanged);
    std::printf(failures ? "FAILED\n" : "OK\n");
    return failures != 0;
}
