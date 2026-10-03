#include "core/BookmarkStore.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

int main() {
    char rootTemplate[] = "/tmp/mpv-sofa-bookmarks-XXXXXX";
    char* root = mkdtemp(rootTemplate);
    assert(root != nullptr);
    assert(setenv("XDG_CONFIG_HOME", root, 1) == 0);

    std::string loaded;
    assert(!BookmarkStore::load(&loaded));

    const std::string places =
        "###Bookmarks###Cinema##/media/Cinema##Series##/media/Series";
    assert(BookmarkStore::save(places));
    assert(BookmarkStore::load(&loaded));
    assert(loaded == places);

    // Empty is meaningful: it preserves removal of the final user bookmark.
    assert(BookmarkStore::save(""));
    assert(BookmarkStore::load(&loaded));
    assert(loaded.empty());

    // A truncated/corrupt file must never be passed to ImGuiFileDialog.
    const auto file = std::filesystem::path(root) / "mpv-sofa" /
                      "bookmarks-v1.txt";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "not-a-bookmark-file";
    }
    assert(!BookmarkStore::load(&loaded));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return 0;
}
