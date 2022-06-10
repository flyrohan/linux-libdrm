/*
 * DRM based mode setting test program
 * Copyright 2008 Tungsten Graphics
 *   Jakob Bornecrantz <jakob@tungstengraphics.com>
 * Copyright 2008 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <drm.h>
#include <drm_fourcc.h>

#include <libdrm_macros.h>
#include <xf86drm.h>

#include "common.h"
#include "format.h"
#include "image.h"

/*------------------------------------------------------------------------------
 * Pattern draw 
 *----------------------------------------------------------------------------*/
static unsigned int util_pattern_height(unsigned int format,
                       unsigned int width, unsigned int height)
{
        unsigned int virtual_height;

	switch (format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_YVU420:
		virtual_height = height * 3 / 2;
		break;

	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV61:
	case DRM_FORMAT_YUV422:
	case DRM_FORMAT_YVU422:
		virtual_height = height * 2;
		break;

	case DRM_FORMAT_YUV444:
	case DRM_FORMAT_YVU444:
		virtual_height = height * 3;
		break;

	default:
		virtual_height = height;
		break;
	}

        return virtual_height;
}

int util_bo_fill_pattern(struct bo *bo, unsigned int format,
	  unsigned int width, unsigned int height,
	  enum util_fill_pattern pattern)
{
	void *planes[3] = { 0, };
        unsigned int handles[4];
	unsigned int pitches[4];
	unsigned int offsets[4]; 
	void *virtual;
	int ret;

	ret = bo_map(bo, &virtual);
	if (ret) {
		fprintf(stderr, "failed to map buffer: %s\n",
			strerror(-errno));
		bo_destroy_dumb(bo);
		return -1;
	}

        ret = bo_get_property(format, width, height,
                         bo, virtual, handles, pitches, offsets, planes);
        if (ret) {
	        bo_unmap(bo);
		bo_destroy_dumb(bo);
                return -1;
        }

	util_fill_pattern(format, pattern, planes, width, height, pitches[0]);
	bo_unmap(bo);

	return 0;
}

struct bo *util_bo_create_pattern(int fd, unsigned int format,
                       unsigned int width, unsigned int height,
                       unsigned int handles[4], unsigned int pitches[4],
                       unsigned int offsets[4], enum util_fill_pattern pattern)
{
	unsigned int virtual_height;
	struct bo *bo;
	int bpp;
	void *planes[3] = { 0, };
	void *virtual;
	int ret;

        bpp = util_format_bpp(format, width, height);
        if (!bpp)
                return NULL;

        virtual_height = util_pattern_height(format, width, height);

	bo = bo_create_dumb(fd, width, virtual_height, bpp);
	if (!bo)
		return NULL;

	ret = bo_map(bo, &virtual);
	if (ret) {
		fprintf(stderr, "failed to map buffer: %s\n",
			strerror(-errno));
		bo_destroy_dumb(bo);
		return NULL;
	}

        ret = bo_get_property(format, width, height,
                         bo, virtual, handles, pitches, offsets, planes);
        if (ret) {
	        bo_unmap(bo);
		bo_destroy_dumb(bo);
                return NULL;
        }

	util_fill_pattern(format, pattern, planes, width, height, pitches[0]);
	bo_unmap(bo);

	return bo;
}

/*------------------------------------------------------------------------------
 * BMP draw 
 *----------------------------------------------------------------------------*/
static void
setpixel_555_565(void *p, int x, int y, int w, int h, __u32 v)
{
	*(__u16*)(p + (y * w + x) * 2) = (__u16)(
                        (((v >> 10) & 0x1F) << 11) |
                        (((v >> 5) & 0x1F) << 6) |
                        (v & 0x1F) );
}

static void
setpixel_565_888(void *p, int x, int y, int w, int h, __u32 v)
{
        /* B,G,R,A */
	*(__u8*)(p + (y * w + x) * 3 + 0) = (((v >> 0 ) << 3) & 0xf8) | (((v >> 0 ) >> 2) & 0x7);
        *(__u8*)(p + (y * w + x) * 3 + 1) = (((v >> 5 ) << 2) & 0xfc) | (((v >> 5 ) >> 4) & 0x3);
	*(__u8*)(p + (y * w + x) * 3 + 2) = (((v >> 11) << 3) & 0xf8) | (((v >> 11) >> 2) & 0x7);
}

static void
setpixel_565_8888(void *p, int x, int y, int w, int h, __u32 v)
{
        /* B,G,R,A */
	*(__u8*)(p + (y * w + x) * 4 + 0) = (((v >> 0 ) << 3) & 0xf8) | (((v >> 0 ) >> 2) & 0x7);
	*(__u8*)(p + (y * w + x) * 4 + 1) = (((v >> 5 ) << 2) & 0xfc) | (((v >> 5 ) >> 4) & 0x3);
	*(__u8*)(p + (y * w + x) * 4 + 2) = (((v >> 11) << 3) & 0xf8) | (((v >> 11) >> 2) & 0x7);
        *(__u8*)(p + (y * w + x) * 4 + 3) = 0xff; /* alpha to opacity */
}

static void
setpixel_888_565(void *p, int x, int y, int w, int h, __u32 v)
{
	*(__u16*)(p + (y * w + x) * 2) = (__u16)(
                        ((((v >> 16) & 0xFF) & 0xF8) << 8) | 
                        ((((v >> 8) & 0xFF) & 0xFC) << 3) |
                        (((v & 0xFF) & 0xF8) >> 3) );
}

static void
setpixel_888_888(void *p, int x, int y, int w, int h, __u32 v)
{
	p = p + (y * w + x) * 3;
	*(__u8*)(p++) = ((v>> 0)&0xFF);
	*(__u8*)(p++) = ((v>> 8)&0xFF);
	*(__u8*)(p)   = ((v>>16)&0xFF);
}

static void
setpixel_888_8888(void *p, int x, int y, int w, int h, __u32 v)
{
        /* alpha to opacity */
	*(__u32 *)(p + (y * w + x) * 4) = (0xFF000000) | (v & 0xFFFFFF);
}

static void
setpixel_8888_8888(void *p, int x, int y, int w, int h, __u32 v)
{
//        printf("[0x%08x] %4d x %4d\n", p, x, y);
	*(__u32 *)(p + (y * w + x) * 4) = (v);
}

typedef void(*PIXEL_FN)(void *p, int x, int y, int w, int h, __u32 v);

enum rgb_bitperpixel {
        rgb_bitperpixel_15 = 15,
        rgb_bitperpixel_16 = 16,
        rgb_bitperpixel_24 = 24,
        rgb_bitperpixel_32 = 32,
};

struct pixel_func {
        enum rgb_bitperpixel src;
        enum rgb_bitperpixel dst;
        PIXEL_FN func;
};

struct pixel_func pixel_fns[] = {
        { rgb_bitperpixel_15, rgb_bitperpixel_16, setpixel_555_565 }, 
        { rgb_bitperpixel_16, rgb_bitperpixel_16, NULL },
        { rgb_bitperpixel_16, rgb_bitperpixel_24, setpixel_565_888 },
        { rgb_bitperpixel_16, rgb_bitperpixel_32, setpixel_565_8888 },
        { rgb_bitperpixel_24, rgb_bitperpixel_16, setpixel_888_565 },
        { rgb_bitperpixel_24, rgb_bitperpixel_24, setpixel_888_888 },
        { rgb_bitperpixel_24, rgb_bitperpixel_32, setpixel_888_8888 },
        { rgb_bitperpixel_32, rgb_bitperpixel_16, setpixel_888_565 },
        { rgb_bitperpixel_32, rgb_bitperpixel_24, setpixel_888_888 },
        { rgb_bitperpixel_32, rgb_bitperpixel_32, setpixel_8888_8888 },
};

static int util_load_bmp(const char *file, void *virtual, 
                        unsigned int width, unsigned int height,
                        unsigned int stride, unsigned int bpp)
{
	bmp_t *bmp = NULL;
        PIXEL_FN fn = NULL;
	int j, i, n, m;
	unsigned int iwidht, iheight, ibpp;
	int x = 0, y = 0;
	int sx, sy, ex, ey;

        bmp = bmp_load(file, NULL);
        if (!bmp)
                return -1;

	iwidht = bmp_width(bmp);
	iheight = bmp_height(bmp);
        ibpp = bmp_bitperpixel(bmp);

        fprintf(stdout, "BMP - %s\n", file);
        fprintf(stdout, "BMP - %dx%d,%dbpp -> %dx%d,%dbpp\n",
                iwidht, iheight, ibpp, width, height, bpp);

	for (i = 0; i < (int)ARRAY_SIZE(pixel_fns); i++) {
                if (ibpp == pixel_fns[i].src && bpp == pixel_fns[i].dst) {
                        fn = pixel_fns[i].func;
                        break;
                }
        } 

        if (!fn) {
                fprintf(stderr, "Not support %dbpp -> %dbpp\n", ibpp, bpp);
                bmp_release(bmp);
                return -1;
        }

	sx = 0, sy = 0;
	ex = iwidht, ey = iheight;

        /* clipping and centor */
	if (iwidht > width) {
		sx = (iwidht - width) >> 1;
		ex = sx + width;
	} else if (iwidht < width) {
		x += (width - iwidht) >> 1;
		/* clear framebuffer */
		memset(virtual, 0, stride * height);
	}

        /* clipping and centor */
	if (iheight > height) {
		sy = (iheight - height) >> 1;
		ey = sy + height;
	} else if (iheight < height) {
		y += (height - iheight) >> 1;
		/* clear framebuffer */
		memset(virtual, 0, stride * height);
	}

        for (i = y, n = sy; n < ey; i++, n++) {
		for (j = x, m = sx; m < ex; j++, m++) {
			rgbpixel_t p = bmp_getpixel(bmp, m, n);
                        fn(virtual, j, i, width, height, (p.a << 24 | p.r << 16 | p.g << 8 | p.b)); 
		}
	}
        fprintf(stdout, "BMP - done\n");

        return 0;
}

struct bo *util_bo_create_image(int fd, unsigned int format,
                     unsigned int width, unsigned int height,
                     unsigned int handles[4], unsigned int pitches[4],
                     unsigned int offsets[4],
                     const struct util_image_info *image)
{
	struct bo *bo;
	int bpp;
	void *planes[3] = { 0, };
	void *virtual;
        struct stat st;
	int ret;

         if (!image || !image->file) {
                fprintf(stderr, "No input image info !!!\n");
                return NULL;
        }
               
        if (!stat(image->file, &st) && errno == EEXIST) {
                fprintf(stderr, "Image file not found :%s\n", image->file);
                return NULL;
        }

        bpp = util_format_bpp(format, width, height);
        if (!bpp)
                return NULL;

	bo = bo_create_dumb(fd, width, height, bpp);
	if (!bo)
		return NULL;

	ret = bo_map(bo, &virtual);
	if (ret) {
		fprintf(stderr, "failed to map buffer: %s\n",
			strerror(-errno));
		bo_destroy_dumb(bo);
		return NULL;
	}

        ret = bo_get_property(format, width, height,
                         bo, virtual, handles, pitches, offsets, planes);
        if (ret) {
	        bo_unmap(bo);
		bo_destroy_dumb(bo);
                return NULL;
        }
        util_load_bmp(image->file, planes[0], width, height, pitches[0], bpp);

	bo_unmap(bo);

	return bo;
}
