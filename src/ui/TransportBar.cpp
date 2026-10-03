#include "TransportBar.h"
#include "audio/MpvPlayer.h"
#include "ui/ImGuiLayer.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <IconsLucide.h>
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>
#include <cmath>

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

std::string Ellipsize(const std::string& text, float maxWidth) {
    if (maxWidth <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
        return text;
    static const char* suffix = "...";
    const float suffixW = ImGui::CalcTextSize(suffix).x;
    if (suffixW >= maxWidth) return suffix;

    size_t lo = 0, hi = text.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        std::string candidate = text.substr(0, mid) + suffix;
        if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth) lo = mid;
        else hi = mid - 1;
    }
    return text.substr(0, lo) + suffix;
}

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
    const float tv = ImGuiLayer::effectiveScale();
    float  h   = 14.0f * tv;
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

    float trackTop    = pos.y + (h - 8.0f * tv) * 0.5f;
    float trackBot    = trackTop + 8.0f * tv;
    float rounding    = 3.0f * tv;

    // Track background.
    dl->AddRectFilled(ImVec2(pos.x, trackTop),
                      ImVec2(pos.x + w, trackBot), colTrack, rounding);
    dl->AddRect(ImVec2(pos.x, trackTop),
                ImVec2(pos.x + w, trackBot), colBorder, rounding, 0, 1.0f * tv);

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
                        ImVec2(x, pos.y + h), colChapter, 1.0f * tv);
        }
    }

    // Position indicator (small circle on the fill edge).
    if (duration > 0) {
        float frac = (float)(position / duration);
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        float cx = pos.x + frac * w;
        float cy = (trackTop + trackBot) * 0.5f;
        dl->AddCircleFilled(ImVec2(cx, cy), 5.0f * tv, colFill);
        dl->AddCircle      (ImVec2(cx, cy), 5.0f * tv, colBorder, 0, 1.0f * tv);
    }

    // Hover indicator + tooltip with time at x.
    if (duration > 0 && (hovered || active)) {
        float mx = ImGui::GetIO().MousePos.x;
        float frac = (mx - pos.x) / w;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        float vx = pos.x + frac * w;

        dl->AddLine(ImVec2(vx, pos.y),
                    ImVec2(vx, pos.y + h), colHover, 1.5f * tv);

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
            const auto& chapters = m_player->getChapters();
            const float rowStart = ImGui::GetCursorPosX();
            const float avail = ImGui::GetContentRegionAvail().x;
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
                const float gap = style.ItemSpacing.x;
                // Keep metadata on the same row only when both fields retain
                // useful room.  Otherwise it becomes a second row rather
                // than painting over the filename.
                if (infoW + gap < avail * 0.55f) {
                    const std::string shown = Ellipsize(filename,
                                                        avail - infoW - gap);
                    ImGui::TextDisabled("%s", shown.c_str());
                    ImGui::SameLine(rowStart + avail - infoW);
                    ImGui::TextDisabled("%s", info);
                } else {
                    const std::string shown = Ellipsize(filename, avail);
                    ImGui::TextDisabled("%s", shown.c_str());
                    ImGui::TextDisabled("%s", info);
                }
            } else {
                const std::string shown = Ellipsize(filename, avail);
                ImGui::TextDisabled("%s", shown.c_str());
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
    const float tv      = ImGuiLayer::effectiveScale();
    // A fixed 32 px box was smaller than the scaled icon plus horizontal
    // padding.  Derive a TV-sized minimum that always contains its glyph.
    const float btnW    = std::max(44.0f * tv,
        ImGui::CalcTextSize(ICON_LC_SLIDERS).x + style.FramePadding.x * 2.0f);
    const float spacing = 4.0f * tv;
    const float volW    = 120.0f * tv;
    const float speedW  = 60.0f * tv;
    const float controlsAvail = ImGui::GetContentRegionAvail().x;
    const float fullRowW = btnW * 11.0f + volW + speedW + spacing * 19.0f;
    const bool wrapControls = fullRowW > controlsAvail;

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
    // The visible glyph changes with playback state, but the widget identity
    // must not. With the icon alone as its label, ImGui generated a different
    // ID after mpv acknowledged pause/play and dropped the nav focus because
    // the previously-focused item had disappeared. "###" keeps the suffix as
    // the stable ID while rendering only the current icon.
    const std::string playPauseLabel =
        std::string(playLbl) + "###transport_play_pause";
    if (ImGui::Button(playPauseLabel.c_str(), ImVec2(btnW, 0)))
        m_player->togglePause();
    if (m_focusPlayPauseRequested) {
        // FocusItem is deliberately stronger than SetItemDefaultFocus: this
        // request happens in an already-visible window after a shortcut or
        // gamepad action, so there is no first-appearance default to apply.
        ImGui::FocusItem();
        ImGui::SetNavCursorVisible(true);
        m_focusPlayPauseRequested = false;
    }
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
    if (!wrapControls) ImGui::SameLine(0, spacing * 4);

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
        ImGui::SetNextItemWidth(volW);
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
        ImGui::SetNextItemWidth(speedW);
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

    // Audio track button — opens the full track picker modal.
    if (ImGui::Button(ICON_LC_LANGUAGES "##audiopop", ImVec2(btnW, 0))) {
        if (m_audioPickerCb) m_audioPickerCb();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Audio tracks");
    ImGui::SameLine(0, spacing);

    // Subtitle button — opens the subtitle picker modal (also exposes
    // visibility, sync delay and external-file load).
    if (ImGui::Button(ICON_LC_CAPTIONS "##subpop", ImVec2(btnW, 0))) {
        if (m_subPickerCb) m_subPickerCb();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Subtitles");

    // Right-aligned: menu + fullscreen + controls drawer
    {
        const float endW = (btnW + spacing) * 3;
        float region = ImGui::GetContentRegionAvail().x;
        if (region > endW) ImGui::SameLine(0, region - endW);

        // Playback menu: the way back out (open another file, settings…).
        if (ImGui::Button(ICON_LC_MENU, ImVec2(btnW, 0))) {
            if (m_menuCb) m_menuCb();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Menu (Esc)");
        ImGui::SameLine(0, spacing);

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
            ImGui::SetTooltip("Spatial settings");
    }
}
