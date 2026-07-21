#pragma once

// Minimal image → OpenGL texture helper, for the handful of bitmaps the UI
// needs (currently just the logo on the home screen).  Decoding goes through
// the stb_image that ImGuiFileDialog already vendors, so this adds no new
// dependency.

// Returns 0 on failure (missing file, unsupported format).  The caller owns
// the texture and should glDeleteTextures it.
unsigned int loadTextureFromFile(const char* path, int* outWidth, int* outHeight);
