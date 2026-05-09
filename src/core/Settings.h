#pragma once
// Settings.h — persists user-tweakable state to `mpv-sofa.ini` alongside
// the executable.  Loaded once at startup, saved automatically on shutdown
// (and on demand from the Settings menu) when the in-memory state has
// drifted from the last saved snapshot.

#include <string>

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

// User-level preferences applied when a file loads (track picker
// pre-selects the matching audio / subtitle stream when one of these
// is non-empty).  Stored as ISO 639-2/B/T codes ("eng", "spa", …) or
// empty for "no preference".  Values flow through the same dirty +
// auto-save path as the rest of the persisted state.
const std::string& preferredAudioLang();
const std::string& preferredSubLang();
void setPreferredAudioLang(std::string lang);
void setPreferredSubLang(std::string lang);

// True when a track-list lang code (whatever the file has, e.g. "es" or
// "spa") matches a preferences code, including the common 2 ↔ 3 letter
// aliases ("eng" ↔ "en", "spa" ↔ "es", "fre"/"fra" ↔ "fr", …).
bool langMatches(const std::string& trackLang, const std::string& prefLang);

// Last room preset chosen from the Control Panel.  Persisted only as
// a UI hint — the actual room geometry / reverb values are saved as
// individual fields and may have drifted from the named preset if the
// user customised them.
int  roomPreset();
void setRoomPreset(int idx);

} // namespace Settings
