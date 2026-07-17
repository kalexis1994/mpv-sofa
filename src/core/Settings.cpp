#include "Settings.h"
#include "SharedState.h"
#include "audio/MpvPlayer.h"

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

    // Auto-selected language preferences.
    std::string pref_audio_lang;
    std::string pref_sub_lang;

    // Last room preset chosen (UI hint only).
    int room_preset = 1;

    // Subtitle style.
    Settings::SubtitleStyle sub_style;

    // Cinema grain.
    Settings::CinemaGrain grain;

    // Display / HDR.
    Settings::DisplayConfig display;

    // Playback (audio-delay).
    Settings::PlaybackConfig playback;

    // OS-level window presentation mode.
    int window_mode = 0;   // 0=Fullscreen, 1=Borderless, 2=Windowed

    // HaloSound media server.
    Settings::ServerConfig server;

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
        if (pref_audio_lang != o.pref_audio_lang) return false;
        if (pref_sub_lang   != o.pref_sub_lang)   return false;
        if (room_preset     != o.room_preset)     return false;
        // Subtitle style equality
        const auto& a = sub_style;
        const auto& b = o.sub_style;
        if (a.font != b.font || a.sizePt != b.sizePt ||
            a.borderSize != b.borderSize || a.shadowOffset != b.shadowOffset ||
            a.bold != b.bold || a.marginY != b.marginY || a.pos != b.pos)
            return false;
        for (int i = 0; i < 4; i++) {
            if (a.color[i]       != b.color[i])       return false;
            if (a.borderColor[i] != b.borderColor[i]) return false;
            if (a.shadowColor[i] != b.shadowColor[i]) return false;
            if (a.backColor[i]   != b.backColor[i])   return false;
        }
        if (grain.enabled     != o.grain.enabled)     return false;
        if (grain.stock       != o.grain.stock)       return false;
        if (grain.intensity   != o.grain.intensity)   return false;
        if (grain.grainSize   != o.grain.grainSize)   return false;
        if (grain.lumAdaptive != o.grain.lumAdaptive) return false;
        if (grain.chroma      != o.grain.chroma)      return false;
        if (display.mode      != o.display.mode)      return false;
        if (display.peakNits  != o.display.peakNits)  return false;
        if (display.toneAlg   != o.display.toneAlg)   return false;
        if (display.gamutMode != o.display.gamutMode) return false;
        if (display.panscan   != o.display.panscan)   return false;
        if (playback.audioDelay != o.playback.audioDelay) return false;
        if (window_mode != o.window_mode) return false;
        if (server.enabled  != o.server.enabled)  return false;
        if (server.mediaDir != o.server.mediaDir) return false;
        if (server.port     != o.server.port)     return false;
        return true;
    }
};

Snapshot g_lastSaved;
std::string g_iniPath;

// Live preferences, kept outside Snapshot so the UI can read/write them
// without dragging the full snapshot around.
std::string g_prefAudioLang;
std::string g_prefSubLang;
int         g_roomPreset = 1;   // 0=studio, 1=home, 2=cinema, 3=concert
Settings::SubtitleStyle  g_subStyle;
Settings::CinemaGrain    g_grain;
Settings::DisplayConfig  g_display;
Settings::PlaybackConfig g_playback;
Settings::ServerConfig   g_server;
int                      g_windowMode = 0;   // 0=Fullscreen
std::vector<std::string> g_recents;
constexpr size_t kMaxRecents = 12;

// Format an RGBA float[4] as the colour string mpv expects.  Note the
// historical mpv convention is "#AARRGGBB" with alpha *first* — passing
// "#RRGGBBAA" makes mpv interpret your alpha as red and so on.
std::string toHexRgba(const float c[4]) {
    auto clamp8 = [](float v) {
        int i = (int)(v * 255.0f + 0.5f);
        if (i < 0)   i = 0;
        if (i > 255) i = 255;
        return i;
    };
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X",
             clamp8(c[3]), clamp8(c[0]), clamp8(c[1]), clamp8(c[2]));
    return buf;
}

// Inverse: parse "#AARRGGBB" or "#RRGGBB" into rgba[4].
void parseHexRgba(const char* s, float out[4]) {
    if (!s || s[0] != '#') return;
    size_t len = strlen(s);
    unsigned r=0, g=0, b=0, a=255;
    if (len >= 9 && sscanf(s, "#%2x%2x%2x%2x", &a, &r, &g, &b) == 4) {
        // matched #AARRGGBB
    } else if (len >= 7 && sscanf(s, "#%2x%2x%2x", &r, &g, &b) == 3) {
        // matched #RRGGBB (no alpha)
        a = 255;
    } else {
        return;
    }
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
}

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

    snap.pref_audio_lang = g_prefAudioLang;
    snap.pref_sub_lang   = g_prefSubLang;
    snap.room_preset     = g_roomPreset;
    snap.sub_style       = g_subStyle;
    snap.grain           = g_grain;
    snap.display         = g_display;
    snap.playback        = g_playback;
    snap.window_mode     = g_windowMode;
    snap.server          = g_server;
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
            g_roomPreset = getI(kv, "preset", g_roomPreset);
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
        if (auto it = ini.find("preferences"); it != ini.end()) {
            const KV& kv = it->second;
            g_prefAudioLang = getS(kv, "audio_lang", "");
            g_prefSubLang   = getS(kv, "sub_lang",   "");
        }
        if (auto it = ini.find("display"); it != ini.end()) {
            const KV& kv = it->second;
            g_display.mode      = getI(kv, "mode",       g_display.mode);
            g_display.peakNits  = getF(kv, "peak_nits",  g_display.peakNits);
            g_display.toneAlg   = getI(kv, "tone_alg",   g_display.toneAlg);
            g_display.gamutMode = getI(kv, "gamut_mode", g_display.gamutMode);
            g_display.panscan   = getF(kv, "panscan",    g_display.panscan);
        }
        if (auto it = ini.find("playback"); it != ini.end()) {
            const KV& kv = it->second;
            g_playback.audioDelay = getF(kv, "audio_delay", g_playback.audioDelay);
        }
        if (auto it = ini.find("window"); it != ini.end()) {
            const KV& kv = it->second;
            g_windowMode = getI(kv, "mode", g_windowMode);
            if (g_windowMode < 0 || g_windowMode > 2) g_windowMode = 0;
        }
        if (auto it = ini.find("server"); it != ini.end()) {
            const KV& kv = it->second;
            g_server.enabled  = getI(kv, "enabled", g_server.enabled ? 1 : 0) != 0;
            g_server.mediaDir = getS(kv, "media_dir", g_server.mediaDir.c_str());
            g_server.port     = getI(kv, "port", g_server.port);
            if (g_server.port < 1 || g_server.port > 65534) g_server.port = 8080;
        }
        if (auto it = ini.find("recent"); it != ini.end()) {
            const KV& kv = it->second;
            g_recents.clear();
            int n = getI(kv, "count", 0);
            if (n < 0) n = 0;
            if ((size_t)n > kMaxRecents) n = (int)kMaxRecents;
            char key[32];
            for (int i = 0; i < n; i++) {
                snprintf(key, sizeof(key), "path_%d", i);
                std::string p = getS(kv, key, "");
                if (!p.empty()) g_recents.push_back(std::move(p));
            }
        }
        if (auto it = ini.find("cinema_grain"); it != ini.end()) {
            const KV& kv = it->second;
            g_grain.enabled     = getI(kv, "enabled", g_grain.enabled ? 1 : 0) != 0;
            g_grain.stock       = getI(kv, "stock",        g_grain.stock);
            g_grain.intensity   = getF(kv, "intensity",    g_grain.intensity);
            g_grain.grainSize   = getF(kv, "grain_size",   g_grain.grainSize);
            g_grain.lumAdaptive = getF(kv, "lum_adaptive", g_grain.lumAdaptive);
            g_grain.chroma      = getF(kv, "chroma",       g_grain.chroma);
        }
        if (auto it = ini.find("subtitle_style"); it != ini.end()) {
            const KV& kv = it->second;
            g_subStyle.font          = getS(kv, "font", g_subStyle.font.c_str());
            g_subStyle.sizePt        = getF(kv, "size_pt",        g_subStyle.sizePt);
            g_subStyle.borderSize    = getF(kv, "border_size",    g_subStyle.borderSize);
            g_subStyle.shadowOffset  = getF(kv, "shadow_offset",  g_subStyle.shadowOffset);
            g_subStyle.bold          = getI(kv, "bold",     g_subStyle.bold ? 1 : 0) != 0;
            g_subStyle.marginY       = getI(kv, "margin_y", g_subStyle.marginY);
            g_subStyle.pos           = getI(kv, "pos",      g_subStyle.pos);
            parseHexRgba(getS(kv, "color",        ""), g_subStyle.color);
            parseHexRgba(getS(kv, "border_color", ""), g_subStyle.borderColor);
            parseHexRgba(getS(kv, "shadow_color", ""), g_subStyle.shadowColor);
            parseHexRgba(getS(kv, "back_color",   ""), g_subStyle.backColor);
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
    fprintf(f, "preset=%d\n",     g_roomPreset);
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
    fprintf(f, "\n");

    fprintf(f, "[preferences]\n");
    fprintf(f, "audio_lang=%s\n", g_prefAudioLang.c_str());
    fprintf(f, "sub_lang=%s\n",   g_prefSubLang.c_str());
    fprintf(f, "\n");

    fprintf(f, "[display]\n");
    fprintf(f, "mode=%d\n",       g_display.mode);
    fprintf(f, "peak_nits=%g\n",  g_display.peakNits);
    fprintf(f, "tone_alg=%d\n",   g_display.toneAlg);
    fprintf(f, "gamut_mode=%d\n", g_display.gamutMode);
    fprintf(f, "panscan=%g\n",    g_display.panscan);
    fprintf(f, "\n");

    fprintf(f, "[playback]\n");
    fprintf(f, "audio_delay=%g\n", g_playback.audioDelay);
    fprintf(f, "\n");

    fprintf(f, "[window]\n");
    fprintf(f, "mode=%d\n", g_windowMode);
    fprintf(f, "\n");

    fprintf(f, "[server]\n");
    fprintf(f, "enabled=%d\n",   g_server.enabled ? 1 : 0);
    fprintf(f, "media_dir=%s\n", g_server.mediaDir.c_str());
    fprintf(f, "port=%d\n",      g_server.port);
    fprintf(f, "\n");

    fprintf(f, "[recent]\n");
    fprintf(f, "count=%d\n", (int)g_recents.size());
    for (size_t i = 0; i < g_recents.size(); i++)
        fprintf(f, "path_%zu=%s\n", i, g_recents[i].c_str());
    fprintf(f, "\n");

    fprintf(f, "[cinema_grain]\n");
    fprintf(f, "enabled=%d\n",       g_grain.enabled ? 1 : 0);
    fprintf(f, "stock=%d\n",         g_grain.stock);
    fprintf(f, "intensity=%g\n",     g_grain.intensity);
    fprintf(f, "grain_size=%g\n",    g_grain.grainSize);
    fprintf(f, "lum_adaptive=%g\n",  g_grain.lumAdaptive);
    fprintf(f, "chroma=%g\n",        g_grain.chroma);
    fprintf(f, "\n");

    fprintf(f, "[subtitle_style]\n");
    fprintf(f, "font=%s\n",          g_subStyle.font.c_str());
    fprintf(f, "size_pt=%g\n",       g_subStyle.sizePt);
    fprintf(f, "color=%s\n",         toHexRgba(g_subStyle.color).c_str());
    fprintf(f, "border_color=%s\n",  toHexRgba(g_subStyle.borderColor).c_str());
    fprintf(f, "border_size=%g\n",   g_subStyle.borderSize);
    fprintf(f, "shadow_color=%s\n",  toHexRgba(g_subStyle.shadowColor).c_str());
    fprintf(f, "shadow_offset=%g\n", g_subStyle.shadowOffset);
    fprintf(f, "back_color=%s\n",    toHexRgba(g_subStyle.backColor).c_str());
    fprintf(f, "bold=%d\n",          g_subStyle.bold ? 1 : 0);
    fprintf(f, "margin_y=%d\n",      g_subStyle.marginY);
    fprintf(f, "pos=%d\n",           g_subStyle.pos);

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

    g_prefAudioLang.clear();
    g_prefSubLang.clear();
    g_roomPreset = 1;
    g_subStyle = SubtitleStyle{};
    g_grain    = CinemaGrain{};
    g_display  = DisplayConfig{};
    g_playback   = PlaybackConfig{};
    g_windowMode = 0;   // Fullscreen
    g_server     = ServerConfig{};
    g_recents.clear();
}

const std::string& Settings::preferredAudioLang() { return g_prefAudioLang; }
const std::string& Settings::preferredSubLang()   { return g_prefSubLang;   }
void Settings::setPreferredAudioLang(std::string lang) { g_prefAudioLang = std::move(lang); }
void Settings::setPreferredSubLang  (std::string lang) { g_prefSubLang   = std::move(lang); }

int  Settings::roomPreset()             { return g_roomPreset; }
void Settings::setRoomPreset(int idx)   { g_roomPreset = idx;  }

const Settings::SubtitleStyle& Settings::subtitleStyle() { return g_subStyle; }

void Settings::setSubtitleStyle(const SubtitleStyle& s) { g_subStyle = s; }

const Settings::CinemaGrain& Settings::cinemaGrain() { return g_grain; }
void Settings::setCinemaGrain(const CinemaGrain& g)  { g_grain = g; }

const Settings::DisplayConfig& Settings::displayConfig() { return g_display; }
void Settings::setDisplayConfig(const DisplayConfig& c)  { g_display = c; }

void Settings::applyDisplayConfigToPlayer(MpvPlayer* p) {
    if (!p) return;
    const DisplayConfig& d = g_display;

    // mpv accepts "auto" for any of these to fall back to its own
    // detection / default behaviour.  Mode 0 (Auto) clears all overrides
    // so the user gets vanilla mpv defaults; the other modes pin the
    // pipeline to a specific target colour space.
    static const char* primMap[] = { "auto", "bt.709", "bt.2020"  };
    static const char* trcMap[]  = { "auto", "bt.1886", "pq"       };
    static const char* toneMap[] = { "bt.2390", "mobius", "hable",
                                      "reinhard", "clip" };
    static const char* gamutMap[] = { "auto", "perceptual",
                                       "relative", "saturation" };

    int idx = (d.mode < 0 || d.mode > 2) ? 0 : d.mode;
    int ti  = (d.toneAlg   < 0 || d.toneAlg   > 4) ? 0 : d.toneAlg;
    int gi  = (d.gamutMode < 0 || d.gamutMode > 3) ? 0 : d.gamutMode;

    p->setStringProperty("target-prim",         primMap[idx]);
    p->setStringProperty("target-trc",          trcMap[idx]);
    p->setStringProperty("tone-mapping",        toneMap[ti]);
    p->setStringProperty("gamut-mapping-mode",  gamutMap[gi]);

    // Peak nits only meaningful for the HDR10 passthrough mode; the
    // other modes let mpv pick (auto = use input or sensible default).
    if (idx == 2) {
        p->setDoubleProperty("target-peak", d.peakNits);
    } else {
        p->setStringProperty("target-peak", "auto");
    }

    // Black-bar / letterbox handling.
    p->setDoubleProperty("panscan", d.panscan);
}

const Settings::PlaybackConfig& Settings::playbackConfig() { return g_playback; }
void Settings::setPlaybackConfig(const PlaybackConfig& c)  { g_playback = c; }

void Settings::applyPlaybackConfigToPlayer(MpvPlayer* p) {
    if (!p) return;
    p->setDoubleProperty("audio-delay", g_playback.audioDelay);
}

const Settings::ServerConfig& Settings::serverConfig() { return g_server; }
void Settings::setServerConfig(const ServerConfig& c)  {
    g_server = c;
    if (g_server.port < 1 || g_server.port > 65534) g_server.port = 8080;
}

int  Settings::windowMode()         { return g_windowMode; }
void Settings::setWindowMode(int m) {
    if (m < 0 || m > 2) m = 0;
    g_windowMode = m;
}

const std::vector<std::string>& Settings::recentFiles() { return g_recents; }

void Settings::pushRecent(const std::string& fullPath) {
    if (fullPath.empty()) return;
    // Dedup: drop any prior entry pointing at the same file so the new
    // push lands at the front and doesn't multiply.
    for (auto it = g_recents.begin(); it != g_recents.end(); ) {
        if (*it == fullPath) it = g_recents.erase(it);
        else                 ++it;
    }
    g_recents.insert(g_recents.begin(), fullPath);
    if (g_recents.size() > kMaxRecents)
        g_recents.resize(kMaxRecents);
}

void Settings::clearRecents() {
    g_recents.clear();
}

void Settings::applyCinemaGrainToPlayer(MpvPlayer* p) {
    if (!p) return;
    ensureIniPath();

    // Locate the executable directory (where mpv-sofa.ini lives).
    std::string baseDir = g_iniPath;
    auto slash = baseDir.find_last_of("\\/");
    baseDir = (slash != std::string::npos)
                 ? baseDir.substr(0, slash + 1)
                 : std::string();

    if (!g_grain.enabled) {
        p->setStringProperty("glsl-shaders", "");
        return;
    }

    // Read the shader template and substitute {{KEY}} placeholders with
    // the current parameter values.  We can't use //!PARAM / glsl-shader-
    // opts because libplacebo's PARAM directive doesn't parse under our
    // vo=libmpv pipeline; baking the constants into the source via
    // #define is the next-best mechanism and matches what most user-
    // shader collections do for tunable params.
    const std::string templatePath = baseDir + "assets/shaders/cinema_grain.glsl";
    const std::string runtimePath  = baseDir + "cinema_grain_runtime.glsl";

    std::string source;
    if (FILE* f = fopen(templatePath.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            source.resize((size_t)sz);
            (void)fread(&source[0], 1, (size_t)sz, f);
        }
        fclose(f);
    }
    if (source.empty()) {
        fprintf(stderr, "[grain] template missing: %s\n", templatePath.c_str());
        return;
    }

    auto replaceAll = [&](const std::string& key, float value) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6f", value);
        std::string ph  = "{{" + key + "}}";
        std::string val = buf;
        size_t pos = 0;
        while ((pos = source.find(ph, pos)) != std::string::npos) {
            source.replace(pos, ph.size(), val);
            pos += val.size();
        }
    };
    replaceAll("INTENSITY",    g_grain.intensity);
    replaceAll("GRAIN_SIZE",   g_grain.grainSize);
    replaceAll("LUM_ADAPTIVE", g_grain.lumAdaptive);
    replaceAll("CHROMA",       g_grain.chroma);

    if (FILE* f = fopen(runtimePath.c_str(), "wb")) {
        fwrite(source.data(), 1, source.size(), f);
        fclose(f);
    } else {
        fprintf(stderr, "[grain] cannot write runtime shader: %s\n",
                runtimePath.c_str());
        return;
    }

    // Toggle to empty first so mpv treats the same path as a fresh load
    // and re-reads our newly-substituted source.
    p->setStringProperty("glsl-shaders", "");
    p->setStringProperty("glsl-shaders", runtimePath.c_str());
}

void Settings::tickCinemaGrain(MpvPlayer* /*p*/) {
    // Kept as a public no-op so existing callers don't break.  Temporal
    // variation now comes from libplacebo's `frame` builtin inside the
    // shader, no per-frame property update needed (touching shader-opts
    // every frame forces a shader recompile and tanks audio sync).
}

void Settings::applySubtitleStyleToPlayer(MpvPlayer* p) {
    if (!p) return;
    const SubtitleStyle& s = g_subStyle;

    // Most MKV subtitles are ASS, which carries its own font / colour /
    // border styling.  By default mpv respects that ("sub-ass-override=no")
    // and the user's --sub-color / --sub-font etc. are silently ignored.
    // Forcing the override makes our settings apply across both ASS and
    // plain-text formats (SRT / WebVTT / SubRip).
    p->setStringProperty("sub-ass-override", "force");

    // Font: empty string means "leave mpv default"; mpv accepts "" to
    // mean "auto-pick a sans-serif", so passing through is fine either way.
    p->setStringProperty("sub-font",          s.font.c_str());
    p->setDoubleProperty("sub-font-size",     s.sizePt);
    p->setStringProperty("sub-color",         toHexRgba(s.color).c_str());
    p->setStringProperty("sub-border-color",  toHexRgba(s.borderColor).c_str());
    p->setDoubleProperty("sub-border-size",   s.borderSize);
    p->setStringProperty("sub-shadow-color",  toHexRgba(s.shadowColor).c_str());
    p->setDoubleProperty("sub-shadow-offset", s.shadowOffset);
    p->setStringProperty("sub-back-color",    toHexRgba(s.backColor).c_str());
    p->setFlagProperty  ("sub-bold",          s.bold);
    p->setIntProperty   ("sub-margin-y",      s.marginY);
    p->setIntProperty   ("sub-pos",           s.pos);
}

bool Settings::langMatches(const std::string& trackLang,
                            const std::string& prefLang) {
    if (prefLang.empty() || trackLang.empty()) return false;
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    const std::string a = lower(trackLang);
    const std::string b = lower(prefLang);
    if (a == b) return true;

    // Common 3 ↔ 2 letter aliases.  Both legs are matched so a track tagged
    // "es" still matches a "spa" preference and vice-versa.
    static const std::pair<const char*, const char*> pairs[] = {
        {"eng","en"}, {"spa","es"}, {"fre","fr"}, {"fra","fr"},
        {"ger","de"}, {"deu","de"}, {"ita","it"}, {"por","pt"},
        {"jpn","ja"}, {"kor","ko"}, {"chi","zh"}, {"zho","zh"},
        {"rus","ru"}, {"ara","ar"}, {"hin","hi"}, {"dut","nl"},
        {"nld","nl"}, {"swe","sv"}, {"nor","no"}, {"dan","da"},
        {"fin","fi"}, {"pol","pl"}, {"tur","tr"},
    };
    for (auto& p : pairs) {
        if ((a == p.first && b == p.second) ||
            (a == p.second && b == p.first))
            return true;
    }
    return false;
}
