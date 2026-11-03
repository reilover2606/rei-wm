/* Compiles the stb_image implementation once, with warnings off: it is
 * third-party code and not ours to clean up. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_FAILURE_STRINGS_UNUSED
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_PNM
#include "stb_image.h"
