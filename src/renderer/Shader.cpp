#include "Shader.h"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <fstream>
#include <sstream>

Shader::Shader() = default;

Shader::~Shader() {
    if (m_program)
        glDeleteProgram(m_program);
}

bool Shader::loadFromFile(const std::string& vertPath, const std::string& fragPath) {
    auto readFile = [](const std::string& path) -> std::string {
        std::ifstream file(path);
        if (!file.is_open()) return "";
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    };

    std::string vertSrc = readFile(vertPath);
    std::string fragSrc = readFile(fragPath);

    if (vertSrc.empty() || fragSrc.empty()) {
        fprintf(stderr, "Failed to read shader files: %s, %s\n",
                vertPath.c_str(), fragPath.c_str());
        return false;
    }

    return loadFromSource(vertSrc.c_str(), fragSrc.c_str());
}

bool Shader::loadFromSource(const char* vertSrc, const char* fragSrc) {
    unsigned int vert = compileShader(GL_VERTEX_SHADER, vertSrc);
    unsigned int frag = compileShader(GL_FRAGMENT_SHADER, fragSrc);

    if (!vert || !frag) {
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return false;
    }

    m_program = glCreateProgram();
    glAttachShader(m_program, vert);
    glAttachShader(m_program, frag);
    glLinkProgram(m_program);

    int success;
    glGetProgramiv(m_program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(m_program, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader link error: %s\n", log);
        glDeleteProgram(m_program);
        m_program = 0;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);

    return m_program != 0;
}

unsigned int Shader::compileShader(unsigned int type, const char* source) {
    unsigned int shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader compile error (%s): %s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

void Shader::use() const {
    glUseProgram(m_program);
}

void Shader::setMat4(const char* name, const glm::mat4& mat) const {
    glUniformMatrix4fv(glGetUniformLocation(m_program, name),
                       1, GL_FALSE, glm::value_ptr(mat));
}

void Shader::setMat3(const char* name, const glm::mat3& mat) const {
    glUniformMatrix3fv(glGetUniformLocation(m_program, name),
                       1, GL_FALSE, glm::value_ptr(mat));
}

void Shader::setVec3(const char* name, const glm::vec3& vec) const {
    glUniform3fv(glGetUniformLocation(m_program, name), 1, glm::value_ptr(vec));
}

void Shader::setVec4(const char* name, const glm::vec4& vec) const {
    glUniform4fv(glGetUniformLocation(m_program, name), 1, glm::value_ptr(vec));
}

void Shader::setFloat(const char* name, float val) const {
    glUniform1f(glGetUniformLocation(m_program, name), val);
}

void Shader::setInt(const char* name, int val) const {
    glUniform1i(glGetUniformLocation(m_program, name), val);
}
