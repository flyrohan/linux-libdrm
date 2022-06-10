#ifndef __UTIL_IMAGE_H__
#define __UTIL_IMAGE_H__

#include "pattern.h"
#include "bmp.h"
#include "buffers.h"

enum util_image_type {
	UTIL_IMAGE_BMP,
	UTIL_IMAGE_RAW,
};

struct util_image_info {
        const char *file;
        enum util_image_type type;
        unsigned int fourcc;
        unsigned int width;
        unsigned int height;
        unsigned int stride;
};

struct bo *util_bo_create_pattern(int fd, unsigned int format,
		                  unsigned int width, unsigned int height,
                                  unsigned int handles[4], unsigned int pitches[4],
                                  unsigned int offsets[4],
                                  enum util_fill_pattern pattern);
int util_bo_fill_pattern(struct bo *bo, unsigned int format,
                    unsigned int width, unsigned int height,
                    enum util_fill_pattern pattern);

struct bo *util_bo_create_image(int fd, unsigned int format,
                                unsigned int width, unsigned int height,
                                unsigned int handles[4], unsigned int pitches[4],
                                unsigned int offsets[4],
                                const struct util_image_info *image);

#endif
