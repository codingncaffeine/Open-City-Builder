#include "rz/cRZFile.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace {

// macOS open(2) flag values used by the original binary.
constexpr int kOpenReadWrite = 2, kOpenWriteOnly = 1;
constexpr int kOpenCreate = O_CREAT, kOpenTruncate = O_TRUNC, kOpenExclusive = O_EXCL;   // macOS: 0x200, 0x400, 0x800 (symbolic so Linux builds map correctly)
constexpr int kFilePermissions = 0x180;  // 0600
constexpr int kErrAccess = 0x20000002, kErrExists = 0x20000001;

// The encoding conversion applied to every path before it reaches the OS is the identity here.
const char* NativePath(const cIGZString& s) { return s.ToChar(); }

void CopyClamped(char* dst, const char* src, std::size_t count) {
    std::size_t n = count > 0xff ? 0xff : count;
    std::strncpy(dst, src, n);
    dst[n] = '\0';
}

std::string BuildTempName(const char* dir, const std::string& prefix, int stamp, const std::string& ext) {
    std::string out;
    out.reserve(std::strlen(dir) + prefix.size() + ext.size() + 16);
    out += dir;
    out += prefix;
    out += std::to_string(stamp);
    out += ext;
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Path helpers

void Splitpath(const char* path, char* drive, char* dir, char* name, char* ext) {
    *drive = '\0';
    const char* nameStart = nullptr;
    const char* lastDot = nullptr;
    const char* p = path;
    while (*p) {
        if (*p == '/') { nameStart = p + 1; p = nameStart; }
        else { if (*p == '.') lastDot = p; ++p; }
    }
    if (!nameStart) {
        nameStart = path;
        if (dir) *dir = '\0';
    } else if (dir) {
        CopyClamped(dir, path, static_cast<std::size_t>(nameStart - path));
    }
    if (!lastDot || lastDot < nameStart) {
        if (name) CopyClamped(name, nameStart, static_cast<std::size_t>(p - nameStart));
        if (ext) *ext = '\0';
    } else {
        if (name) CopyClamped(name, nameStart, static_cast<std::size_t>(lastDot - nameStart));
        if (ext) CopyClamped(ext, lastDot, static_cast<std::size_t>(p - lastDot));
    }
}

void Splitpath(const cRZString& path, cRZString* drive, cRZString* dir, cRZString* name, cRZString* ext) {
    char d[1], dr[0x100], n[0x100], e[0x100];
    Splitpath(path.ToChar(), d, dr, n, e);
    if (drive) drive->AssignFromCString(d);
    if (dir) dir->AssignFromCString(dr);
    if (name) name->AssignFromCString(n);
    if (ext) ext->AssignFromCString(e);
}

void SC4AdjustPathSeparatorsForPlatform(const cRZString& source, cRZString& dest) {
    // Reads `source` and writes the same index in `dest`; may be the same object.
    std::string s(source.ToChar(), source.Strlen());
    for (char& c : s) if (c == '\\') c = '/';
    dest.AssignFromChars(s.data(), static_cast<std::uint32_t>(s.size()));
}

// ---------------------------------------------------------------------------------------------
// Construction

cRZFile::cRZFile() = default;

cRZFile::cRZFile(const cIGZString& path) {
    path_.AssignFromChars(path.Data(), path.Strlen());
    SC4AdjustPathSeparatorsForPlatform(path_, path_);
}

cRZFile::cRZFile(const char* path) {
    if (!path) return;
    path_.AssignFromCString(path);
    SC4AdjustPathSeparatorsForPlatform(path_, path_);
}

cRZFile::~cRZFile() {
    if (open_) cRZFile::Close();
}

bool cRZFile::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZFile) {
        *out = static_cast<cIGZFile*>(this);
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cRZFile::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

void cRZFile::NoteErrno() { lastError_ = errno; }

// ---------------------------------------------------------------------------------------------
// Open / close / size

bool cRZFile::Open(int access, int creation, int shareFlags) {
    if (IsOpen()) return true;
    unsigned flags = access == 2 ? kOpenWriteOnly : 0u;
    if (access == 3) flags = kOpenReadWrite;
    switch (creation) {
        case 0: flags |= kOpenExclusive; break;
        case 1: flags |= kOpenCreate | kOpenTruncate; break;
        case 3: flags |= kOpenCreate; break;
        case 4: flags |= kOpenTruncate; break;
        default: break;
    }
    fd_ = ::open(NativePath(path_), static_cast<int>(flags), kFilePermissions);
    if (fd_ == -1) {
        if (errno == EACCES) lastError_ = kErrAccess;
        else if (errno == EEXIST) lastError_ = kErrExists;
        else lastError_ = errno;
        return false;
    }
    open_ = true;
    access_ = access;
    creation_ = creation;
    shareFlags_ = shareFlags;
    fdPosition_ = position_ = static_cast<int>(::lseek(fd_, 0, SEEK_CUR));
    readWindowOffset_ = readWindowCount_ = 0;
    writeFileOffset_ = writeFill_ = 0;
    return true;
}

bool cRZFile::Close() {
    if (!open_) return false;
    FlushWriteBuffer();
    readWindowOffset_ = readWindowCount_ = 0;
    writeFileOffset_ = 0;
    if (::close(fd_) == -1) NoteErrno();
    fdPosition_ = 0;
    fd_ = -1;
    access_ = creation_ = shareFlags_ = 0;
    open_ = false;
    return true;
}

bool cRZFile::FlushWriteBuffer() {
    if (writeFill_ == 0) return true;
    bool ok;
    ssize_t n = ::write(fd_, writeBuffer_.data(), static_cast<std::size_t>(writeFill_));
    if (static_cast<int>(n) == writeFill_) {
        fdPosition_ += static_cast<int>(n);
        ok = true;
    } else {
        ok = false;
        fdPosition_ = static_cast<int>(::lseek(fd_, 0, SEEK_CUR));
    }
    writeFileOffset_ = fdPosition_;
    writeFill_ = 0;
    return ok;
}

int cRZFile::Position() {
    if (!open_) return -1;
    if (position_ == -1) {
        NoteErrno();
        return -1;
    }
    return position_;
}

std::uint32_t cRZFile::Length() {
    if (!open_) return 0xffffffffu;
    int current = static_cast<int>(::lseek(fd_, 0, SEEK_CUR));
    if (current >= 0) {
        int end = static_cast<int>(::lseek(fd_, 0, SEEK_END));
        if (end >= 0) {
            ::lseek(fd_, current, SEEK_SET);
            if (writeFill_ == 0) return static_cast<std::uint32_t>(end);
            if (position_ < end) return static_cast<std::uint32_t>(end);
            return static_cast<std::uint32_t>(position_);
        }
    }
    NoteErrno();
    return 0xffffffffu;
}

bool cRZFile::SetLength(std::uint32_t length) {
    if (!open_) return false;
    readWindowOffset_ = readWindowCount_ = 0;
    FlushWriteBuffer();
    bool ok;
    if (::ftruncate(fd_, static_cast<off_t>(length)) == 0) {
        SeekToPosition(length);
        ok = true;
    } else {
        NoteErrno();
        ok = false;
    }
    fdPosition_ = position_ = static_cast<int>(::lseek(fd_, 0, SEEK_CUR));
    return ok;
}

std::uint32_t cRZFile::Seek(int offset, int origin) {
    if (!open_) return 0xffffffffu;
    if (origin != 0 && origin != 2) {
        if (origin != 1) return static_cast<std::uint32_t>(Position());
        // Relative seek: honour buffered (logical) position when it differs from the descriptor's.
        bool same = fdPosition_ == position_;
        int base = same ? 0 : position_;
        origin = same ? 1 : 0;
        offset += base;
    }
    FlushWriteBuffer();
    int result = static_cast<int>(::lseek(fd_, offset, origin));
    if (result == -1) {
        fdPosition_ = position_ = static_cast<int>(::lseek(fd_, 0, SEEK_CUR));
        NoteErrno();
        return 0xffffffffu;
    }
    fdPosition_ = position_ = result;
    return static_cast<std::uint32_t>(result);
}

// ---------------------------------------------------------------------------------------------
// Reading

bool cRZFile::Read(void* destination, std::uint32_t byteCount) {
    std::uint32_t count = byteCount;
    bool ok = ReadWithCount(destination, count);
    return ok && count == byteCount;
}

bool cRZFile::FillReadBuffer() {
    if (readBuffer_.empty()) readBuffer_.resize(readBufferSize_);
    ssize_t n = ::read(fd_, readBuffer_.data(), readBufferSize_);
    int got = static_cast<int>(n);
    bool ok;
    int offset;
    if (got < 0) {
        offset = 0;
        got = 0;
        fdPosition_ = static_cast<int>(::lseek(fd_, 0, SEEK_CUR));
        ok = false;
    } else {
        offset = fdPosition_;
        fdPosition_ += got;
        ok = true;
    }
    readWindowOffset_ = offset;
    readWindowCount_ = got;
    return ok;
}

bool cRZFile::ReadWithCount(void* destination, std::uint32_t& count) {
    if (!open_) return false;
    if (count == 0) return true;
    if (writeFill_ != 0) FlushWriteBuffer();

    auto* out = static_cast<std::uint8_t*>(destination);
    if (readBufferSize_ == 0) {  // unbuffered
        ssize_t n = ::read(fd_, out, count);
        bool ok = static_cast<int>(n) != -1;
        std::uint32_t got;
        if (!ok) {
            got = 0;
            fdPosition_ = static_cast<int>(::lseek(fd_, 0, SEEK_CUR));
            NoteErrno();
        } else {
            got = static_cast<std::uint32_t>(n);
            fdPosition_ += static_cast<int>(got);
        }
        count = got;
        position_ = fdPosition_;
        return ok;
    }

    std::uint32_t want = count;
    // Serve from the current read window first.
    if (readWindowOffset_ <= position_ && position_ < readWindowOffset_ + readWindowCount_) {
        int avail = readWindowCount_ - (position_ - readWindowOffset_);
        int n = std::min<int>(static_cast<int>(want), avail);
        std::memcpy(out, readBuffer_.data() + (position_ - readWindowOffset_), static_cast<std::size_t>(n));
        want -= static_cast<std::uint32_t>(n);
        out += n;
        position_ += n;
    }
    bool ok = true;
    while (want != 0) {
        readWindowOffset_ = readWindowCount_ = 0;
        if (fdPosition_ != position_)  // re-sync the descriptor with the logical position
            ok = Seek(position_, 0) == static_cast<std::uint32_t>(position_);
        if (ok) ok = FillReadBuffer();
        if (!ok || readWindowCount_ == 0) break;
        int n = std::min<int>(static_cast<int>(want), readWindowCount_);
        std::memcpy(out, readBuffer_.data(), static_cast<std::size_t>(n));
        out += n;
        position_ += n;
        want -= static_cast<std::uint32_t>(n);
    }
    count -= want;
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Writing

bool cRZFile::Write(const void* source, std::uint32_t byteCount) {
    std::uint32_t count = byteCount;
    bool ok = WriteWithCount(source, count);
    return ok && count == byteCount;
}

bool cRZFile::FillWriteBuffer(const std::uint8_t* data, int count) {
    bool ok = true;
    if (count <= 0) return ok;
    if (writeBuffer_.empty()) writeBuffer_.resize(writeBufferSize_);
    if (writeFill_ == 0) writeFileOffset_ = fdPosition_;
    int capacity = static_cast<int>(writeBufferSize_);
    if (capacity < writeFill_ + count) {
        while (true) {
            int chunk = std::min(count, capacity - writeFill_);
            if (chunk != 0) {
                std::memcpy(writeBuffer_.data() + writeFill_, data, static_cast<std::size_t>(chunk));
                count -= chunk;
                writeFill_ += chunk;
                data += chunk;
                capacity = static_cast<int>(writeBufferSize_);
            }
            if (writeFill_ == capacity) ok = FlushWriteBuffer();
            if (count == 0 || !ok) break;
            capacity = static_cast<int>(writeBufferSize_);
        }
    } else {
        std::memcpy(writeBuffer_.data() + writeFill_, data, static_cast<std::size_t>(count));
        writeFill_ += count;
    }
    return ok;
}

bool cRZFile::WriteWithCount(const void* source, std::uint32_t& count) {
    if (!open_) return false;
    if (readWindowCount_ != 0) {
        readWindowOffset_ = readWindowCount_ = 0;
        if (position_ != fdPosition_) SeekToPosition(static_cast<std::uint32_t>(position_));
    }
    if (writeBufferSize_ != 0) {
        bool ok = FillWriteBuffer(static_cast<const std::uint8_t*>(source), static_cast<int>(count));
        position_ += static_cast<int>(count);
        return ok;
    }
    ssize_t n = ::write(fd_, source, count);
    bool ok = static_cast<int>(n) != -1;
    std::uint32_t wrote;
    if (!ok) {
        wrote = 0;
        fdPosition_ = static_cast<int>(::lseek(fd_, 0, SEEK_CUR));
        NoteErrno();
    } else {
        wrote = static_cast<std::uint32_t>(n);
        fdPosition_ += static_cast<int>(wrote);
    }
    count = wrote;
    position_ = fdPosition_;
    return ok;
}

bool cRZFile::Flush() {
    bool wasOpen = open_;
    if (wasOpen) FlushWriteBuffer();
    return wasOpen;
}

// ---------------------------------------------------------------------------------------------
// Instance path operations

bool cRZFile::Remove() {
    if (IsOpen()) Close();
    int rc = ::remove(NativePath(path_));
    if (rc != 0) NoteErrno();
    return rc == 0;
}

// rename(2), and across filesystems (EXDEV) a copy then a remove: the original's MoveFile moves files between volumes,
// and the temporary directory (/tmp, often a tmpfs) is rarely on the filesystem the saves live on.
static bool MovePath(const char* from, const char* to) {
    if (::rename(from, to) == 0) return true;
    if (errno != EXDEV) return false;
    std::error_code error;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, error);
    if (error) { std::filesystem::remove(to, error); errno = EXDEV; return false; }
    return ::unlink(from) == 0 || errno == ENOENT;
}

bool cRZFile::Rename(const cIGZString& newPath) {
    if (IsOpen()) Close();
    if (MovePath(NativePath(path_), NativePath(newPath))) {
        path_.AssignFromChars(newPath.Data(), newPath.Strlen());
        return true;
    }
    NoteErrno();
    return false;
}

bool cRZFile::Exists() {
    struct stat st;
    return ::stat(NativePath(path_), &st) == 0;
}

void cRZFile::GetPath(cIGZString& out) const { out.AssignFromChars(path_.Data(), path_.Strlen()); }

void cRZFile::SetPath(const cIGZString& path) {
    path_.AssignFromChars(path.Data(), path.Strlen());
    SC4AdjustPathSeparatorsForPlatform(path_, path_);
}

void cRZFile::SetPath(const char* path) {
    if (!path) return;
    path_.AssignFromCString(path);
    SC4AdjustPathSeparatorsForPlatform(path_, path_);
}

bool cRZFile::GetOpenModes(int& access, int& creation, int& shareFlags) const {
    if (!open_) return false;
    access = access_;
    creation = creation_;
    shareFlags = shareFlags_;
    return true;
}

void cRZFile::GetBuffering(std::uint32_t& readSize, std::uint32_t& writeSize) const {
    readSize = readBufferSize_;
    writeSize = writeBufferSize_;
}

static std::uint32_t NormalizeBufferSize(std::uint32_t requested) {
    std::uint32_t v = requested == 0xffffffffu ? 0x200u : (requested & 0xfffffffeu);
    // 1..3 round up to 4; 0 is allowed and switches buffering off.
    std::uint32_t r = (v - 1u > 2u) ? v : 4u;
    return r > 0x1f3fffu ? 0x1f4000u : r;
}

bool cRZFile::SetBuffering(std::uint32_t readSize, std::uint32_t writeSize) {
    std::uint32_t r = NormalizeBufferSize(readSize);
    if (r < readBufferSize_) readWindowOffset_ = readWindowCount_ = 0;
    readBufferSize_ = r;
    if (!readBuffer_.empty()) readBuffer_.resize(r);

    std::uint32_t w = NormalizeBufferSize(writeSize);
    if (w < writeBufferSize_) FlushWriteBuffer();
    writeBufferSize_ = w;
    if (!writeBuffer_.empty()) writeBuffer_.resize(w);
    return true;
}

bool cRZFile::Copy(const cIGZString& sourcePath, bool overwrite) {
    bool ok = Copy(sourcePath, path_, overwrite);
    if (!ok) NoteErrno();
    return ok;
}

bool cRZFile::Copy(const cIGZFile& source, bool overwrite) {
    bool ok = Copy(*source.GetPath(), overwrite);
    if (!ok) NoteErrno();
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Static helpers

bool cRZFile::FileCreate(const cIGZString& path, bool truncate) {
    int fd = ::open(NativePath(path), kOpenReadWrite | kOpenCreate | (truncate ? kOpenTruncate : 0), kFilePermissions);   // macOS 0x602 / 0x202
    if (fd == -1) return false;
    ::close(fd);
    return true;
}

bool cRZFile::FileExists(const cIGZString& path) {
    struct stat st;
    return ::stat(NativePath(path), &st) == 0;
}

bool cRZFile::Copy(const cIGZString& source, const cIGZString& dest, bool overwrite) {
    if (source.ReverseFindChars(".DS_Store", 0xffffffffu, true) != static_cast<std::size_t>(-1))
        return true;  // Finder metadata is silently skipped
    if (!overwrite && FileExists(source)) return false;  // (sic) checks the source, see header
    int in = ::open(NativePath(source), 0);
    if (in == -1) return false;
    int out = ::open(NativePath(dest), kOpenWriteOnly | kOpenCreate | kOpenTruncate, kFilePermissions);   // macOS 0x601
    bool ok = false;
    if (out != -1) {
        std::vector<std::uint8_t> chunk(0x40000);
        while (true) {
            ssize_t n = ::read(in, chunk.data(), chunk.size());
            if (n == -1) break;
            if (n == 0) { ok = true; break; }
            if (::write(out, chunk.data(), static_cast<std::size_t>(n)) == -1) break;
        }
        ::close(out);
    }
    ::close(in);
    return ok;
}

bool cRZFile::GetAttribute(const cIGZString& path, int a) {
    struct stat st;
    if (::stat(NativePath(path), &st) != 0) return false;
    unsigned mode = st.st_mode;
    if ((a & 1) && !(mode & 0x100)) return false;
    if ((a & 2) && !(mode & 0x80)) return false;
    if ((a & 4) && !(mode & 0x40)) return false;
    if ((a & 8) && !(mode & 0x4000)) return false;
    if (a & 0x10) {
        if ((a & 0x80) || (a & 0x60) || (mode & 0xa000) == 0) return false;
    } else if ((a & 0x60) || (a & 0x80)) {
        return false;
    }
    if ((a & 0x1000) && (mode & 0x80)) return false;  // read-only requested but writable
    return true;
}

bool cRZFile::SetAttribute(const cIGZString& path, int a, bool set) {
    if (!FileExists(path)) return false;
    unsigned mode = 0x100;
    if (a & 1) mode = set ? 0x100u : 0u;
    if ((a & 2) && set) mode |= 0x80;
    if ((a & 4) && set) mode |= 0x40;
    bool result = false;
    if ((a & 0x78) == 0 && (a & 0x80) == 0) {
        if (a & 0x1000) {
            if (set) mode &= 0xff7fu;
            else mode |= 0x80;
        }
        result = ::chmod(NativePath(path), static_cast<mode_t>(mode)) != 0;  // (sic) true on failure
    }
    return result;
}

std::uint32_t cRZFile::GetTime(const cIGZString& path, TimeType type) {
    struct stat st;
    if (::stat(NativePath(path), &st) != 0) return 0;
    switch (type) {
        case kTimeAccessed: return static_cast<std::uint32_t>(st.st_atimespec.tv_sec);
        case kTimeModified: return static_cast<std::uint32_t>(st.st_mtimespec.tv_sec);
        case kTimeCreated: return static_cast<std::uint32_t>(st.st_ctimespec.tv_sec);
    }
    return 0;
}

bool cRZFile::IsFileWritable(const cIGZString& path) {
    if (!FileExists(path)) {
        if (!FileCreate(path, false)) return false;
        Remove(path);
        return true;
    }
    cRZFile file(path);
    if (!file.Open(2, 2, 1)) return false;
    file.Close();
    return true;
}

bool cRZFile::Remove(const cIGZString& path) { return ::remove(NativePath(path)) == 0; }

bool cRZFile::Rename(const cIGZString& from, const cIGZString& to) {
    return MovePath(NativePath(from), NativePath(to));
}

std::uint32_t cRZFile::GetLength(const cIGZString& path) {
    cRZFile file(path);
    if (!file.Open(1, 2, 1)) return 0xffffffffu;
    std::uint32_t length = file.Length();
    file.Close();
    return length;
}

bool cRZFile::GetTempDirectory(cIGZString& out) {
    out.Erase(0, 0xffffffffu);
    const char* dir = std::getenv("TMPDIR");
    char buf[1024];
    if (!dir || !*dir) {
#ifdef __APPLE__
        std::size_t n = ::confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof buf);
        dir = (n > 0 && n <= sizeof buf) ? buf : "/tmp";
#else
        (void)buf;
        dir = "/tmp";   // no per-user temporary directory key off macOS (the build maps it to _CS_PATH = "/bin:/usr/bin")
#endif
    }
    out.AssignFromCString(dir);
    out.AppendChars("/", 1);  // the original always appends, even when a separator is present
    return true;
}

static bool MakeUniqueTempFile(const std::string& dir, std::string prefix, std::string ext,
                               std::string& result) {
    if (ext.empty()) ext = ".tmp";
    if (prefix.size() <= 0xf4 && prefix.empty()) prefix = "RZ_";
    int stamp = static_cast<int>(std::time(nullptr));
    for (int attempt = 0; attempt < 10000; ++attempt) {
        std::string candidate = BuildTempName(dir.c_str(), prefix, stamp, ext);
        int fd = ::open(candidate.c_str(), O_CREAT | O_EXCL, kFilePermissions);  // 0xa00 on macOS
        if (fd != -1) {
            ::close(fd);
            result = candidate;
            return true;
        }
        --stamp;
    }
    return false;
}

bool cRZFile::MakeTempPathName(cIGZString& inOutPrefix, const cIGZString* extension) {
    cRZString dir;
    GetTempDirectory(dir);
    std::string result;
    bool ok = MakeUniqueTempFile(dir.ToChar(), std::string(inOutPrefix.ToChar(), inOutPrefix.Strlen()),
                                 extension ? std::string(extension->ToChar(), extension->Strlen())
                                           : std::string(),
                                 result);
    if (ok) inOutPrefix.AssignFromChars(result.data(), static_cast<std::uint32_t>(result.size()));
    return ok;
}

bool cRZFile::MakeTempFileName(const cIGZString& directory, cIGZString& inOutPrefix,
                               const cIGZString* extension) {
    std::string result;
    bool ok = MakeUniqueTempFile(directory.ToChar(),
                                 std::string(inOutPrefix.ToChar(), inOutPrefix.Strlen()),
                                 extension ? std::string(extension->ToChar(), extension->Strlen())
                                           : std::string(),
                                 result);
    if (ok) inOutPrefix.AssignFromChars(result.data(), static_cast<std::uint32_t>(result.size()));
    return ok;
}

bool cRZFile::GetPathExtension(const cIGZString& path, cIGZString& out) {
    char drive[1], dir[0x100], name[0x100], ext[0x100];
    Splitpath(path.ToChar(), drive, dir, name, ext);
    out.AssignFromChars(ext, static_cast<std::uint32_t>(std::strlen(ext)));
    return std::strlen(ext) != 0;
}

bool cRZFile::GetPathDirectory(const cIGZString& path, cIGZString& out) {
    char drive[1], dir[0x100], name[0x100], ext[0x100];
    Splitpath(path.ToChar(), drive, dir, name, ext);
    // The original reports the drive buffer here, which Splitpath always leaves empty.
    out.AssignFromChars(drive, 0);
    return false;
}

bool cRZFile::GetPathName(const cIGZString& path, cIGZString& out, bool includeExtension) {
    char drive[1], dir[0x100], name[0x100], ext[0x100];
    Splitpath(path.ToChar(), drive, dir, name, ext);
    std::string result(name);
    if (includeExtension) result += ext;
    out.AssignFromChars(result.data(), static_cast<std::uint32_t>(result.size()));
    return !result.empty();
}

bool cRZFile::GetVolumeName(const cIGZString&, cIGZString& out) {
    out.Erase(0, 0xffffffffu);
    return true;
}
