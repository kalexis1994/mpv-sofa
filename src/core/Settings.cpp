#include "Settings.h"
#include "SharedState.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

// ---------------------------------------------------------------------------
// Snapshot — captures every persisted field so dirty detection is just a
// comparison against the last load/save state.
// ---------------------------------------------------------------------------

namespace {

struct Snapshot {
    // UI
    bool show_controls = false;
    bool show_3d_viz   = false;

    // Output
    std::string sofa_path;
    float       master_volume = 1.0f;
    int         hrtf_enabled  = 1;

    // Headphone EQ
    std::string hp_eq_path;
    int         hp_eq_enabled = 1;

    // Schroeder reverb
    int   reverb_enabled  = 1;
    float reverb_decay    = 0.45f;
    float reverb_wet      = 0.15f;
    float reverb_damping  = 0.5f;
    float reverb_predelay = 10.0f;

    // IR convolution reverb
    std::string ir_path;
    float       ir_wet = 0.35f;

    // Room
    float room_width      = 6.5f;
    float room_depth      = 5.0f;
    float room_height     = 2.7f;
    float room_absorption = 0.3f;

    // Spatial DSP
    float er_level             = 0.3f;
    float crossfeed            = 0.0f;
    float bauer_crossfeed      = 0.15f;
    int   channel_order_smpte  = 0;
    int   screen_baffling      = 0;
    int   front_pinna_boost    = 1;
    int   near_field_comp      = 1;
    int   direct_min_phase     = 0;

    // Speaker positions
    int          num_speakers = 12;
    HrtfPosition speakers[HRTF_MAX_CHANNELS];

    bool operator==(const Snapshot& o) const {
        if (show_controls != o.show_controls) return false;
        if (show_3d_viz   != o.show_3d_viz)   return false;
        if (sofa_path != o.sofa_path) return false;
        if (master_volume != o.master_volume) return false;
        if (hrtf_enabled  != o.hrtf_enabled)  return false;
        if (hp_eq_path    != o.hp_eq_path)    return false;
        if (hp_eq_enabled != o.hp_eq_enabled) return false;
        if (reverb_enabled  != o.reverb_enabled)  return false;
        if (reverb_decay    != o.reverb_decay)    return false;
        if (reverb_wet      != o.reverb_wet)      return false;
        if (reverb_damping  != o.reverb_damping)  return false;
        if (reverb_predelay != o.reverb_predelay) return false;
        if (ir_path != o.ir_path) return false;
        if (ir_wet  != o.ir_wet)  return false;
        if (room_width      != o.room_width)      return false;
        if (room_depth      != o.room_depth)      return false;
        if (room_height     != o.room_height)     return false;
        if (room_absorption != o.room_absorption) return false;
        if (er_level             != o.er_level)             return false;
        if (crossfeed            != o.crossfeed)            return false;
        if (bauer_crossfeed      != o.bauer_crossfeed)      return false;
        if (channel_order_smpte  != o.channel_order_smpte)  return false;
        if (screen_baffling      != o.screen_baffling)      return false;
        if (front_pinna_boost    != o.front_pinna_boost)    return false;
        if (near_field_comp      != o.near_field_comp)      return false;
        if (direct_min_phase     != o.direct_min_phase)     return false;
        if (num_speakers != o.num_speakers) return false;
        for (int i = 0; i < num_speakers && i < HRTF_MAX_CHANNELS; i++) {
            if (speakers[i].azimuth   != o.speakers[i].azimuth)   return false;
            if (speakers[i].elevation != o.speakers[i].elevation) return false;
            if (speakers[i].distance  != o.speakers[i].distance)  return false;
        }
        return true;
    }
};

Snapshot g_lastSaved;
std::string g_iniPath;

Snapshot capture(const HrtfSharedState* s, bool showCtrl, bool showViz) {
    Snapshot snap;
    snap.show_controls = showCtrl;
    snap.show_3d_viz   = showViz;

    snap.sofa_path     = s->sofa_path;
    snap.master_volume = atomic_load(&s->master_volume);
    snap.hrtf_enabled  = atomic_load(&s->hrtf_enabled);

    snap.hp_eq_path    = s->hp_eq_path;
    snap.hp_eq_enabled = atomic_load(&s->hp_eq_enabled);

    snap.reverb_enabled  = atomic_load(&s->reverb_enabled);
    snap.reverb_decay    = atomic_load(&s->reverb_decay);
    snap.reverb_wet      = atomic_load(&s->reverb_wet);
    snap.reverb_damping  = atomic_load(&s->reverb_damping);
    snap.reverb_predelay = atomic_load(&s->reverb_predelay);

    snap.ir_path = s->ir_file_path;
    snap.ir_wet  = atomic_load(&s->ir_wet);

    snap.room_width      = atomic_load(&s->room_width);
    snap.room_depth      = atomic_load(&s->room_depth);
    snap.room_height     = atomic_load(&s->room_height);
    snap.room_absorption = atomic_load(&s->room_absorption);

    snap.er_level            = atomic_load(&s->er_level);
    snap.crossfeed           = atomic_load(&s->crossfeed);
    snap.bauer_crossfeed     = atomic_load(&s->bauer_crossfeed);
    snap.channel_order_smpte = atomic_load(&s->channel_order_smpte);
    snap.screen_baffling     = atomic_load(&s->screen_baffling);
    snap.front_pinna_boost   = atomic_load(&s->front_pinna_boost);
    snap.near_field_comp     = atomic_load(&s->near_field_comp);
    snap.direct_min_phase    = atomic_load(&s->direct_min_phase);

    snap.num_speakers = atomic_load(&s->num_channels);
    if (snap.num_speakers <= 0 || snap.num_speakers > HRTF_MAX_CHANNELS)
        snap.num_speakers = 12;
    for (int i = 0; i < snap.num_speakers; i++)
        snap.speakers[i] = s->speaker_pos[i];
    return snap;
}

// ---------------------------------------------------------------------------
// Path resolution: find mpv-sofa.ini next to the running executable.
// ---------------------------------------------------------------------------

void ensureIniPath() {
    if (!g_iniPath.empty()) return;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        g_iniPath = "mpv-sofa.ini";  // fallback to cwd
        return;
    }
    std::string full(buf, n);
    auto slash = full.find_last_of("\\/");
    if (slash == std::string::npos) {
        g_iniPath = "mpv-sofa.ini";
    } else {
        g_iniPath = full.substr(0, slash + 1) + "mpv-sofa.ini";
    }
#else
    g_iniPath = "mpv-sofa.ini";
#endif
}

// ---------------------------------------------------------------------------
// Minimal INI parser.  Sections are written as `[name]`, entries as
// `key=value`.  Strings with embedded `=` are not supported (none of our
// values contain it).  Whitespace around `=` is trimmed.
// ---------------------------------------------------------------------------

using KV = std::unordered_map<std::string, std::string>;
using Sections = std::unordered_map<std::string, KV>;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

Sections parseIni(const std::string& path) {
    Sections out;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return out;

    char line[2048];
    std::string section;
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s.front() == '[' && s.back() == ']') {
            section = trim(s.substr(1, s.size() - 2));
            continue;
        }
        auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(s.substr(0, eq));
        std::string val = trim(s.substr(eq + 1));
        out[section][key] = val;
    }
    fclose(f);
    return out;
}

float getF(const KV& kv, const char* key, float def) {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    return (float)atof(it->second.c_str());
}

int getI(const KV& kv, const char* key, int def) {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    return atoi(it->second.c_str());
}

const char* getS(const KV& kv, const char* key, const char* def) {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    return it->second.c_str();
}

void copyStrField(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const char* Settings::filePath() {
    ensureIniPath();
    return g_iniPath.c_str();
}

void Settings::load(HrtfSharedState* state, bool* showControls, bool* show3DViz) {
    ensureIniPath();
    Sections ini = parseIni(g_iniPath);

    // No file or empty parse → keep defaults.  Take the snapshot of the
    // current state so isDirty() starts at false.
    if (!ini.empty()) {
        if (auto it = ini.find("ui"); it != ini.end()) {
            const KV& kv = it->second;
            if (showControls) *showControls = getI(kv, "show_controls", *showControls) != 0;
            if (show3DViz)    *show3DViz    = getI(kv, "show_3d_viz",   *show3DViz)    != 0;
        }
        if (auto it = ini.find("output"); it != ini.end()) {
            const KV& kv = it->second;
            const char* sp = getS(kv, "sofa_path", state->sofa_path);
            if (sp != state->sofa_path) {
                copyStrField(state->sofa_path, sizeof(state->sofa_path), sp);
                atomic_store(&state->sofa_path_changed, 1);
            }
            atomic_store(&state->master_volume, getF(kv, "master_volume", atomic_load(&state->master_volume)));
            atomic_store(&state->hrtf_enabled,  getI(kv, "hrtf_enabled",  atomic_load(&state->hrtf_enabled)));
        }
        if (auto it = ini.find("headphone_eq"); it != ini.end()) {
            const KV& kv = it->second;
            const char* p = getS(kv, "path", state->hp_eq_path);
            if (p != state->hp_eq_path) {
                copyStrField(state->hp_eq_path, sizeof(state->hp_eq_path), p);
                atomic_store(&state->hp_eq_changed, 1);
            }
            atomic_store(&state->hp_eq_enabled, getI(kv, "enabled", atomic_load(&state->hp_eq_enabled)));
        }
        if (auto it = ini.find("reverb"); it != ini.end()) {
            const KV& kv = it->second;
            atomic_store(&state->reverb_enabled,  getI(kv, "enabled",     atomic_load(&state->reverb_enabled)));
            atomic_store(&state->reverb_decay,    getF(kv, "decay",       atomic_load(&state->reverb_decay)));
            atomic_store(&state->reverb_wet,      getF(kv, "wet",         atomic_load(&state->reverb_wet)));
            atomic_store(&state->reverb_damping,  getF(kv, "damping",     atomic_load(&state->reverb_damping)));
            atomic_store(&state->reverb_predelay, getF(kv, "predelay_ms", atomic_load(&state->reverb_predelay)));
            atomic_store(&state->reverb_changed, 1);
        }
        if (auto it = ini.find("reverb_ir"); it != ini.end()) {
            const KV& kv = it->second;
            const char* p = getS(kv, "path", state->ir_file_path);
            if (p != state->ir_file_path) {
                copyStrField(state->ir_file_path, sizeof(state->ir_file_path), p);
                atomic_store(&state->ir_changed, 1);
            }
            atomic_store(&state->ir_wet, getF(kv, "wet", atomic_load(&state->ir_wet)));
        }
        if (auto it = ini.find("room"); it != ini.end()) {
            const KV& kv = it->second;
            atomic_store(&state->room_width,      getF(kv, "width",      atomic_load(&state->room_width)));
            atomic_store(&state->room_depth,      getF(kv, "depth",      atomic_load(&state->room_depth)));
            atomic_store(&state->room_height,     getF(kv, "height",     atomic_load(&state->room_height)));
            atomic_store(&state->room_absorption, getF(kv, "absorption", atomic_load(&state->room_absorption)));
            atomic_store(&state->room_changed, 1);
        }
        if (auto it = ini.find("spatial"); it != ini.end()) {
            const KV& kv = it->second;
            atomic_store(&state->er_level,            getF(kv, "er_level",            atomic_load(&state->er_level)));
            atomic_store(&state->crossfeed,           getF(kv, "crossfeed",           atomic_load(&state->crossfeed)));
            atomic_store(&state->bauer_crossfeed,     getF(kv, "bauer_crossfeed",     atomic_load(&state->bauer_crossfeed)));
            atomic_store(&state->channel_order_smpte, getI(kv, "channel_order_smpte", atomic_load(&state->channel_order_smpte)));
            atomic_store(&state->screen_baffling,     getI(kv, "screen_baffling",     atomic_load(&state->screen_baffling)));
            atomic_store(&state->front_pinna_boost,   getI(kv, "front_pinna_boost",   atomic_load(&state->front_pinna_boost)));
            atomic_store(&state->near_field_comp,     getI(kv, "near_field_comp",     atomic_load(&state->near_field_comp)));
            atomic_store(&state->direct_min_phase,    getI(kv, "direct_min_phase",    atomic_load(&state->direct_min_phase)));
        }
        if (auto it = ini.find("speakers"); it != ini.end()) {
            const KV& kv = it->second;
            int n = getI(kv, "count", atomic_load(&state->num_channels));
            if (n < 0) n = 0;
            if (n > HRTF_MAX_CHANNELS) n = HRTF_MAX_CHANNELS;
            char key[64];
            for (int i = 0; i < n; i++) {
                snprintf(key, sizeof(key), "speaker_%d_az", i);
                state->speaker_pos[i].azimuth   = getF(kv, key, state->speaker_pos[i].azimuth);
                snprintf(key, sizeof(key), "speaker_%d_el", i);
                state->speaker_pos[i].elevation = getF(kv, key, state->speaker_pos[i].elevation);
                snprintf(key, sizeof(key), "speaker_%d_d", i);
                state->speaker_pos[i].distance  = getF(kv, key, state->speaker_pos[i].distance);
            }
            atomic_store(&state->speaker_pos_changed, 1);
        }
    }

    g_lastSaved = capture(state,
                           showControls ? *showControls : false,
                           show3DViz    ? *show3DViz    : false);
}

bool Settings::save(const HrtfSharedState* state, bool showControls, bool show3DViz) {
    ensureIniPath();
    FILE* f = fopen(g_iniPath.c_str(), "w");
    if (!f) {
        fprintf(stderr, "[Settings] Failed to open '%s' for writing\n", g_iniPath.c_str());
        return false;
    }

    fprintf(f, "; mpv-sofa configuration\n");
    fprintf(f, "; Auto-generated.  Edit while the app is closed.\n\n");

    fprintf(f, "[ui]\n");
    fprintf(f, "show_controls=%d\n", showControls ? 1 : 0);
    fprintf(f, "show_3d_viz=%d\n",   show3DViz    ? 1 : 0);
    fprintf(f, "\n");

    fprintf(f, "[output]\n");
    fprintf(f, "sofa_path=%s\n",     state->sofa_path);
    fprintf(f, "master_volume=%g\n", atomic_load(&state->master_volume));
    fprintf(f, "hrtf_enabled=%d\n",  atomic_load(&state->hrtf_enabled));
    fprintf(f, "\n");

    fprintf(f, "[headphone_eq]\n");
    fprintf(f, "path=%s\n",    state->hp_eq_path);
    fprintf(f, "enabled=%d\n", atomic_load(&state->hp_eq_enabled));
    fprintf(f, "\n");

    fprintf(f, "[reverb]\n");
    fprintf(f, "enabled=%d\n",     atomic_load(&state->reverb_enabled));
    fprintf(f, "decay=%g\n",       atomic_load(&state->reverb_decay));
    fprintf(f, "wet=%g\n",         atomic_load(&state->reverb_wet));
    fprintf(f, "damping=%g\n",     atomic_load(&state->reverb_damping));
    fprintf(f, "predelay_ms=%g\n", atomic_load(&state->reverb_predelay));
    fprintf(f, "\n");

    fprintf(f, "[reverb_ir]\n");
    fprintf(f, "path=%s\n", state->ir_file_path);
    fprintf(f, "wet=%g\n",  atomic_load(&state->ir_wet));
    fprintf(f, "\n");

    fprintf(f, "[room]\n");
    fprintf(f, "width=%g\n",      atomic_load(&state->room_width));
    fprintf(f, "depth=%g\n",      atomic_load(&state->room_depth));
    fprintf(f, "height=%g\n",     atomic_load(&state->room_height));
    fprintf(f, "absorption=%g\n", atomic_load(&state->room_absorption));
    fprintf(f, "\n");

    fprintf(f, "[spatial]\n");
    fprintf(f, "er_level=%g\n",            atomic_load(&state->er_level));
    fprintf(f, "crossfeed=%g\n",           atomic_load(&state->crossfeed));
    fprintf(f, "bauer_crossfeed=%g\n",     atomic_load(&state->bauer_crossfeed));
    fprintf(f, "channel_order_smpte=%d\n", atomic_load(&state->channel_order_smpte));
    fprintf(f, "screen_baffling=%d\n",     atomic_load(&state->screen_baffling));
    fprintf(f, "front_pinna_boost=%d\n",   atomic_load(&state->front_pinna_boost));
    fprintf(f, "near_field_comp=%d\n",     atomic_load(&state->near_field_comp));
    fprintf(f, "direct_min_phase=%d\n",    atomic_load(&state->direct_min_phase));
    fprintf(f, "\n");

    int n = atomic_load(&state->num_channels);
    if (n < 0) n = 0;
    if (n > HRTF_MAX_CHANNELS) n = HRTF_MAX_CHANNELS;
    fprintf(f, "[speakers]\n");
    fprintf(f, "count=%d\n", n);
    for (int i = 0; i < n; i++) {
        fprintf(f, "speaker_%d_az=%g\n", i, state->speaker_pos[i].azimuth);
        fprintf(f, "speaker_%d_el=%g\n", i, state->speaker_pos[i].elevation);
        fprintf(f, "speaker_%d_d=%g\n",  i, state->speaker_pos[i].distance);
    }

    fclose(f);
    g_lastSaved = capture(state, showControls, show3DViz);
    return true;
}

bool Settings::isDirty(const HrtfSharedState* state, bool showControls, bool show3DViz) {
    return !(capture(state, showControls, show3DViz) == g_lastSaved);
}

void Settings::resetToDefaults(HrtfSharedState* state, bool* showControls, bool* show3DViz) {
    if (showControls) *showControls = false;
    if (show3DViz)    *show3DViz    = false;

    state->sofa_path[0] = '\0';
    atomic_store(&state->sofa_path_changed, 1);
    atomic_store(&state->master_volume, 1.0f);
    atomic_store(&state->hrtf_enabled, 1);

    state->hp_eq_path[0] = '\0';
    atomic_store(&state->hp_eq_changed, 1);
    atomic_store(&state->hp_eq_enabled, 1);

    atomic_store(&state->reverb_enabled,  1);
    atomic_store(&state->reverb_decay,    0.45f);
    atomic_store(&state->reverb_wet,      0.15f);
    atomic_store(&state->reverb_damping,  0.5f);
    atomic_store(&state->reverb_predelay, 10.0f);
    atomic_store(&state->reverb_changed, 1);

    state->ir_file_path[0] = '\0';
    atomic_store(&state->ir_changed, 1);
    atomic_store(&state->ir_wet, 0.35f);

    atomic_store(&state->room_width,      6.5f);
    atomic_store(&state->room_depth,      5.0f);
    atomic_store(&state->room_height,     2.7f);
    atomic_store(&state->room_absorption, 0.3f);
    atomic_store(&state->room_changed, 1);

    atomic_store(&state->er_level,            0.3f);
    atomic_store(&state->crossfeed,           0.0f);
    atomic_store(&state->bauer_crossfeed,     0.15f);
    atomic_store(&state->channel_order_smpte, 0);
    atomic_store(&state->screen_baffling,     0);
    atomic_store(&state->front_pinna_boost,   1);
    atomic_store(&state->near_field_comp,     1);
    atomic_store(&state->direct_min_phase,    0);

    hrtf_shared_state_init_714(state);
    atomic_store(&state->speaker_pos_changed, 1);
}
