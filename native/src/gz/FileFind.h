// Directory enumeration with a wildcard pattern (RZFileFindFirst / Next / Finish @ 0x1000327ea, 0x100032c33, 0x100032e6d) and
// two path helpers that live beside them. The pattern is split at its last '/': the part before it is the directory ("./"
// when there is none) and the rest is matched case-insensitively with fnmatch against each entry. Matches come back in
// case-insensitive sorted order, as the original platforms list them (FileFind.cpp).
//
// Quirk kept: when the first search finds nothing, the data block is freed without closing the directory handle.
#pragma once
#include <string>

#include "rz/cRZString.h"

struct RZFileFindDir;   // the open directory and its path

struct RZFileFindData {
    cRZString name;                 // +0 the current entry's name
    bool isDirectory = false;       // +0x28
    cRZString pattern;              // +0x30 the part of the search path after the last '/'
    RZFileFindDir* dir = nullptr;   // +0x58
};

// Opens the search and positions it on the first match; null when the directory cannot be opened or nothing matches.
RZFileFindData* RZFileFindFirst(const cRZString& path);
// Moves to the next match; false at the end or when the entry cannot be stat'ed.
bool RZFileFindNext(RZFileFindData* data);
// Closes the directory and frees the data (null is fine).
void RZFileFindFinish(RZFileFindData* data);

// Fullpath(char*, const char*, size_t) reads the current directory into a scratch buffer and then reports failure (returns
// null) whatever it is given, so the cRZString overload never succeeds either. @ 0x100034e69 / 0x10003514f
char* Fullpath(char* out, const char* path, unsigned long size);
bool Fullpath(cRZString& out, const cRZString& path);
