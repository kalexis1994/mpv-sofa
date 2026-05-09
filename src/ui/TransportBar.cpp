#include "TransportBar.h"
#include "audio/MpvPlayer.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <IconsLucide.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

TransportBar::TransportBar(MpvPlayer* player) : m_player(player) {}

void TransportBar::formatTime(double seconds, char* buf, int bufSize) {
    if (seconds < 0 || !std::isfinite(seconds)) seconds = 0;
    int h = (int)(seconds / 3600);
    int m = (int)(seconds / 60) % 60;
    int s = (int)seconds % 60;

    if (h > 0)
        snprintf(buf, bufSize, "%d:%02d:%02d", h, m, s);
    else
        snprintf(buf, bufSize, "%02d:%02d", m, s);
}

void TransportBar::render() {
    ImGui::Begin("Transport");
    renderContent();
    ImGui::End();
}

namespace {

// Custom timeline.  Returns true and writes the new position to
// `*outPosSec` when the user clicks/drags to seek, so the caller can issue
// the actual seek.  Renders progress fill, chapter ticks, and a hover
// indicator + tooltip showing the time at the hovered x.
bool DrawTimeline(double position, double duration,
                   const std::vector<Chapter>& chapters,
                   double* outPosSec) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float  w   = ImGui::GetContentRegionAvail().x;
    float  h   = 14.0f;
    if (w < 16.0f) w = 16.0f;

    ImGui::InvisibleButton("##timeline", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    ImGuiStyle& st = ImGui::GetStyle();
    ImU32 colTrack    = ImGui::GetColorU32(ImGuiCol_FrameBg);
    ImU32 colFill     = ImGui::GetColorU32(ImGuiCol_SliderGrab);
    ImU32 colChapter  = ImGui::GetColorU32(ImGuiCol_Text, 0.55f);
    ImU32 colHover    = ImGui::GetColorU32(ImGuiCol_Text, 0.75f);
    ImU32 colBorder   = ImGui::GetColorU32(ImGuiCol_Border);

    float trackTop    = pos.y + (h - 8.0f) * 0.5f;
    float trackBot    = trackTop + 8.0f;
    float rounding    = 3.0f;

    // Track background.
    dl->AddRectFilled(ImVec2(pos.x, trackTop),
                      ImVec2(pos.x + w, trackBot), colTrack, rounding);
    dl->AddRect(ImVec2(pos.x, trackTop),
                ImVec2(pos.x + w, trackBot), colBorder, rounding, 0, 1.0f);

    // Fill up to current position.
    if (duration > 0) {
        float frac = (float)(position / duration);
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        dl->AddRectFilled(ImVec2(pos.x, trackTop),
                          ImVec2(pos.x + frac * w, trackBot),
                          colFill, rounding);
    }

    // Chapter ticks (full timeline height to stand out).
    if (duration > 0) {
        for (const auto& ch : chapters) {
            float frac = (float)(ch.time / duration);
            if (frac <= 0 || frac >= 1) continue;
            float x = pos.x + frac * w;
            dl->AddLine(ImVec2(x, pos.y),
                        ImVec2(x, pos.y + h), colChapter, 1.0f);
        }
    }

    // Position indicator (small circle on the fill edge).
    if (duration > 0) {
        float frac = (float)(position / duration);
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        float cx = pos.x + frac * w;
        float cy = (trackTop + trackBot) * 0.5f;
        dl->AddCircleFilled(ImVec2(cx, cy), 5.0f, colFill);
        dl->AddCircle      (ImVec2(cx, cy), 5.0f, colBorder, 0, 1.0f);
    }

    // Hover indicator + tooltip with time at x.
    if (duration > 0 && (hovered || active)) {
        float mx = ImGui::GetIO().MousePos.x;
        float frac = (mx - pos.x) / w;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        float vx = pos.x + frac * w;

        dl->AddLine(ImVec2(vx, pos.y),
                    ImVec2(vx, pos.y + h), colHover, 1.5f);

        double tHover = frac * duration;
        char buf[32];
        int hh = (int)(tHover / 3600);
        int mm = (int)(tHover / 60) % 60;
        int ss = (int)tHover % 60;
        if (hh > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", hh, mm, ss);
        else        snprintf(buf, sizeof(buf), "%02d:%02d", mm, ss);

        // Optionally annotate with chapter name at this position.
        const Chapter* hoveredChapter = nullptr;
        for (const auto& ch : chapters) {
            if (ch.time <= tHover + 1e-3) hoveredChapter = &ch;
            else break;
        }
        if (hoveredChapter && !hoveredChapter->title.empty())
            ImGui::SetTooltip("%s\n%s", buf, hoveredChapter->title.c_str());
        else
            ImGui::SetTooltip("%s", buf);

        if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            *outPosSec = (double)frac * duration;
            return true;
        }
    }
    return false;
}

} // anonymous namespace

void TransportBar::renderContent() {
    if (!m_player) {
        ImGui::Text("No player available");
        return;
    }

    ImGuiStyle& style = ImGui::GetStyle();

    // ---------- Row 1: filename + chapter status ----------
    {
        std::string filename = m_player->getFilename();
        if (!filename.empty()) {
            ImGui::TextDisabled("%s", filename.c_str());
            const auto& chapters = m_player->getChapters();
            if (!chapters.empty()) {
                int cur = m_player->getCurrentChapterIndex();
                char info[128];
                if (cur >= 0 && !chapters[cur].title.empty()) {
                    snprintf(info, sizeof(info), ICON_LC_BOOKMARK "  %d/%zu  %s",
                             cur + 1, chapters.size(), chapters[cur].title.c_str());
                } else if (cur >= 0) {
                    snprintf(info, sizeof(info), ICON_LC_BOOKMARK "  %d/%zu",
                             cur + 1, chapters.size());
                } else {
                    snprintf(info, sizeof(info), ICON_LC_BOOKMARK "  %zu chapters",
                             chapters.size());
                }
                float infoW = ImGui::CalcTextSize(info).x;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x +
                                 ImGui::GetCursorPosX() - infoW);
                ImGui::TextDisabled("%s", info);
            }
        }
    }

    // ---------- Row 2: timeline + time labels ----------
    {
        double duration = m_player->getDuration();
        double position = m_player->getPosition();
        double newPos   = 0.0;
        if (DrawTimeline(position, duration,
                          m_player->getChapters(), &newPos)) {
            m_player->seek(newPos);
        }

        char posStr[32], durStr[32];
        formatTime(position, posStr, sizeof(posStr));
        formatTime(duration, durStr, sizeof(durStr));
        ImGui::Text("%s", posStr);
        float durW = ImGui::CalcTextSize(durStr).x;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x +
                         ImGui::GetCursorPosX() - durW);
        ImGui::TextDisabled("%s", durStr);
    }

    ImGui::Spacing();

    // ---------- Row 3: controls ----------
    const float btnW    = 32.0f;
    const float spacing = 4.0f;

    // Previous chapter
    bool hasChapters = !m_player->getChapters().empty();
    ImGui::BeginDisabled(!hasChapters);
    if (ImGui::Button(ICON_LC_SKIP_BACK, ImVec2(btnW, 0)))
        m_player->prevChapter();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous chapter");
    ImGui::EndDisabled();
    ImGui::SameLine(0, spacing);

    // Rewind 10 s
    if (ImGui::Button(ICON_LC_REWIND, ImVec2(btnW, 0)))
        m_player->seekRelative(-10.0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back 10 s");
    ImGui::SameLine(0, spacing);

    // Play / Pause
    const char* playLbl = m_player->isPaused() ? ICON_LC_PLAY : ICON_LC_PAUSE;
    if (ImGui::Button(playLbl, ImVec2(btnW, 0)))
        m_player->togglePause();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(m_player->isPaused() ? "Play (Space)" : "Pause (Space)");
    ImGui::SameLine(0, spacing);

    // Forward 10 s
    if (ImGui::Button(ICON_LC_FAST_FORWARD, ImVec2(btnW, 0)))
        m_player->seekRelative(10.0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward 10 s");
    ImGui::SameLine(0, spacing);

    // Next chapter
    ImGui::BeginDisabled(!hasChapters);
    if (ImGui::Button(ICON_LC_SKIP_FORWARD, ImVec2(btnW, 0)))
        m_player->nextChapter();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next chapter");
    ImGui::EndDisabled();
    ImGui::SameLine(0, spacing * 4);

    // Volume — mute toggle + slider
    {
        bool muted = m_player->isMuted();
        const char* volIcon = muted              ? ICON_LC_VOLUME_X
                            : (m_player->getVolume() < 1.0f ? ICON_LC_VOLUME
                            : (m_player->getVolume() < 50.0f ? ICON_LC_VOLUME_1
                            :                                  ICON_LC_VOLUME_2));
        if (ImGui::Button(volIcon, ImVec2(btnW, 0)))
            m_player->toggleMute();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(muted ? "Unmute" : "Mute");
        ImGui::SameLine(0, spacing);

        float volPct = (float)m_player->getVolume();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::SliderFloat("##vol", &volPct, 0.0f, 100.0f, "%.0f%%")) {
            m_player->setVolume(volPct);
        }
    }
    ImGui::SameLine(0, spacing * 3);

    // Speed selector
    {
        static const float speeds[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
        char speedLbl[32];
        snprintf(speedLbl, sizeof(speedLbl), "%.2gx", m_player->getSpeed());
        ImGui::SetNextItemWidth(60.0f);
        if (ImGui::BeginCombo("##speed", speedLbl)) {
            for (float s : speeds) {
                char itemLbl[16];
                snprintf(itemLbl, sizeof(itemLbl), "%.2gx", s);
                bool sel = std::abs(m_player->getSpeed() - s) < 1e-3;
                if (ImGui::Selectable(itemLbl, sel))
                    m_player->setSpeed(s);
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Playback speed");
    }
    ImGui::SameLine(0, spacing * 2);

    // Audio track popup
    if (ImGui::Button(ICON_LC_LANGUAGES "##audiopop", ImVec2(btnW, 0)))
        ImGui::OpenPopup("##audiopopup");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Audio tracks");
    if (ImGui::BeginPopup("##audiopopup")) {
        const auto& tracks = m_player->getAudioTracks();
        if (tracks.empty()) {
            ImGui::TextDisabled("No audio tracks");
        } else {
            int currentId = m_player->getCurrentAudioTrackId();
            for (const auto& t : tracks) {
                char label[256];
                snprintf(label, sizeof(label), "#%d %s%s%s%s%dch %dHz",
                         t.id,
                         t.title.empty() ? "" : t.title.c_str(),
                         t.title.empty() ? "" : "  ",
                         t.lang.empty()  ? "" : (std::string("[") + t.lang + "]  ").c_str(),
                         t.codec.empty() ? "" : (t.codec + "  ").c_str(),
                         t.channels, t.samplerate);
                bool sel = (t.id == currentId);
                if (ImGui::Selectable(label, sel))
                    m_player->setAudioTrack(t.id);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, spacing);

    // Subtitle popup
    if (ImGui::Button(ICON_LC_CAPTIONS "##subpop", ImVec2(btnW, 0)))
        ImGui::OpenPopup("##subpopup");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Subtitles");
    if (ImGui::BeginPopup("##subpopup")) {
        const auto& subs = m_player->getSubtitleTracks();
        bool subVis = m_player->areSubtitlesVisible();
        if (ImGui::Checkbox("Visible", &subVis))
            m_player->toggleSubtitles();

        ImGui::Separator();

        int currentSubId = m_player->getCurrentSubtitleTrackId();
        if (ImGui::Selectable("Off", currentSubId == 0))
            m_player->setSubtitleTrack(0);
        for (const auto& s : subs) {
            char label[256];
            snprintf(label, sizeof(label), "#%d %s%s%s%s",
                     s.id,
                     s.title.empty() ? "" : s.title.c_str(),
                     s.title.empty() ? "" : "  ",
                     s.lang.empty()  ? "" : (std::string("[") + s.lang + "]  ").c_str(),
                     s.codec.c_str());
            bool sel = (s.id == currentSubId);
            if (ImGui::Selectable(label, sel))
                m_player->setSubtitleTrack(s.id);
        }

        ImGui::Separator();
        ImGui::TextDisabled("Sync");
        float delay = (float)m_player->getSubDelay();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("##subdelay", &delay, -10.0f, 10.0f, "%+.2f s"))
            m_player->adjustSubDelay(delay - (float)m_player->getSubDelay());
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) m_player->resetSubDelay();

        if (ImGui::Button(ICON_LC_FILE_PLUS "  Load file...")) {
#ifdef _WIN32
            char filePath[MAX_PATH] = {0};
            OPENFILENAMEA ofn = {0};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = "Subtitle Files\0*.srt;*.ass;*.ssa;*.sub;*.vtt\0All Files\0*.*\0";
            ofn.lpstrFile = filePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            ofn.lpstrTitle = "Load Subtitle File";
            if (GetOpenFileNameA(&ofn)) {
                m_player->loadSubtitleFile(filePath);
            }
#endif
        }
        ImGui::EndPopup();
    }

    // Right-aligned: fullscreen + controls drawer
    {
        const float endW = (btnW + spacing) * 2;
        float region = ImGui::GetContentRegionAvail().x;
        if (region > endW) ImGui::SameLine(0, region - endW);

        const char* fsLabel = m_isFullscreen ? ICON_LC_MINIMIZE : ICON_LC_MAXIMIZE;
        if (ImGui::Button(fsLabel, ImVec2(btnW, 0))) {
            if (m_fullscreenCb) m_fullscreenCb();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(m_isFullscreen ? "Exit Fullscreen (F11/Esc)" : "Fullscreen (F11)");
        ImGui::SameLine(0, spacing);

        if (ImGui::Button(ICON_LC_SLIDERS, ImVec2(btnW, 0))) {
            if (m_controlsCb) m_controlsCb();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Toggle Controls drawer (F3)");
    }
}
