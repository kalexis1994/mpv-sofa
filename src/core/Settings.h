#pragma once
// Settings.h — persists user-tweakable state to `mpv-sofa.ini` alongside
// the executable.  Loaded once at startup, saved automatically on shutdown
// (and on demand from the Settings menu) when the in-memory state has
// drifted from the last saved snapshot.

struct HrtfSharedState;

namespace Settings {

// Read mpv-sofa.ini.  Missing/unreadable file is not an error — caller's
// defaults stay in place and a fresh snapshot is taken so isDirty() returns
// false until the user changes something.
void load(HrtfSharedState* state, bool* showControls, bool* show3DViz);

// Write the current state to mpv-sofa.ini and refresh the snapshot.
// Returns true on success.
bool save(const HrtfSharedState* state, bool showControls, bool show3DViz);

// True when any tracked field has changed since the last load() or save().
bool isDirty(const HrtfSharedState* state, bool showControls, bool show3DViz);

// Restore factory defaults for every persisted field.  Marks the state
// dirty so the next save() picks up the change.
void resetToDefaults(HrtfSharedState* state, bool* showControls, bool* show3DViz);

// Absolute path the module reads from / writes to (for status display).
const char* filePath();

} // namespace Settings
