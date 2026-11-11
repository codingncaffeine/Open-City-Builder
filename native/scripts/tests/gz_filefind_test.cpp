#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "gz/FileFind.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
    const char* tmp = std::getenv("TMPDIR");
    const std::string dir = std::string(tmp ? tmp : "/tmp") + "/filefind_test/";
    std::string cmd = "rm -rf '" + dir + "' && mkdir -p '" + dir + "sub' && touch '" + dir + "a.dat' '" + dir + "B.DAT' '" + dir + "c.txt'";
    CHECK(std::system(cmd.c_str()) == 0);
    std::set<std::string> seen;
    RZFileFindData* d = RZFileFindFirst(cRZString((dir + "*.dat").c_str()));
    CHECK(d != nullptr);
    if (d) {
        do { seen.insert(d->name.ToChar()); CHECK(!d->isDirectory); } while (RZFileFindNext(d));
        RZFileFindFinish(d);
    }
    CHECK(seen == (std::set<std::string>{"a.dat", "B.DAT"}));   // case-insensitive match
    d = RZFileFindFirst(cRZString((dir + "sub").c_str()));
    CHECK(d && std::string(d->name.ToChar()) == "sub" && d->isDirectory);
    RZFileFindFinish(d);
    {   // the order: case-insensitive sorted, as FindFirstFile on NTFS lists it, whatever order the files were made in
        // (the region loader keeps the first city it reads for a map slot)
        const std::string ord = dir + "order/";
        CHECK(std::system(("mkdir -p '" + ord + "' && cd '" + ord + "' && touch 'City - New City (21).sc4' 'b.sc4' "
                           "'City - Making Money Tutorial.sc4' 'A.sc4' 'City - big.sc4'").c_str()) == 0);
        std::vector<std::string> order;
        RZFileFindData* o = RZFileFindFirst(cRZString((ord + "*.sc4").c_str()));
        CHECK(o != nullptr);
        if (o) {
            do order.push_back(o->name.ToChar()); while (RZFileFindNext(o));
            RZFileFindFinish(o);
        }
        CHECK(order == (std::vector<std::string>{"A.sc4", "b.sc4", "City - big.sc4", "City - Making Money Tutorial.sc4",
                                                 "City - New City (21).sc4"}));
    }
    CHECK(RZFileFindFirst(cRZString((dir + "*.zzz").c_str())) == nullptr);
    CHECK(RZFileFindFirst(cRZString("/no/such/dir/*")) == nullptr);
    RZFileFindFinish(nullptr);
    char buf[16];
    cRZString out;
    CHECK(Fullpath(buf, "x", sizeof buf) == nullptr && !Fullpath(out, cRZString("y")));
    std::system(("rm -rf '" + dir + "'").c_str());
    std::printf(failures ? "FAILED\n" : "gz_filefind ok\n");
    return failures != 0;
}
