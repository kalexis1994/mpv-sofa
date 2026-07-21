#include "ImageTexture.h"

#include <glad/glad.h>

// ImGuiFileDialog vendors stb_image but only compiles it when thumbnails are
// enabled (they aren't), so this translation unit owns the implementation.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#include "stb/stb_image.h"

#include <cstdio>

unsigned int loadTextureFromFile(const char* path, int* outWidth, int* outHeight) {
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) {
        fprintf(stderr, "[UI] could not load image %s (%s)\n",
                path, stbi_failure_reason());
        return 0;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    // The logo is drawn well below its native size, so a mip chain is what
    // keeps the downscale from sparkling.
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(pixels);
    if (outWidth)  *outWidth  = w;
    if (outHeight) *outHeight = h;
    return tex;
}
