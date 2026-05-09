#include "TransportBar.h"
#include "audio/MpvPlayer.h"
#include <imgui.h>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

TransportBar::TransportBar(MpvPlayer* player) : m_player(player) {}

void TransportBar::formatTime(double seconds, char* buf, int bufSize) {
    if (seconds < 0) seconds = 0;
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

void TransportBar::renderContent() {
    if (!m_player) {
        ImGui::Text("No player available");
        return;
    }

    // File info
    std::string filename = m_player->getFilename();
    if (!filename.empty()) {
        ImGui::Text("%s", filename.c_str());
        ImGui::Separator();
    }

    // Transport controls
    float buttonWidth = 40.0f;
    float spacing = 4.0f;

    // Seek backward
    if (ImGui::Button("<<", ImVec2(buttonWidth, 0))) {
        m_player->seekRelative(-10.0);
    }
    ImGui::SameLine(0, spacing);

    // Play/Pause
    const char* playLabel = m_player->isPaused() ? " > " : " || ";
    if (ImGui::Button(playLabel, ImVec2(buttonWidth, 0))) {
        m_player->togglePause();
    }
    ImGui::SameLine(0, spacing);

    // Seek forward
    if (ImGui::Button(">>", ImVec2(buttonWidth, 0))) {
        m_player->seekRelative(10.0);
    }
    ImGui::SameLine(0, spacing);

    // Fullscreen toggle
    const char* fsLabel = m_isFullscreen ? "[x]" : "[ ]";
    if (ImGui::Button(fsLabel, ImVec2(buttonWidth, 0))) {
        if (m_fullscreenCb) m_fullscreenCb();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(m_isFullscreen ? "Exit Fullscreen (F11/Esc)" : "Fullscreen (F11)");
    ImGui::SameLine(0, spacing);

    // Controls drawer toggle (gear).  ImGui's default font has no glyph
    // for U+2699, so we use ASCII "Cfg" as a portable label.
    if (ImGui::Button("Cfg", ImVec2(buttonWidth, 0))) {
        if (m_controlsCb) m_controlsCb();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toggle Controls drawer (F3)");

    // Time display
    ImGui::SameLine(0, 20.0f);
    char posStr[32], durStr[32];
    formatTime(m_player->getPosition(), posStr, sizeof(posStr));
    formatTime(m_player->getDuration(), durStr, sizeof(durStr));
    ImGui::Text("%s / %s", posStr, durStr);

    // Seek bar
    double duration = m_player->getDuration();
    if (duration > 0) {
        float pos = (float)(m_player->getPosition() / duration);
        if (ImGui::SliderFloat("##seek", &pos, 0.0f, 1.0f, "")) {
            m_player->seek(pos * duration);
        }
    }

    // Audio track selector
    const auto& tracks = m_player->getAudioTracks();
    if (!tracks.empty()) {
        ImGui::Separator();
        ImGui::Text("Audio Track:");

        int currentId = m_player->getCurrentAudioTrackId();
        int selectedIdx = 0;

        // Build combo items
        std::vector<std::string> labels;
        for (size_t i = 0; i < tracks.size(); i++) {
            const auto& t = tracks[i];
            char label[256];
            snprintf(label, sizeof(label), "#%d: %s%s%s(%s %dch %dHz)",
                     t.id,
                     t.title.empty() ? "" : t.title.c_str(),
                     t.title.empty() ? "" : " ",
                     t.lang.empty() ? "" : (std::string("[") + t.lang + "] ").c_str(),
                     t.codec.c_str(), t.channels, t.samplerate);
            labels.push_back(label);
            if (t.id == currentId)
                selectedIdx = (int)i;
        }

        if (ImGui::BeginCombo("##audiotrack", labels[selectedIdx].c_str())) {
            for (size_t i = 0; i < tracks.size(); i++) {
                bool isSelected = ((int)i == selectedIdx);
                if (ImGui::Selectable(labels[i].c_str(), isSelected)) {
                    m_player->setAudioTrack(tracks[i].id);
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Detailed metadata for selected track
        ImGui::Separator();
        ImGui::Text("Selected Audio Track Details:");
        if (selectedIdx >= 0 && selectedIdx < (int)tracks.size()) {
            const auto& t = tracks[selectedIdx];

            ImGui::Indent();

            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Track ID:");
            ImGui::SameLine(); ImGui::Text("%d", t.id);

            if (!t.title.empty()) {
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Title:");
                ImGui::SameLine(); ImGui::TextWrapped("%s", t.title.c_str());
            }

            if (!t.lang.empty()) {
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Language:");
                ImGui::SameLine(); ImGui::Text("%s", t.lang.c_str());
            }

            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Codec:");
            ImGui::SameLine(); ImGui::Text("%s", t.codec.c_str());

            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Channels:");
            ImGui::SameLine(); ImGui::Text("%d", t.channels);

            if (!t.channelLayout.empty()) {
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Layout:");
                ImGui::SameLine(); ImGui::Text("%s", t.channelLayout.c_str());
            }

            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Sample Rate:");
            ImGui::SameLine(); ImGui::Text("%d Hz", t.samplerate);

            if (t.bitrate > 0) {
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "Bitrate:");
                ImGui::SameLine();
                if (t.bitrate >= 1000000)
                    ImGui::Text("%.1f Mbps", t.bitrate / 1000000.0);
                else
                    ImGui::Text("%d kbps", t.bitrate / 1000);
            }

            if (t.isDefault) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "[Default]");
            }

            ImGui::Unindent();
        }
    }

    // Subtitle section
    const auto& subs = m_player->getSubtitleTracks();
    ImGui::Separator();
    ImGui::Text("Subtitles:");

    if (!subs.empty()) {
        ImGui::SameLine();

        // Visibility toggle
        bool subVis = m_player->areSubtitlesVisible();
        if (ImGui::Checkbox("##subvis", &subVis)) {
            m_player->toggleSubtitles();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(subVis ? "(visible)" : "(hidden)");

        int currentSubId = m_player->getCurrentSubtitleTrackId();
        int selectedSubIdx = -1;

        // Build combo items: "Off" + all subtitle tracks
        std::vector<std::string> subLabels;
        subLabels.push_back("Off");

        for (size_t i = 0; i < subs.size(); i++) {
            const auto& s = subs[i];
            char label[256];
            snprintf(label, sizeof(label), "#%d: %s%s%s(%s)%s%s",
                     s.id,
                     s.title.empty() ? "" : s.title.c_str(),
                     s.title.empty() ? "" : " ",
                     s.lang.empty() ? "" : (std::string("[") + s.lang + "] ").c_str(),
                     s.codec.c_str(),
                     s.isDefault ? " [Default]" : "",
                     s.isForced ? " [Forced]" : "");
            subLabels.push_back(label);
            if (s.id == currentSubId)
                selectedSubIdx = (int)i + 1;
        }

        if (selectedSubIdx < 0)
            selectedSubIdx = 0;

        if (ImGui::BeginCombo("##subtrack", subLabels[selectedSubIdx].c_str())) {
            if (ImGui::Selectable(subLabels[0].c_str(), selectedSubIdx == 0)) {
                m_player->setSubtitleTrack(0);
            }
            if (selectedSubIdx == 0)
                ImGui::SetItemDefaultFocus();

            for (size_t i = 0; i < subs.size(); i++) {
                bool isSelected = ((int)i + 1 == selectedSubIdx);
                if (ImGui::Selectable(subLabels[i + 1].c_str(), isSelected)) {
                    m_player->setSubtitleTrack(subs[i].id);
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // Subtitle sync controls
    double delay = m_player->getSubDelay();
    ImGui::Text("Sync: %+.1fs", delay);
    ImGui::SameLine();
    if (ImGui::Button("-5s##sub")) m_player->adjustSubDelay(-5.0);
    ImGui::SameLine();
    if (ImGui::Button("-1s##sub")) m_player->adjustSubDelay(-1.0);
    ImGui::SameLine();
    if (ImGui::Button("-0.5##sub")) m_player->adjustSubDelay(-0.5);
    ImGui::SameLine();
    if (ImGui::Button("+0.5##sub")) m_player->adjustSubDelay(0.5);
    ImGui::SameLine();
    if (ImGui::Button("+1s##sub")) m_player->adjustSubDelay(1.0);
    ImGui::SameLine();
    if (ImGui::Button("+5s##sub")) m_player->adjustSubDelay(5.0);
    ImGui::SameLine();
    if (ImGui::Button("Reset##sub")) m_player->resetSubDelay();

    // Load external subtitle file
    if (ImGui::Button("Load SRT...")) {
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
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Load external subtitle file (.srt, .ass, .ssa, .sub, .vtt)\nYou can also drag-and-drop subtitle files onto the window");
}
