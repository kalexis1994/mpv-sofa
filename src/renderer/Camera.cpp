#include "Camera.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

Camera::Camera() {
    updateFromSpherical();
}

void Camera::setPosition(const glm::vec3& pos) {
    m_position = pos;
    // Derive spherical from position
    glm::vec3 dir = pos - m_target;
    m_distance = glm::length(dir);
    if (m_distance > 0.001f) {
        m_pitch = glm::degrees(asinf(dir.y / m_distance));
        m_yaw = glm::degrees(atan2f(dir.x, dir.z));
    }
}

void Camera::lookAt(const glm::vec3& target) {
    m_target = target;
    updateFromSpherical();
}

void Camera::orbit(float deltaX, float deltaY) {
    m_yaw -= deltaX;
    m_pitch += deltaY;
    m_pitch = std::clamp(m_pitch, -89.0f, 89.0f);
    updateFromSpherical();
}

void Camera::zoom(float delta) {
    m_distance += delta;
    m_distance = std::clamp(m_distance, 1.0f, 50.0f);
    updateFromSpherical();
}

void Camera::pan(float deltaX, float deltaY) {
    glm::vec3 forward = glm::normalize(m_target - m_position);
    glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
    glm::vec3 up = glm::cross(right, forward);

    m_target += right * (-deltaX) + up * deltaY;
    updateFromSpherical();
}

void Camera::updateFromSpherical() {
    float yawRad = glm::radians(m_yaw);
    float pitchRad = glm::radians(m_pitch);

    m_position.x = m_target.x + m_distance * cosf(pitchRad) * sinf(yawRad);
    m_position.y = m_target.y + m_distance * sinf(pitchRad);
    m_position.z = m_target.z + m_distance * cosf(pitchRad) * cosf(yawRad);
}

glm::mat4 Camera::getViewMatrix() const {
    return glm::lookAt(m_position, m_target, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Camera::getProjectionMatrix(float aspect) const {
    return glm::perspective(glm::radians(m_fov), aspect, m_nearPlane, m_farPlane);
}

glm::vec3 Camera::getPosition() const {
    return m_position;
}

void Camera::screenToRay(float ndcX, float ndcY, float aspect,
                          glm::vec3& origin, glm::vec3& direction) const {
    glm::mat4 invVP = glm::inverse(getProjectionMatrix(aspect) * getViewMatrix());

    glm::vec4 near = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    glm::vec4 far  = invVP * glm::vec4(ndcX, ndcY,  1.0f, 1.0f);

    near /= near.w;
    far  /= far.w;

    origin = glm::vec3(near);
    direction = glm::normalize(glm::vec3(far - near));
}
