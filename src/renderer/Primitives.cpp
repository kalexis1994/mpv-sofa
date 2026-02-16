#include "Primitives.h"
#include <glad/glad.h>
#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- SphereMesh ----

SphereMesh::SphereMesh() = default;

SphereMesh::~SphereMesh() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
}

void SphereMesh::init(int rings, int sectors) {
    std::vector<float> vertices;
    std::vector<unsigned int> indices;

    for (int r = 0; r <= rings; r++) {
        for (int s = 0; s <= sectors; s++) {
            float y = sinf(-(float)M_PI / 2.0f + (float)M_PI * r / rings);
            float x = cosf(2.0f * (float)M_PI * s / sectors) *
                      sinf((float)M_PI * r / rings);
            float z = sinf(2.0f * (float)M_PI * s / sectors) *
                      sinf((float)M_PI * r / rings);

            // position
            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(z);
            // normal (same as position for unit sphere)
            vertices.push_back(x);
            vertices.push_back(y);
            vertices.push_back(z);
        }
    }

    for (int r = 0; r < rings; r++) {
        for (int s = 0; s < sectors; s++) {
            int a = r * (sectors + 1) + s;
            int b = a + sectors + 1;

            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(a + 1);

            indices.push_back(b);
            indices.push_back(b + 1);
            indices.push_back(a + 1);
        }
    }

    m_indexCount = (int)indices.size();

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                 vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int),
                 indices.data(), GL_STATIC_DRAW);

    // position attribute
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // normal attribute
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                         (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void SphereMesh::draw() const {
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

// ---- BoxMesh ----

BoxMesh::BoxMesh() = default;

BoxMesh::~BoxMesh() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
}

void BoxMesh::init() {
    // Unit box centered at origin, 1x1x1
    // Each face has its own vertices for correct normals
    // Format: x, y, z, nx, ny, nz
    float v[] = {
        // Front face (z = +0.5)
        -0.5f, -0.5f,  0.5f,  0, 0, 1,
         0.5f, -0.5f,  0.5f,  0, 0, 1,
         0.5f,  0.5f,  0.5f,  0, 0, 1,
        -0.5f,  0.5f,  0.5f,  0, 0, 1,
        // Back face (z = -0.5)
         0.5f, -0.5f, -0.5f,  0, 0,-1,
        -0.5f, -0.5f, -0.5f,  0, 0,-1,
        -0.5f,  0.5f, -0.5f,  0, 0,-1,
         0.5f,  0.5f, -0.5f,  0, 0,-1,
        // Right face (x = +0.5)
         0.5f, -0.5f,  0.5f,  1, 0, 0,
         0.5f, -0.5f, -0.5f,  1, 0, 0,
         0.5f,  0.5f, -0.5f,  1, 0, 0,
         0.5f,  0.5f,  0.5f,  1, 0, 0,
        // Left face (x = -0.5)
        -0.5f, -0.5f, -0.5f, -1, 0, 0,
        -0.5f, -0.5f,  0.5f, -1, 0, 0,
        -0.5f,  0.5f,  0.5f, -1, 0, 0,
        -0.5f,  0.5f, -0.5f, -1, 0, 0,
        // Top face (y = +0.5)
        -0.5f,  0.5f,  0.5f,  0, 1, 0,
         0.5f,  0.5f,  0.5f,  0, 1, 0,
         0.5f,  0.5f, -0.5f,  0, 1, 0,
        -0.5f,  0.5f, -0.5f,  0, 1, 0,
        // Bottom face (y = -0.5)
        -0.5f, -0.5f, -0.5f,  0,-1, 0,
         0.5f, -0.5f, -0.5f,  0,-1, 0,
         0.5f, -0.5f,  0.5f,  0,-1, 0,
        -0.5f, -0.5f,  0.5f,  0,-1, 0,
    };

    unsigned int idx[] = {
         0, 1, 2,  2, 3, 0,   // front
         4, 5, 6,  6, 7, 4,   // back
         8, 9,10, 10,11, 8,   // right
        12,13,14, 14,15,12,   // left
        16,17,18, 18,19,16,   // top
        20,21,22, 22,23,20,   // bottom
    };

    m_indexCount = 36;

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                         (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
}

void BoxMesh::draw() const {
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

// ---- GridMesh ----

GridMesh::GridMesh() = default;

GridMesh::~GridMesh() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
}

void GridMesh::init(int size, float spacing) {
    std::vector<float> vertices;

    float half = size * spacing;
    for (int i = -size; i <= size; i++) {
        float pos = i * spacing;
        // Line along Z
        vertices.push_back(pos); vertices.push_back(0); vertices.push_back(-half);
        vertices.push_back(pos); vertices.push_back(0); vertices.push_back(half);
        // Line along X
        vertices.push_back(-half); vertices.push_back(0); vertices.push_back(pos);
        vertices.push_back(half);  vertices.push_back(0); vertices.push_back(pos);
    }

    m_vertexCount = (int)(vertices.size() / 3);

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float),
                 vertices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

void GridMesh::draw() const {
    glBindVertexArray(m_vao);
    glDrawArrays(GL_LINES, 0, m_vertexCount);
    glBindVertexArray(0);
}

// ---- LineBatch ----

LineBatch::LineBatch() = default;

LineBatch::~LineBatch() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    delete[] m_vertices;
}

void LineBatch::init(int maxLines) {
    m_maxLines = maxLines;
    m_vertices = new float[maxLines * 6]; // 2 verts * 3 floats per line
    m_count = 0;

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, maxLines * 6 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

void LineBatch::clear() {
    m_count = 0;
}

void LineBatch::addLine(float x1, float y1, float z1, float x2, float y2, float z2) {
    if (m_count >= m_maxLines) return;
    int offset = m_count * 6;
    m_vertices[offset + 0] = x1; m_vertices[offset + 1] = y1; m_vertices[offset + 2] = z1;
    m_vertices[offset + 3] = x2; m_vertices[offset + 4] = y2; m_vertices[offset + 5] = z2;
    m_count++;
}

void LineBatch::flush() {
    if (m_count == 0) return;
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, m_count * 6 * sizeof(float), m_vertices);
    glDrawArrays(GL_LINES, 0, m_count * 2);
    glBindVertexArray(0);
}
