#pragma once

#include "Shader.h"
#include "Primitives.h"
#include "Camera.h"
#include <glm/glm.hpp>
#include <array>
#include <memory>

struct HrtfSharedState;

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init();
    void render(const Camera& camera, HrtfSharedState* state, float aspect,
                int selectedSpeaker = -1);

    // Convert speaker azimuth/elevation/distance to 3D position
    static glm::vec3 speakerToWorldPos(float azimuth, float elevation, float distance);

private:
    void renderGrid(const Camera& camera, float aspect);
    void renderSpeakers(const Camera& camera, HrtfSharedState* state, float aspect,
                        int selectedSpeaker);
    void renderListener(const Camera& camera, float aspect);
    void renderLines(const Camera& camera, HrtfSharedState* state, float aspect);
    void renderSpatialObjects(const Camera& camera, HrtfSharedState* state, float aspect);
    float getSpatialObjectLevel(HrtfSharedState* state, int objectIndex, int numChannels);
    glm::vec4 getSpatialObjectColor(float level) const;

    // Get color for speaker based on channel index.  numChannels is passed
    // so 6.1 (7-ch) renders SL/SR/BC at the right indices instead of the
    // 7.1.4 mapping.
    glm::vec4 getSpeakerColor(int channel, int numChannels);

    // Speaker name for channel index (layout-aware, see above).
    const char* getSpeakerName(int channel, int numChannels);

    std::unique_ptr<Shader> m_solidShader;
    std::unique_ptr<Shader> m_gridShader;

    SphereMesh m_sphere;
    BoxMesh m_box;
    GridMesh m_grid;
    LineBatch m_lines;

    static constexpr int kMaxObjectViz = 128;
    std::array<float, kMaxObjectViz> m_objectLevelSmooth{};
    float m_objectPulsePhase = 0.0f;
};
