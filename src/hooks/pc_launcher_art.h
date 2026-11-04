#pragma once
#include "imgui.h"

// Optional launcher art the player supplies from their own copy of the game. Nothing is shipped:
// images are read from <data folder>/launcher/<name>.png|.jpg|.jpeg, e.g. cover_city, cover_logo.
struct LauncherImage
{
    ImTextureRef texture;
    float width = 0, height = 0;
};

// The decoded image, or nullptr when the file is absent or cannot be decoded. Loaded once per
// ImGui context and uploaded by the active renderer backend as a user texture.
const LauncherImage *launcher_image(const char *name);

// An image made in memory (8-bit RGBA), kept under `name` the same way: the first call's pixels
// are the image's for the rest of the context.
const LauncherImage *launcher_image_rgba(const char *name, const unsigned char *rgba, int width, int height);
