#pragma once

#include <glm/glm.hpp>

class Camera {
public:
    Camera();

    void setPosition(const glm::vec3& pos);
    void lookAt(const glm::vec3& target);

    void orbit(float deltaX, float deltaY);
    void zoom(float delta);
    void pan(float deltaX, float deltaY);

    glm::mat4 getViewMatrix() const;
    glm::mat4 getProjectionMatrix(float aspect) const;
    glm::vec3 getPosition() const;

    // Ray from camera through screen point (normalized device coords)
    void screenToRay(float ndcX, float ndcY, float aspect,
                     glm::vec3& origin, glm::vec3& direction) const;

private:
    void updateFromSpherical();

    glm::vec3 m_target = glm::vec3(0.0f);
    float m_distance = 8.0f;
    float m_yaw = 0.0f;     // horizontal angle (degrees)
    float m_pitch = 30.0f;  // vertical angle (degrees)
    float m_fov = 45.0f;
    float m_nearPlane = 0.1f;
    float m_farPlane = 100.0f;

    glm::vec3 m_position;
};
