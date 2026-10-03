#pragma once

#include <string>

// Crash-safe persistence for ImGuiFileDialog's serialized Places data.
// The dialog owns the bookmark model; this store only preserves its opaque
// representation between runs.
namespace BookmarkStore {

bool load(std::string* serializedPlaces);
bool save(const std::string& serializedPlaces);

} // namespace BookmarkStore
