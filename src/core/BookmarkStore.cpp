#include "BookmarkStore.h"
#include "platform/Platform.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr const char* kHeader = "mpv-sofa-bookmarks\t1\n";
constexpr std::size_t kMaximumBytes = 1024 * 1024;

std::string path() {
    return plat::configDir() + "bookmarks-v1.txt";
}

bool flushFile(FILE* f) {
    if (!f || fflush(f) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

FILE* openForWrite(const std::string& filePath) {
#ifdef _WIN32
    return _wfopen(std::filesystem::u8path(filePath).c_str(), L"wb");
#else
    return fopen(filePath.c_str(), "wb");
#endif
}

bool atomicReplace(const std::string& tmp, const std::string& destination) {
#ifdef _WIN32
    const auto tmpWide = std::filesystem::u8path(tmp).wstring();
    const auto dstWide = std::filesystem::u8path(destination).wstring();
    return MoveFileExW(tmpWide.c_str(), dstWide.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (::rename(tmp.c_str(), destination.c_str()) != 0) return false;
    const auto parent = std::filesystem::u8path(destination).parent_path();
    const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { (void)::fsync(fd); ::close(fd); }
    return true;
#endif
}

} // namespace

bool BookmarkStore::load(std::string* serializedPlaces) {
    if (!serializedPlaces) return false;
    serializedPlaces->clear();

    const std::string filePath = path();
    if (plat::fileSize(filePath) > kMaximumBytes) return false;
    std::ifstream input(std::filesystem::u8path(filePath), std::ios::binary);
    if (!input) return false;

    std::string contents((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    const std::string header(kHeader);
    if (contents.compare(0, header.size(), header) != 0) return false;
    *serializedPlaces = contents.substr(header.size());
    return true;
}

bool BookmarkStore::save(const std::string& serializedPlaces) {
    if (serializedPlaces.size() > kMaximumBytes) return false;
    const std::string destination = path();
    const std::string tmp = destination + ".tmp";

    FILE* f = openForWrite(tmp);
    if (!f) return false;
    bool ok = fwrite(kHeader, 1, std::char_traits<char>::length(kHeader), f) ==
              std::char_traits<char>::length(kHeader);
    if (ok && !serializedPlaces.empty()) {
        ok = fwrite(serializedPlaces.data(), 1, serializedPlaces.size(), f) ==
             serializedPlaces.size();
    }
    if (ok) ok = flushFile(f);
    if (fclose(f) != 0) ok = false;
    if (ok) ok = atomicReplace(tmp, destination);
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(tmp), ec);
        fprintf(stderr, "[Bookmarks] Could not persist '%s'\n",
                destination.c_str());
    }
    return ok;
}
