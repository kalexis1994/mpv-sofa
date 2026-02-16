#include "Picking.h"
#include <cmath>
#include <limits>

static bool raySphereIntersect(const glm::vec3& origin, const glm::vec3& dir,
                                const glm::vec3& center, float radius, float& t) {
    glm::vec3 oc = origin - center;
    float a = glm::dot(dir, dir);
    float b = 2.0f * glm::dot(oc, dir);
    float c = glm::dot(oc, oc) - radius * radius;
    float discriminant = b * b - 4.0f * a * c;

    if (discriminant < 0)
        return false;

    t = (-b - sqrtf(discriminant)) / (2.0f * a);
    return t > 0;
}

PickResult pickSpeaker(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                       const glm::vec3* speakerPositions, int numSpeakers,
                       float sphereRadius) {
    PickResult result = {false, -1, std::numeric_limits<float>::max(), glm::vec3(0)};

    for (int i = 0; i < numSpeakers; i++) {
        float t;
        if (raySphereIntersect(rayOrigin, rayDir, speakerPositions[i], sphereRadius, t)) {
            if (t < result.distance) {
                result.hit = true;
                result.speakerIndex = i;
                result.distance = t;
                result.point = rayOrigin + rayDir * t;
            }
        }
    }

    return result;
}
