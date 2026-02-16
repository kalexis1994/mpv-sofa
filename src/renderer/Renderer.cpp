#include "Renderer.h"
#include "core/SharedState.h"
#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Inline shaders (no external files needed)

static const char* solidVertSrc = R"(
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat3 uNormalMat;

out vec3 vNormal;
out vec3 vFragPos;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    vFragPos = worldPos.xyz;
    vNormal = uNormalMat * aNormal;
    gl_Position = uProjection * uView * worldPos;
}
)";

static const char* solidFragSrc = R"(
#version 410 core
in vec3 vNormal;
in vec3 vFragPos;

uniform vec4 uColor;
uniform vec3 uLightDir;
uniform vec3 uViewPos;

out vec4 FragColor;

void main() {
    vec3 norm = normalize(vNormal);
    vec3 lightDir = normalize(uLightDir);

    // Ambient
    float ambient = 0.3;

    // Diffuse
    float diff = max(dot(norm, lightDir), 0.0);

    // Specular
    vec3 viewDir = normalize(uViewPos - vFragPos);
    vec3 halfDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(norm, halfDir), 0.0), 32.0);

    vec3 result = uColor.rgb * (ambient + diff * 0.6 + spec * 0.2);
    FragColor = vec4(result, uColor.a);
}
)";

static const char* gridVertSrc = R"(
#version 410 core
layout(location = 0) in vec3 aPos;

uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vWorldPos;

void main() {
    vWorldPos = aPos;
    gl_Position = uProjection * uView * vec4(aPos, 1.0);
}
)";

static const char* gridFragSrc = R"(
#version 410 core
in vec3 vWorldPos;

out vec4 FragColor;

void main() {
    float dist = length(vWorldPos.xz);
    float fade = 1.0 - smoothstep(5.0, 10.0, dist);

    // Darker color for grid lines
    vec3 color = vec3(0.3, 0.3, 0.35);

    // Highlight axes
    if (abs(vWorldPos.x) < 0.05)
        color = vec3(0.2, 0.2, 0.8); // Z axis (blue)
    if (abs(vWorldPos.z) < 0.05)
        color = vec3(0.8, 0.2, 0.2); // X axis (red)

    FragColor = vec4(color, fade * 0.5);
}
)";

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::init() {
    m_solidShader = std::make_unique<Shader>();
    if (!m_solidShader->loadFromSource(solidVertSrc, solidFragSrc))
        return false;

    m_gridShader = std::make_unique<Shader>();
    if (!m_gridShader->loadFromSource(gridVertSrc, gridFragSrc))
        return false;

    m_sphere.init(16, 32);
    m_box.init();
    m_grid.init(10, 1.0f);
    m_lines.init(HRTF_MAX_CHANNELS);

    return true;
}

glm::vec3 Renderer::speakerToWorldPos(float azimuth, float elevation, float distance) {
    float az = azimuth * (float)(M_PI / 180.0);
    float el = elevation * (float)(M_PI / 180.0);

    // Convention: +X=right, +Y=up, -Z=front
    // Azimuth: 0=front, positive=left (so negate for +X=right)
    float x = -distance * cosf(el) * sinf(az);
    float y = distance * sinf(el);
    float z = -distance * cosf(el) * cosf(az);

    return glm::vec3(x, y, z);
}

glm::vec4 Renderer::getSpeakerColor(int channel) {
    switch (channel) {
        case 0: case 1: case 2:  return glm::vec4(0.3f, 0.5f, 1.0f, 1.0f); // Front: blue
        case 3:                   return glm::vec4(1.0f, 0.2f, 0.2f, 1.0f); // LFE: red
        case 4: case 5:          return glm::vec4(1.0f, 0.6f, 0.2f, 1.0f); // Back: orange
        case 6: case 7:          return glm::vec4(0.3f, 0.8f, 0.3f, 1.0f); // Side: green
        case 8: case 9: case 10: case 11:
                                  return glm::vec4(0.7f, 0.3f, 0.9f, 1.0f); // Height: purple
        default:                  return glm::vec4(0.7f, 0.7f, 0.7f, 1.0f); // Unknown: gray
    }
}

const char* Renderer::getSpeakerName(int channel) {
    static const char* names[] = {
        "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR",
        "TFL", "TFR", "TBL", "TBR", "?", "?", "?", "?"
    };
    if (channel >= 0 && channel < 16) return names[channel];
    return "?";
}

void Renderer::render(const Camera& camera, HrtfSharedState* state, float aspect,
                      int selectedSpeaker) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 1. Grid (uses grid shader)
    renderGrid(camera, aspect);

    // 2. Lines (uses grid shader — already bound from renderGrid)
    renderLines(camera, state, aspect);

    // 3. Solid objects (speakers + listener) — set solid shader once
    m_solidShader->use();
    glm::mat4 view = camera.getViewMatrix();
    glm::mat4 proj = camera.getProjectionMatrix(aspect);
    m_solidShader->setMat4("uView", view);
    m_solidShader->setMat4("uProjection", proj);
    m_solidShader->setVec3("uLightDir", glm::vec3(0.5f, 1.0f, 0.3f));
    m_solidShader->setVec3("uViewPos", camera.getPosition());

    renderSpeakers(camera, state, aspect, selectedSpeaker);
    renderSpatialObjects(camera, state, aspect);
    renderListener(camera, aspect);
}

void Renderer::renderGrid(const Camera& camera, float aspect) {
    m_gridShader->use();
    m_gridShader->setMat4("uView", camera.getViewMatrix());
    m_gridShader->setMat4("uProjection", camera.getProjectionMatrix(aspect));
    m_grid.draw();
}

void Renderer::renderSpeakers(const Camera& camera, HrtfSharedState* state, float aspect,
                               int selectedSpeaker) {
    if (!state) return;

    int numCh = atomic_load(&state->num_channels);

    // Solid shader is already bound and configured by render()
    for (int ch = 0; ch < numCh && ch < HRTF_MAX_CHANNELS; ch++) {
        HrtfPosition pos = state->speaker_pos[ch];
        glm::vec3 worldPos = speakerToWorldPos(pos.azimuth, pos.elevation, pos.distance);
        glm::vec4 color = getSpeakerColor(ch);

        // Highlight selected speaker
        if (ch == selectedSpeaker) {
            color = glm::mix(color, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), 0.5f);
        }

        // Scale based on channel RMS level for visual feedback
        float rms = atomic_load(&state->channel_rms[ch]);
        float pulse = 0.15f + rms * 0.3f;

        // Speaker box proportions: wider than tall, shallow depth
        glm::vec3 boxScale(0.35f, 0.45f, 0.2f);
        boxScale *= pulse / 0.15f; // scale with audio level

        // Selected speaker is slightly larger
        if (ch == selectedSpeaker) {
            boxScale *= 1.15f;
        }

        // Rotate box to face the listener (origin)
        glm::vec3 dir = glm::normalize(-worldPos); // direction toward origin
        glm::vec3 up(0.0f, 1.0f, 0.0f);

        // Build rotation matrix: box front (local -Z) faces toward listener
        glm::vec3 right = glm::normalize(glm::cross(up, dir));
        glm::vec3 correctedUp = glm::cross(dir, right);

        glm::mat4 rotation(1.0f);
        rotation[0] = glm::vec4(right, 0.0f);
        rotation[1] = glm::vec4(correctedUp, 0.0f);
        rotation[2] = glm::vec4(-dir, 0.0f); // -dir so front face points at listener

        glm::mat4 model = glm::translate(glm::mat4(1.0f), worldPos);
        model = model * rotation;
        model = glm::scale(model, boxScale);

        // Compute normal matrix on CPU (avoids inverse in shader)
        glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(model)));

        m_solidShader->setMat4("uModel", model);
        m_solidShader->setMat3("uNormalMat", normalMat);
        m_solidShader->setVec4("uColor", color);
        m_box.draw();
    }
}

void Renderer::renderListener(const Camera& camera, float aspect) {
    // Solid shader is already bound and configured by render()

    // Listener head (white sphere at origin)
    glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(0.2f));
    glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(model)));
    m_solidShader->setMat4("uModel", model);
    m_solidShader->setMat3("uNormalMat", normalMat);
    m_solidShader->setVec4("uColor", glm::vec4(0.9f, 0.9f, 0.9f, 1.0f));
    m_sphere.draw();

    // Nose cone (small sphere offset forward)
    model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -0.25f));
    model = glm::scale(model, glm::vec3(0.08f));
    normalMat = glm::transpose(glm::inverse(glm::mat3(model)));
    m_solidShader->setMat4("uModel", model);
    m_solidShader->setMat3("uNormalMat", normalMat);
    m_solidShader->setVec4("uColor", glm::vec4(0.9f, 0.7f, 0.6f, 1.0f));
    m_sphere.draw();
}

void Renderer::renderLines(const Camera& camera, HrtfSharedState* state, float aspect) {
    if (!state) return;

    int numCh = atomic_load(&state->num_channels);

    // Grid shader is already bound from renderGrid
    m_lines.clear();
    for (int ch = 0; ch < numCh && ch < HRTF_MAX_CHANNELS; ch++) {
        HrtfPosition pos = state->speaker_pos[ch];
        glm::vec3 worldPos = speakerToWorldPos(pos.azimuth, pos.elevation, pos.distance);
        m_lines.addLine(0, 0, 0, worldPos.x, worldPos.y, worldPos.z);
    }
    m_lines.flush();  // single GPU upload + single draw call
}

void Renderer::renderSpatialObjects(const Camera& camera, HrtfSharedState* state, float aspect) {
    if (!state) return;

    int numObj = atomic_load(&state->num_objects);
    if (numObj <= 0) return;
    if (numObj > HRTF_MAX_OBJECTS) numObj = HRTF_MAX_OBJECTS;

    // Read room dimensions for coordinate conversion
    float room_w = atomic_load(&state->room_width);
    float room_d = atomic_load(&state->room_depth);
    float room_h = atomic_load(&state->room_height);
    if (room_w <= 0) room_w = 6.5f;
    if (room_d <= 0) room_d = 5.0f;
    if (room_h <= 0) room_h = 2.7f;

    // Solid shader is already bound by render()
    for (int i = 0; i < numObj; i++) {
        float ox = atomic_load(&state->object_x[i]);  // 0=L, 1=R
        float oy = atomic_load(&state->object_y[i]);  // 0=front, 1=back
        float oz = atomic_load(&state->object_z[i]);   // -1=floor, 1=ceiling

        // Convert to room-relative cartesian (same as af_hrtf.c)
        float rx = (ox - 0.5f) * room_w;    // centered X
        float ry = oz * room_h * 0.5f;      // height -> Y (up)
        float rz = -(1.0f - oy) * room_d + room_d * 0.5f;  // depth -> -Z (front)

        glm::vec3 worldPos(rx, ry, rz);

        // Object color: cyan for spatial dynamic objects
        glm::vec4 color(0.0f, 0.9f, 0.9f, 1.0f);

        // Small sphere for each object
        float radius = 0.12f;
        glm::mat4 model = glm::translate(glm::mat4(1.0f), worldPos);
        model = glm::scale(model, glm::vec3(radius));
        glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(model)));

        m_solidShader->setMat4("uModel", model);
        m_solidShader->setMat3("uNormalMat", normalMat);
        m_solidShader->setVec4("uColor", color);
        m_sphere.draw();
    }
}
