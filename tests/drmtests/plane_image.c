/*
 * DRM based mode setting test program
 */

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "common.h"
#include "format.h"
#include "pattern.h"
#include "kms.h"

#include "image.h"

struct plane_info;
struct plane_arg {
	uint32_t plane_id;  /* the id of plane to use */
	uint32_t crtc_id;  /* the id of CRTC to bind to */
	bool has_position;
	int32_t x, y;
	uint32_t w, h;
	double scale;
	unsigned int fb_id;
	struct bo *bo;
	char format_str[5]; /* need to leave room for terminating \0 */
	unsigned int fourcc;
        struct util_image_info image;
};

struct plane_info {
	struct plane_arg *p;
	uint32_t fb_id;
	struct bo *bo;
	int32_t crtc_x, crtc_y;
	int32_t crtc_w, crtc_h;
	uint32_t flags;
};

static int set_plane(struct device *dev, struct plane_arg *p)
{
	drmModePlane *ovr;
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
	uint32_t plane_id;
	struct bo *plane_bo;
	uint32_t plane_flags = 0;
	int crtc_x, crtc_y, crtc_w, crtc_h;
	struct crtc *crtc = NULL;
	unsigned int pipe;
	unsigned int i;

	/* Find an unused plane which can be connected to our CRTC. Find the
	 * CRTC index first, then iterate over available planes.
	 */
	for (i = 0; i < (unsigned int)dev->resources->res->count_crtcs; i++) {
		if (p->crtc_id == dev->resources->res->crtcs[i]) {
			crtc = &dev->resources->crtcs[i];
			pipe = i;
			break;
		}
	}

	if (!crtc) {
		fprintf(stderr, "CRTC %u not found\n", p->crtc_id);
		return -1;
	}

	plane_id = p->plane_id;

	for (i = 0; i < dev->resources->plane_res->count_planes; i++) {
		ovr = dev->resources->planes[i].plane;
		if (!ovr)
			continue;

		if (plane_id && plane_id != ovr->plane_id)
			continue;

		if (!drm_format_support(ovr, p->fourcc))
			continue;

		if ((ovr->possible_crtcs & (1 << pipe)) &&
		    (ovr->crtc_id == 0 || ovr->crtc_id == p->crtc_id)) {
			plane_id = ovr->plane_id;
			break;
		}
	}

	if (i == dev->resources->plane_res->count_planes) {
		fprintf(stderr, "no unused plane available for CRTC %u\n",
			crtc->crtc->crtc_id);
		return -1;
	}

	fprintf(stderr, "testing %dx%d@%s overlay plane %u\n",
		p->w, p->h, p->format_str, plane_id);

	plane_bo = util_bo_create_image(dev->fd, p->fourcc, p->w, p->h, handles,
			     pitches, offsets, &p->image);
	if (plane_bo == NULL)
		return -1;

	p->bo = plane_bo;

	/* just use single plane format for now.. */
	if (drmModeAddFB2(dev->fd, p->w, p->h, p->fourcc,
			handles, pitches, offsets, &p->fb_id, plane_flags)) {
		fprintf(stderr, "failed to add fb: %s\n", strerror(errno));
		return -1;
	}

	crtc_w = p->w * p->scale;
	crtc_h = p->h * p->scale;
	if (!p->has_position) {
		/* Default to the middle of the screen */
		crtc_x = (crtc->mode->hdisplay - crtc_w) / 2;
		crtc_y = (crtc->mode->vdisplay - crtc_h) / 2;
	} else {
		crtc_x = p->x;
		crtc_y = p->y;
	}

	/* note src coords (last 4 args) are in Q16 format */
	if (drmModeSetPlane(dev->fd, plane_id, crtc->crtc->crtc_id, p->fb_id,
			    plane_flags, crtc_x, crtc_y, crtc_w, crtc_h,
			    0, 0, p->w << 16, p->h << 16)) {
		fprintf(stderr, "failed to enable plane: %s\n",
			strerror(errno));
		return -1;
	}

	ovr->crtc_id = crtc->crtc->crtc_id;

	return 0;
}

static void clear_plane(struct device *dev, struct plane_arg *p)
{
	if (p->fb_id)
		drmModeRmFB(dev->fd, p->fb_id);

	if (p->bo)
		bo_destroy_dumb(p->bo);
}

static int parse_plane(struct plane_arg *plane, const char *p)
{
	char *end;

	plane->plane_id = strtoul(p, &end, 10);
	if (*end != '@')
		return -EINVAL;

	p = end + 1;
	plane->crtc_id = strtoul(p, &end, 10);
	if (*end != ':')
		return -EINVAL;

	p = end + 1;
	plane->w = strtoul(p, &end, 10);
	if (*end != 'x')
		return -EINVAL;

	p = end + 1;
	plane->h = strtoul(p, &end, 10);

	if (*end == '+' || *end == '-') {
		plane->x = strtol(end, &end, 10);
		if (*end != '+' && *end != '-')
			return -EINVAL;
		plane->y = strtol(end, &end, 10);

		plane->has_position = true;
	}

	if (*end == '*') {
		p = end + 1;
		plane->scale = strtod(p, &end);
		if (plane->scale <= 0.0)
			return -EINVAL;
	} else {
		plane->scale = 1.0;
	}

	if (*end == '@') {
		p = end + 1;
		if (strlen(p) != 4)
			return -EINVAL;

		strcpy(plane->format_str, p);
	} else {
		strcpy(plane->format_str, "XR24");
	}

	plane->fourcc = util_format_fourcc(plane->format_str);
	if (plane->fourcc == 0) {
		fprintf(stderr, "unknown format %s\n", plane->format_str);
		return -EINVAL;
	}

	return 0;
}

static int parse_property(struct property_arg *p, const char *arg)
{
	if (sscanf(arg, "%d:%32[^:]:%" SCNu64, &p->obj_id, p->name, &p->value) != 3)
		return -1;

	p->obj_type = 0;
	p->name[DRM_PROP_NAME_LEN] = '\0';

	return 0;
}

#define parse_strtoul(ptr, p, n) { \
        if (!ptr) return; \
        p = strtoul(ptr, NULL, 10); \
        ptr = strtok(NULL, ","); \
}
static void parse_image_type(char *arg, struct plane_arg *plane_arg)
{
        struct util_image_info *image = &plane_arg->image;
        char *ptr = strtok(arg, ",");

        if (!ptr)
                return;

        image->file = ptr;
        ptr = strtok(NULL, ",");
        parse_strtoul(ptr, image->type, 16);
        parse_strtoul(ptr, image->type, 16);
        parse_strtoul(ptr, image->fourcc, 16);
        parse_strtoul(ptr, image->width, 10);
        parse_strtoul(ptr, image->height, 10);
        parse_strtoul(ptr, image->stride, 10);
}


static void usage(char *name)
{
	fprintf(stderr, "usage: %s [potions] -i [options]\n", name);

	fprintf(stderr, "\n Test options:\n\n");
	fprintf(stderr, "\t-P <plane_id>@<crtc_id>:<w>x<h>[+<x>+<y>][*<scale>][@<format>]\tset a plane\n");
	fprintf(stderr, "\t-w <obj_id>:<prop_name>:<value>\tset property\n");
	fprintf(stderr, "\t-i <file>,<type>,<fourcc,w,h,stride>\tload image <type> (bmp, raw)\n");

	fprintf(stderr, "\n Generic options:\n\n");
	fprintf(stderr, "\t-M module\tuse the given driver\n");
	fprintf(stderr, "\t-D device\tuse the given device\n");

	fprintf(stderr, "\n\tDefault is to dump all info.\n");
	exit(0);
}

static char optstr[] = "aD:M:P:w:i:";

int main(int argc, char **argv)
{
	struct device dev;

	int c;
	char *device = NULL;
	char *module = NULL;
	unsigned int i;
	unsigned int prop_count = 0;
	struct plane_arg *plane_arg = NULL;
	struct property_arg *prop_args = NULL;
	unsigned int args = 0;
	int ret;

	memset(&dev, 0, sizeof dev);

	opterr = 0;
	while ((c = getopt(argc, argv, optstr)) != -1) {
		args++;

		switch (c) {
		case 'D':
			device = optarg;
			args--;
			break;
		case 'M':
			module = optarg;
			/* Preserve the default behaviour of dumping all information. */
			args--;
			break;
		case 'P':
			plane_arg = malloc(sizeof *plane_arg);
			if (plane_arg == NULL) {
				fprintf(stderr, "memory allocation failed\n");
				return 1;
			}
			memset(plane_arg, 0, sizeof(*plane_arg));

			if (parse_plane(plane_arg, optarg) < 0)
				usage(argv[0]);
			break;
		case 'w':
			prop_args = realloc(prop_args,
					   (prop_count + 1) * sizeof *prop_args);
			if (prop_args == NULL) {
				fprintf(stderr, "memory allocation failed\n");
				return 1;
			}
			memset(&prop_args[prop_count], 0, sizeof(*prop_args));

			if (parse_property(&prop_args[prop_count], optarg) < 0)
				usage(argv[0]);

			prop_count++;
			break;
                case 'i':
                        if (plane_arg == NULL) {
			        usage(argv[0]);
                                return -1;
                        }
                        parse_image_type(optarg, plane_arg);
                        break;
		default:
			usage(argv[0]);
			break;
		}
	}

        if (!plane_arg->image.file) {
                usage(argv[0]);
                return -1;
        }

	dev.fd = drm_open(device, module);
	if (dev.fd < 0)
		return -1;

	dev.resources = drm_get_resources(&dev);
	if (!dev.resources) {
		drm_close(dev.fd);
		return 1;
	}

	for (i = 0; i < prop_count; ++i)
		drm_set_property(&dev, &prop_args[i]);

	if (plane_arg) {
		uint64_t cap = 0;

		ret = drmGetCap(dev.fd, DRM_CAP_DUMB_BUFFER, &cap);
		if (ret || cap == 0) {
			fprintf(stderr, "driver doesn't support the dumb buffer API\n");
			return 1;
		}

		set_plane(&dev, plane_arg);
		getchar();
		clear_plane(&dev, plane_arg);
	}

	drm_free_resources(dev.resources);

	return 0;
}
