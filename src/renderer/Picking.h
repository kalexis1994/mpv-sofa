#pragma once

#include <glm/glm.hpp>

// Ray-sphere intersection for speaker picking
struct PickResult {
    bool hit;
    int speakerIndex;  // -1 if no hit
    float distance;
    glm::vec3 point;
};

PickResult pickSpeaker(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                       const glm::vec3* speakerPositions, int numSpeakers,
                       float sphereRadius);
