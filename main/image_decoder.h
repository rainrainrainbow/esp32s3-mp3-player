#ifndef IMAGE_DECODER_H
#define IMAGE_DECODER_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool decode_bmp_file(const char *filepath, uint16_t *width, uint16_t *height, uint16_t **pixels);
void free_decoded_image(uint16_t *pixels);

#endif
