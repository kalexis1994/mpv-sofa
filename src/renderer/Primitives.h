#pragma once

// Simple mesh primitives: sphere, grid, line, box

class SphereMesh {
public:
    SphereMesh();
    ~SphereMesh();

    void init(int rings = 12, int sectors = 24);
    void draw() const;

private:
    unsigned int m_vao = 0, m_vbo = 0, m_ebo = 0;
    int m_indexCount = 0;
};

class GridMesh {
public:
    GridMesh();
    ~GridMesh();

    void init(int size = 10, float spacing = 1.0f);
    void draw() const;

private:
    unsigned int m_vao = 0, m_vbo = 0;
    int m_vertexCount = 0;
};

class BoxMesh {
public:
    BoxMesh();
    ~BoxMesh();

    void init();
    void draw() const;

private:
    unsigned int m_vao = 0, m_vbo = 0, m_ebo = 0;
    int m_indexCount = 0;
};

class LineBatch {
public:
    LineBatch();
    ~LineBatch();

    void init(int maxLines = 64);
    void clear();
    void addLine(float x1, float y1, float z1, float x2, float y2, float z2);
    void flush();  // uploads and draws all lines in one call

private:
    unsigned int m_vao = 0, m_vbo = 0;
    float* m_vertices = nullptr;
    int m_count = 0;      // number of line segments added
    int m_maxLines = 0;
};
