#include "MpvPlayer.h"

#ifdef HAVE_MPV
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#endif

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "core/SharedState.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cinttypes>
#include <cmath>

// Generate a small WAV file with silence (used to pump audio frames for test tones)
static bool createSilenceWav(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    const int sampleRate = 48000;
    const int channels = 2;
    const int bitsPerSample = 16;
    const int durationSec = 2;
    const int numSamples = sampleRate * durationSec;
    const int dataSize = numSamples * channels * (bitsPerSample / 8);

    // RIFF header
    fwrite("RIFF", 1, 4, f);
    int32_t riffSize = 36 + dataSize;
    fwrite(&riffSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    // fmt chunk
    fwrite("fmt ", 1, 4, f);
    int32_t fmtSize = 16;
    fwrite(&fmtSize, 4, 1, f);
    int16_t audioFormat = 1; // PCM
    fwrite(&audioFormat, 2, 1, f);
    int16_t nCh = channels;
    fwrite(&nCh, 2, 1, f);
    int32_t sr = sampleRate;
    fwrite(&sr, 4, 1, f);
    int32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    fwrite(&byteRate, 4, 1, f);
    int16_t blockAlign = channels * (bitsPerSample / 8);
    fwrite(&blockAlign, 2, 1, f);
    int16_t bps = bitsPerSample;
    fwrite(&bps, 2, 1, f);

    // data chunk (all zeros = silence)
    fwrite("data", 1, 4, f);
    int32_t ds = dataSize;
    fwrite(&ds, 4, 1, f);

    char zero[4096] = {0};
    int remaining = dataSize;
    while (remaining > 0) {
        int chunk = remaining > (int)sizeof(zero) ? (int)sizeof(zero) : remaining;
        fwrite(zero, 1, chunk, f);
        remaining -= chunk;
    }

    fclose(f);
    return true;
}

MpvPlayer::MpvPlayer() = default;

MpvPlayer::~MpvPlayer() {
    shutdown();
}

bool MpvPlayer::init(HrtfSharedState* sharedState) {
    m_sharedState = sharedState;
    fprintf(stderr, "[MpvPlayer] init() called, sharedState=%p\n", (void*)sharedState);

#ifdef HAVE_MPV
    m_mpv = mpv_create();
    if (!m_mpv) {
        fprintf(stderr, "[MpvPlayer] mpv_create() failed\n");
        return false;
    }
    fprintf(stderr, "[MpvPlayer] mpv_create() OK\n");

    // Enable mpv's own logging to stderr
    mpv_request_log_messages(m_mpv, "v");

    // Configure mpv for embedded use
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "hwdec", "auto-copy");
    mpv_set_option_string(m_mpv, "keep-open", "yes");

    // Enable lossless HD spatial object extraction so the decoder outputs
    // separate height/object channels (up to 16ch) instead of mixing
    // everything into the 7.1 bed.
    mpv_set_option_string(m_mpv, "ad-lavc-o", "extract_objects=1");

    // Log mpv messages to file for decoder debug analysis
    mpv_set_option_string(m_mpv, "log-file", "mpv_debug.log");

    // Enable our HRTF audio filter
    // Pass the shared state pointer so the filter can communicate with the UI
    char af_opt[256];
    snprintf(af_opt, sizeof(af_opt),
             "hrtf=sofa=assets/hrtf/default.sofa:shared-state=%" PRId64,
             (int64_t)(intptr_t)m_sharedState);
    fprintf(stderr, "[MpvPlayer] af option: %s\n", af_opt);
    int af_err = mpv_set_option_string(m_mpv, "af", af_opt);
    fprintf(stderr, "[MpvPlayer] af set result: %d (%s)\n", af_err,
            af_err < 0 ? mpv_error_string(af_err) : "ok");

    if (mpv_initialize(m_mpv) < 0) {
        fprintf(stderr, "[MpvPlayer] mpv_initialize() failed\n");
        mpv_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }
    fprintf(stderr, "[MpvPlayer] mpv_initialize() OK\n");

    // Setup render context for OpenGL
    mpv_opengl_init_params gl_init = {
        .get_proc_address = [](void*, const char* name) -> void* {
            return (void*)glfwGetProcAddress(name);
        },
    };

    int advanced = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    if (mpv_render_context_create(&m_renderCtx, m_mpv, params) < 0) {
        fprintf(stderr, "mpv_render_context_create() failed\n");
        mpv_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }

    // Set update callback so mpv notifies us when new frames are ready
    mpv_render_context_set_update_callback(m_renderCtx,
        [](void* ctx) {
            MpvPlayer* self = (MpvPlayer*)ctx;
            self->m_renderRequested = true;
        }, this);

    // Observe properties for UI updates
    mpv_observe_property(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "filename", MPV_FORMAT_STRING);
    mpv_observe_property(m_mpv, 0, "aid", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "sid", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "sub-visibility", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "sub-delay", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "track-list/count", MPV_FORMAT_INT64);

    // Generate silence WAV for test tone (af_hrtf needs audio frames to process)
    m_silencePath = "silence_testtone.wav";
    if (createSilenceWav(m_silencePath.c_str())) {
        fprintf(stderr, "[MpvPlayer] Created silence WAV: %s\n", m_silencePath.c_str());
    } else {
        fprintf(stderr, "[MpvPlayer] Warning: could not create silence WAV\n");
    }

    return true;
#else
    fprintf(stderr, "Built without libmpv support\n");
    return false;
#endif
}

void MpvPlayer::shutdown() {
#ifdef HAVE_MPV
    if (m_renderCtx) {
        mpv_render_context_free(m_renderCtx);
        m_renderCtx = nullptr;
    }
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
#endif
}

void MpvPlayer::loadFile(const std::string& path) {
#ifdef HAVE_MPV
    if (!m_mpv) {
        fprintf(stderr, "[MpvPlayer] loadFile: m_mpv is null!\n");
        return;
    }

    fprintf(stderr, "[MpvPlayer] loadFile: %s\n", path.c_str());
    const char* cmd[] = {"loadfile", path.c_str(), nullptr};
    int err = mpv_command_async(m_mpv, 0, cmd);
    fprintf(stderr, "[MpvPlayer] loadFile async result: %d (%s)\n", err, mpv_error_string(err));
#endif
}

void MpvPlayer::play() {
#ifdef HAVE_MPV
    if (!m_mpv) return;
    int flag = 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
#endif
}

void MpvPlayer::pause() {
#ifdef HAVE_MPV
    if (!m_mpv) return;
    int flag = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
#endif
}

void MpvPlayer::togglePause() {
#ifdef HAVE_MPV
    if (!m_mpv) return;
    const char* cmd[] = {"cycle", "pause", nullptr};
    mpv_command_async(m_mpv, 0, cmd);
#endif
}

void MpvPlayer::seek(double seconds) {
#ifdef HAVE_MPV
    if (!m_mpv) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f", seconds);
    const char* cmd[] = {"seek", buf, "absolute", nullptr};
    mpv_command_async(m_mpv, 0, cmd);
#endif
}

void MpvPlayer::seekRelative(double seconds) {
#ifdef HAVE_MPV
    if (!m_mpv) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f", seconds);
    const char* cmd[] = {"seek", buf, "relative", nullptr};
    mpv_command_async(m_mpv, 0, cmd);
#endif
}

void MpvPlayer::setAudioTrack(int id) {
#ifdef HAVE_MPV
    if (!m_mpv) return;
    int64_t aid = id;
    mpv_set_property(m_mpv, "aid", MPV_FORMAT_INT64, &aid);
#endif
}

void MpvPlayer::setSubtitleTrack(int id) {
#ifdef HAVE_MPV
    if (!m_mpv) return;
    if (id == 0) {
        // Disable subtitles
        mpv_set_option_string(m_mpv, "sid", "no");
    } else {
        int64_t sid = id;
        mpv_set_property(m_mpv, "sid", MPV_FORMAT_INT64, &sid);
    }
#endif
}

void MpvPlayer::toggleSubtitles() {
#ifdef HAVE_MPV
    if (!m_mpv) return;
    const char* cmd[] = {"cycle", "sub-visibility", nullptr};
    mpv_command_async(m_mpv, 0, cmd);
#endif
}

void MpvPlayer::adjustSubDelay(double deltaSec) {
#ifdef HAVE_MPV
    if (!m_mpv) return;
    m_subDelay += deltaSec;
    mpv_set_property(m_mpv, "sub-delay", MPV_FORMAT_DOUBLE, &m_subDelay);
#endif
}

void MpvPlayer::resetSubDelay() {
#ifdef HAVE_MPV
    if (!m_mpv) return;
    m_subDelay = 0.0;
    mpv_set_property(m_mpv, "sub-delay", MPV_FORMAT_DOUBLE, &m_subDelay);
#endif
}

void MpvPlayer::loadSubtitleFile(const std::string& path) {
#ifdef HAVE_MPV
    if (!m_mpv) return;
    const char* cmd[] = {"sub-add", path.c_str(), "auto", nullptr};
    int err = mpv_command_async(m_mpv, 0, cmd);
    fprintf(stderr, "[MpvPlayer] sub-add '%s' result: %d (%s)\n",
            path.c_str(), err, mpv_error_string(err));
#endif
}

void MpvPlayer::playTestTone(int channel) {
    if (!m_sharedState) return;

    // Signal the audio filter to play a test tone on this channel
    atomic_store(&m_sharedState->test_tone_channel, (int32_t)channel);
    atomic_store(&m_sharedState->test_tone_active, (int32_t)1);

#ifdef HAVE_MPV
    if (!m_mpv) return;

    if (!m_hasVideo) {
        // Nothing loaded - load a silence WAV to pump audio frames through the filter
        const char* cmd[] = {"loadfile", m_silencePath.c_str(), "replace", nullptr};
        mpv_command_async(m_mpv, 0, cmd);
        m_testToneSilenceSource = true;
        m_testToneWasPaused = false;
        fprintf(stderr, "[MpvPlayer] Test tone: loaded silence WAV for channel %d\n", channel);
    } else if (m_paused) {
        // Media is loaded but paused - temporarily unpause to pump audio
        m_testToneWasPaused = true;
        int flag = 0;
        mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
        fprintf(stderr, "[MpvPlayer] Test tone: unpaused for channel %d\n", channel);
    } else {
        // Already playing - filter is active, tone will mix in
        m_testToneWasPaused = false;
        fprintf(stderr, "[MpvPlayer] Test tone: already playing, channel %d\n", channel);
    }
#endif
}

void MpvPlayer::refreshTrackList() {
#ifdef HAVE_MPV
    if (!m_mpv) return;

    m_audioTracks.clear();
    m_currentAudioTrack = 0;
    m_subtitleTracks.clear();

    mpv_node trackList;
    if (mpv_get_property(m_mpv, "track-list", MPV_FORMAT_NODE, &trackList) < 0)
        return;

    if (trackList.format != MPV_FORMAT_NODE_ARRAY)  {
        mpv_free_node_contents(&trackList);
        return;
    }

    for (int i = 0; i < trackList.u.list->num; i++) {
        mpv_node& entry = trackList.u.list->values[i];
        if (entry.format != MPV_FORMAT_NODE_MAP)
            continue;

        // Check if this is an audio track
        const char* type = nullptr;
        int id = 0;
        const char* lang = nullptr;
        const char* title = nullptr;
        const char* codec = nullptr;
        const char* channelLayout = nullptr;
        int channels = 0;
        int samplerate = 0;
        int bitrate = 0;
        bool isDefault = false;
        bool isForced = false;
        bool selected = false;

        mpv_node_list* map = entry.u.list;
        for (int j = 0; j < map->num; j++) {
            const char* key = map->keys[j];
            mpv_node& val = map->values[j];

            if (strcmp(key, "type") == 0 && val.format == MPV_FORMAT_STRING)
                type = val.u.string;
            else if (strcmp(key, "id") == 0 && val.format == MPV_FORMAT_INT64)
                id = (int)val.u.int64;
            else if (strcmp(key, "lang") == 0 && val.format == MPV_FORMAT_STRING)
                lang = val.u.string;
            else if (strcmp(key, "title") == 0 && val.format == MPV_FORMAT_STRING)
                title = val.u.string;
            else if (strcmp(key, "codec") == 0 && val.format == MPV_FORMAT_STRING)
                codec = val.u.string;
            else if (strcmp(key, "demux-channel-count") == 0 && val.format == MPV_FORMAT_INT64)
                channels = (int)val.u.int64;
            else if (strcmp(key, "demux-samplerate") == 0 && val.format == MPV_FORMAT_INT64)
                samplerate = (int)val.u.int64;
            else if (strcmp(key, "demux-bitrate") == 0 && val.format == MPV_FORMAT_INT64)
                bitrate = (int)val.u.int64;
            else if (strcmp(key, "demux-channels") == 0 && val.format == MPV_FORMAT_STRING)
                channelLayout = val.u.string;
            else if (strcmp(key, "selected") == 0 && val.format == MPV_FORMAT_FLAG)
                selected = val.u.flag;
            else if (strcmp(key, "default") == 0 && val.format == MPV_FORMAT_FLAG)
                isDefault = val.u.flag;
            else if (strcmp(key, "forced") == 0 && val.format == MPV_FORMAT_FLAG)
                isForced = val.u.flag;
        }

        if (type && strcmp(type, "audio") == 0) {
            AudioTrack track;
            track.id = id;
            track.lang = lang ? lang : "";
            track.title = title ? title : "";
            track.codec = codec ? codec : "";
            track.channelLayout = channelLayout ? channelLayout : "";
            track.channels = channels;
            track.samplerate = samplerate;
            track.bitrate = bitrate;
            track.isDefault = isDefault;
            track.isForced = isForced;
            track.selected = selected;
            if (selected)
                m_currentAudioTrack = id;
            m_audioTracks.push_back(track);
        }
        else if (type && strcmp(type, "sub") == 0) {
            SubtitleTrack track;
            track.id = id;
            track.lang = lang ? lang : "";
            track.title = title ? title : "";
            track.codec = codec ? codec : "";
            track.isDefault = isDefault;
            track.isForced = isForced;
            track.selected = selected;
            if (selected)
                m_currentSubtitleTrack = id;
            m_subtitleTracks.push_back(track);
        }
    }

    mpv_free_node_contents(&trackList);
    fprintf(stderr, "[MpvPlayer] Found %zu audio tracks (current=%d), %zu subtitle tracks (current=%d)\n",
            m_audioTracks.size(), m_currentAudioTrack,
            m_subtitleTracks.size(), m_currentSubtitleTrack);
#endif
}

void MpvPlayer::update() {
#ifdef HAVE_MPV
    if (!m_mpv) return;

    // Process mpv events
    while (true) {
        mpv_event* event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;

        if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message* msg = (mpv_event_log_message*)event->data;
            fprintf(stderr, "[mpv/%s] %s", msg->prefix, msg->text);
        }
        else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property* prop = (mpv_event_property*)event->data;

            if (strcmp(prop->name, "time-pos") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                m_position = *(double*)prop->data;
            }
            else if (strcmp(prop->name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                m_duration = *(double*)prop->data;
            }
            else if (strcmp(prop->name, "pause") == 0 && prop->format == MPV_FORMAT_FLAG) {
                m_paused = *(int*)prop->data;
            }
            else if (strcmp(prop->name, "filename") == 0 && prop->format == MPV_FORMAT_STRING) {
                m_filename = *(char**)prop->data;
            }
            else if (strcmp(prop->name, "aid") == 0 && prop->format == MPV_FORMAT_INT64) {
                m_currentAudioTrack = (int)*(int64_t*)prop->data;
                refreshTrackList();
            }
            else if (strcmp(prop->name, "sid") == 0 && prop->format == MPV_FORMAT_INT64) {
                m_currentSubtitleTrack = (int)*(int64_t*)prop->data;
                refreshTrackList();
            }
            else if (strcmp(prop->name, "sub-visibility") == 0 && prop->format == MPV_FORMAT_FLAG) {
                m_subtitlesVisible = *(int*)prop->data;
            }
            else if (strcmp(prop->name, "sub-delay") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                m_subDelay = *(double*)prop->data;
            }
            else if (strcmp(prop->name, "track-list/count") == 0) {
                refreshTrackList();
            }
        }
        else if (event->event_id == MPV_EVENT_FILE_LOADED) {
            fprintf(stderr, "[MpvPlayer] FILE_LOADED event received\n");
            m_hasVideo = true;
            refreshTrackList();
        }
        else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
            fprintf(stderr, "[MpvPlayer] PLAYBACK_RESTART event, hasVideo=%d\n", m_hasVideo);
            if (!m_hasVideo) {
                fprintf(stderr, "[MpvPlayer] Setting hasVideo=true from PLAYBACK_RESTART\n");
                m_hasVideo = true;
            }
        }
        else if (event->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file* ef = (mpv_event_end_file*)event->data;
            fprintf(stderr, "[MpvPlayer] END_FILE reason=%d error=%d\n",
                    ef->reason, ef->error);
            m_hasVideo = false;
            m_audioTracks.clear();
        }
        else if (event->event_id != MPV_EVENT_NONE) {
            fprintf(stderr, "[MpvPlayer] event: %s\n", mpv_event_name(event->event_id));
        }
    }

    // Check if test tone finished and restore state
    if (m_sharedState && (m_testToneWasPaused || m_testToneSilenceSource)) {
        if (!atomic_load(&m_sharedState->test_tone_active)) {
            if (m_testToneWasPaused) {
                // Re-pause since we temporarily unpaused
                int flag = 1;
                mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
                m_testToneWasPaused = false;
                fprintf(stderr, "[MpvPlayer] Test tone done, re-pausing\n");
            }
            if (m_testToneSilenceSource) {
                // Stop the silence source
                const char* cmd[] = {"stop", nullptr};
                mpv_command_async(m_mpv, 0, cmd);
                m_testToneSilenceSource = false;
                fprintf(stderr, "[MpvPlayer] Test tone done, stopping silence source\n");
            }
        }
    }
#endif
}

void MpvPlayer::renderToFBO(unsigned int fbo, int width, int height) {
#ifdef HAVE_MPV
    if (!m_renderCtx) return;

    // Check if mpv has a new frame (also resets mpv's internal update flag)
    uint64_t flags = mpv_render_context_update(m_renderCtx);
    if (!(flags & MPV_RENDER_UPDATE_FRAME))
        return;  // no new frame — keep showing previous texture

    // Set GL state for mpv (no glGet* queries — avoids pipeline stalls)
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    mpv_opengl_fbo mpvFbo = {
        .fbo = (int)fbo,
        .w = width,
        .h = height,
        .internal_format = 0x881A  // GL_RGBA16F
    };

    int flip_y = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &mpvFbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    mpv_render_context_render(m_renderCtx, params);
    m_renderRequested = false;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
}
