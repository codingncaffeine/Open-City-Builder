#include "sc4/GroupLayer.h"
#include "gz/StreamOps.h"
#include "sc4/QuadGrid.h"
#include <cstdio>
#include <vector>
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main() {
    cGroupLayer* l = new cGroupLayer(5.0f);
    l->AddRef();
    CHECK(l->InsertInstance(1, {0, 0, 0}, 1));
    CHECK(l->InsertInstance(2, {2, 0, 0}, 1));   // within 6 of the first group
    CHECK(l->InsertInstance(3, {50, 0, 0}, 1));
    CHECK(!l->InsertInstance(3, {50, 0, 0}, 1));
    CHECK(l->groupCount == 2);
    // the first instance founds a group but (as in the original) is not linked into it; the second joins it
    CHECK(l->instances.at(1)->group == nullptr);
    cGroup_SC4RH* g = l->instances.at(2)->group;
    CHECK(g && g->count == 2 && g->peak == 2);
    CHECK(l->RemoveInstance(2));
    CHECK(!l->RemoveInstance(2));
    CHECK(g->count == 1 && g->peak == 2);
    l->UpdateInstance(1, {100, 0, 0}, 1);
    CHECK(l->groupCount == 3);
    l->ClearEmptyGroups();
    CHECK(l->groupCount == 3);   // none is empty by count
    std::list<cS3DVector3> pos; int n = 0, tot = 0;
    SC4RectFloat r{40, -1, 60, 1};
    l->GetPositions(r, pos, n, tot, true);
    CHECK(n == 1 && tot == 1 && pos.front().x == 50);
    void* q = nullptr;
    CHECK(l->QueryInterface(1, &q) && q == l && !l->QueryInterface(2, &q));
    CHECK(l->Release() == 1);
    cGroupLayer* m = new cGroupLayer(5.0f);
    m->InsertInstance({51, 0, 0}, 1);
    m->InsertInstance({500, 0, 0}, 1);
    cGroupLayer* out = new cGroupLayer(5.0f);
    out->Intersect(l, m, false);
    CHECK(out->groups.size() == 1);
    out->Shutdown();
    CHECK(out->groups.empty());
    // Founding instances are in no group's list, so Shutdown cannot reach them: remove them through the public API.
    std::vector<std::uint32_t> ids;
    for (const auto& entry : l->instances) ids.push_back(entry.first);
    for (std::uint32_t id : ids) l->RemoveInstance(id);
    l->Shutdown();
    m->Shutdown();
    l->Release();
    m->Release();
    out->Release();
    std::puts("OK");
    return 0;
}
