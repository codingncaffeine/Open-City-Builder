#include "gz/FileFind.h"

#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

// The original's FindFirstFile (NTFS) and the Mac's directory API hand back names in case-insensitive sorted order, and the
// game depends on it: the region loader keeps the first city it reads for a map slot and evicts later ones, and plugins
// load in that order. readdir order is whatever the file system keeps, so the matches are read up front and sorted the
// same way (ASCII letters folded to upper case, as NTFS collates; ties broken on the raw bytes).
struct RZFileFindDir {
    cRZString path;     // the directory part of the search path
    DIR* handle = nullptr;
    struct Match {
        std::string name;
        bool isDirectory = false;
        bool statOk = false;
    };
    std::vector<Match> matches;
    std::size_t next = 0;
};

namespace {

int FoldedCompare(const std::string& a, const std::string& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char x = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(a[i]))),
                            y = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(b[i])));
        if (x != y) return x < y ? -1 : 1;
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return a.compare(b);
}

void ReadMatches(RZFileFindDir& dir, const cRZString& pattern) {
    while (dirent* entry = ::readdir(dir.handle)) {
        if (::fnmatch(pattern.ToChar(), entry->d_name, FNM_CASEFOLD) != 0) continue;
        RZFileFindDir::Match match;
        match.name = entry->d_name;
        const std::string fullPath = std::string(dir.path.ToChar()) + entry->d_name;
        struct stat info;
        match.statOk = ::stat(fullPath.c_str(), &info) == 0;
        match.isDirectory = match.statOk && (info.st_mode & S_IFMT) == S_IFDIR;
        dir.matches.push_back(std::move(match));
    }
    std::sort(dir.matches.begin(), dir.matches.end(),
              [](const RZFileFindDir::Match& a, const RZFileFindDir::Match& b) { return FoldedCompare(a.name, b.name) < 0; });
}

}  // namespace

RZFileFindData* RZFileFindFirst(const cRZString& search) {
    const std::string full(search.ToChar(), search.Strlen());
    std::string dirPart, patternPart;
    const std::size_t slash = full.rfind('/');
    if (slash == std::string::npos) {
        dirPart = "./";
        patternPart = full;
    } else {
        dirPart = full.substr(0, slash + 1);
        patternPart = full.substr(slash + 1);
    }
    DIR* handle = ::opendir(dirPart.c_str());
    if (!handle) return nullptr;
    auto* data = new RZFileFindData();
    auto* dir = new RZFileFindDir();
    dir->handle = handle;
    dir->path.AssignFromCString(dirPart.c_str());
    data->pattern.AssignFromCString(patternPart.c_str());
    data->dir = dir;
    ReadMatches(*dir, data->pattern);
    if (RZFileFindNext(data)) return data;
    // No match: the data is released but the directory handle is left open (as in the original).
    delete data->dir;
    delete data;
    return nullptr;
}

bool RZFileFindNext(RZFileFindData* data) {
    RZFileFindDir* dir = data->dir;
    if (dir->next >= dir->matches.size()) return false;
    const RZFileFindDir::Match& match = dir->matches[dir->next++];
    data->name.AssignFromCString(match.name.c_str());
    if (!match.statOk) return false;   // an entry that cannot be stat'ed ends the search, as before
    data->isDirectory = match.isDirectory;
    return true;
}

void RZFileFindFinish(RZFileFindData* data) {
    if (!data) return;
    ::closedir(data->dir->handle);
    delete data->dir;
    delete data;
}

char* Fullpath(char*, const char*, unsigned long) {
    char scratch[0x201];
    if (::getcwd(scratch, sizeof scratch)) {
        const std::size_t length = std::strlen(scratch);
        if (scratch[length - 1] != '/') {
            scratch[length] = '/';
            scratch[length + 1] = '\0';
        }
    }
    return nullptr;
}

bool Fullpath(cRZString&, const cRZString& path) {
    char buffer[0x200];
    return Fullpath(buffer, path.ToChar(), sizeof buffer) != nullptr;
}
