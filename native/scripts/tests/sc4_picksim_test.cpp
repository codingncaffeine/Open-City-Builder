// cSC4PickSimWinProc / cSC4MySimPool (i386 0x00281d1c.. / 0x0054e108..): the "add a sim" dialog's sorting and the imported
// sims list (MySims.dat).
#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <vector>

#include "sc4/PickSimWinProc.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

tMySimTemplate Sim(const char* name, bool male, std::uint8_t zodiac) {
    tMySimTemplate t;
    t.name = cRZString(name);
    t.male = male;
    t.zodiac = zodiac;
    t.bitmapId = 1;
    return t;
}
std::vector<std::uint8_t> Bytes(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}
std::vector<std::uint8_t> Str(const char* s) {
    std::vector<std::uint8_t> out{static_cast<std::uint8_t>(std::strlen(s))};
    out.insert(out.end(), s, s + std::strlen(s));
    return out;
}
}  // namespace

int main() {
    // MySims.dat: magic, then name / male / sign / bitmap file (".bmp" appended)
    {
        const std::vector<std::uint8_t> magic{0xaa, 0xe4, 0x32, 0x4a};
        std::vector<tMySimTemplate> out;
        cSC4MySimPool::ReadImports(Bytes({magic, Str("Bella"), {0}, {7}, Str("bella01"), Str("Mortimer"), {1}, {2}, Str("mort")}), out);
        CHECK(out.size() == 2);
        if (out.size() == 2) {
            CHECK(std::string(out[0].name.ToChar()) == "Bella" && !out[0].male && out[0].zodiac == 7);
            CHECK(std::string(out[0].bitmapFile.ToChar()) == "bella01.bmp" && out[0].bitmapId == 0);
            CHECK(std::string(out[1].name.ToChar()) == "Mortimer" && out[1].male && out[1].zodiac == 2);
        }
        // a cut-off record is dropped; a wrong magic reads nothing (control)
        out.clear();
        cSC4MySimPool::ReadImports(Bytes({magic, Str("Bella"), {0}}), out);
        CHECK(out.empty());
        cSC4MySimPool::ReadImports(Bytes({{1, 2, 3, 4}, Str("Bella"), {0}, {7}, Str("b")}), out);
        CHECK(out.empty());
    }

    // sorting (no dialog: only the row order changes)
    auto* dialog = new cSC4PickSimWinProc(2);
    dialog->AddRef();
    dialog->SetTemplatesForTest({Sim("Carol", false, 5), Sim("alice", true, 0), Sim("Bob", true, 5), Sim("dave", false, 11)});
    CHECK(dialog->Count() == 4 && dialog->Order() == (std::vector<std::uint32_t>{0, 1, 2, 3}));
    dialog->SortByName(true);    // case-insensitive
    CHECK(dialog->Order() == (std::vector<std::uint32_t>{1, 2, 0, 3}));
    dialog->SortByName(false);
    CHECK(dialog->Order() == (std::vector<std::uint32_t>{3, 0, 2, 1}));
    dialog->SortBySign(true);    // by sign, list order inside a sign
    CHECK(dialog->Order() == (std::vector<std::uint32_t>{1, 0, 2, 3}));
    dialog->SortBySign(false);   // 11..0, each sign from the end of the list
    CHECK(dialog->Order() == (std::vector<std::uint32_t>{3, 2, 0, 1}));
    dialog->SortByGender(true);  // the other sex than "male" first
    CHECK(dialog->Order() == (std::vector<std::uint32_t>{0, 3, 1, 2}));
    dialog->SortByGender(false);
    CHECK(dialog->Order() == (std::vector<std::uint32_t>{1, 2, 0, 3}));
    dialog->Release();

    std::printf(failures ? "FAILED (%d)\n" : "sc4_picksim ok\n", failures);
    return failures ? 1 : 0;
}
