#ifndef IMAGE_DECODER_H
#define IMAGE_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Image format types
typedef enum {
    IMG_FMT_UNKNOWN = 0,
    IMG_FMT_BMP,
    IMG_FMT_JPEG
} image_format_t;

// Decoded image structure
typedef struct {
    uint16_t *pixels;      // RGB565 pixel data
    uint16_t width;        // Original width
    uint16_t height;       // Original height
    image_format_t format; // Source format
} decoded_image_t;

// Detect image format from file header
image_format_t detect_image_format(const char *filepath);

// Decode image file (BMP or JPG) with auto-scaling to fit target size
// Returns true on success, false on failure
bool decode_image_file(const char *filepath, uint16_t target_width, uint16_t target_height, decoded_image_t *out_image);

// Free decoded image
void free_decoded_image(decoded_image_t *image);

#endif // IMAGE_DECODER_H
