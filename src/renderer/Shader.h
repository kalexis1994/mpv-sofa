#pragma once

#include <string>
#include <glm/glm.hpp>

class Shader {
public:
    Shader();
    ~Shader();

    bool loadFromFile(const std::string& vertPath, const std::string& fragPath);
    bool loadFromSource(const char* vertSrc, const char* fragSrc);

    void use() const;
    void setMat4(const char* name, const glm::mat4& mat) const;
    void setMat3(const char* name, const glm::mat3& mat) const;
    void setVec3(const char* name, const glm::vec3& vec) const;
    void setVec4(const char* name, const glm::vec4& vec) const;
    void setFloat(const char* name, float val) const;
    void setInt(const char* name, int val) const;

    unsigned int getProgram() const { return m_program; }

private:
    unsigned int m_program = 0;
    unsigned int compileShader(unsigned int type, const char* source);
};
