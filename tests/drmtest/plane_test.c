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
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "xf86drm.h"
#include "xf86drmMode.h"
#include "drm_fourcc.h"

#include "util/common.h"
#include "util/format.h"
#include "util/kms.h"
#include "util/pattern.h"

#include "buffers.h"

struct crtc {
	drmModeCrtc *crtc;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	drmModeModeInfo *mode;
};

struct encoder {
	drmModeEncoder *encoder;
};

struct connector {
	drmModeConnector *connector;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	char *name;
};

struct fb {
	drmModeFB *fb;
};

struct plane {
	drmModePlane *plane;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
};

struct resources {
	drmModeRes *res;
	drmModePlaneRes *plane_res;

	struct crtc *crtcs;
	struct encoder *encoders;
	struct connector *connectors;
	struct fb *fbs;
	struct plane *planes;
};

struct device {
	int fd;

	struct resources *resources;

	struct {
		unsigned int width;
		unsigned int height;

		unsigned int fb_id;
		struct bo *bo;
	} mode;

	int use_atomic;
	drmModeAtomicReq *req;
};

struct plane_info;
struct plane_arg {
	uint32_t plane_id;  /* the id of plane to use */
	uint32_t crtc_id;  /* the id of CRTC to bind to */
	bool has_position;
	int32_t x, y;
	uint32_t w, h;
	double scale;
	unsigned int fb_id;
	unsigned int old_fb_id;
	struct bo *bo;
	struct bo *old_bo;
	char format_str[5]; /* need to leave room for terminating \0 */
	unsigned int fourcc;
	struct plane_info *flip;
};

struct property_arg {
	uint32_t obj_id;
	uint32_t obj_type;
	char name[DRM_PROP_NAME_LEN+1];
	uint32_t prop_id;
	uint64_t value;
};

#define PLANE_FLIP_NUM	2

struct plane_info {
	struct plane_arg *p;
	uint32_t fb_id[PLANE_FLIP_NUM];
	struct bo *bo[PLANE_FLIP_NUM];
	int32_t crtc_x, crtc_y;
	int32_t crtc_w, crtc_h;
	unsigned int pattern[PLANE_FLIP_NUM];
	unsigned int vbl_count;
	unsigned int swap_count;
	struct timeval start;
	uint32_t flags;
	int draw_flip;
};


static void free_resources(struct resources *res)
{
	int i;

	if (!res)
		return;

#define free_resource(_res, __res, type, Type)					\
	do {									\
		if (!(_res)->type##s)						\
			break;							\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			if (!(_res)->type##s[i].type)				\
				break;						\
			drmModeFree##Type((_res)->type##s[i].type);		\
		}								\
		free((_res)->type##s);						\
	} while (0)

#define free_properties(_res, __res, type)					\
	do {									\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			drmModeFreeObjectProperties(res->type##s[i].props);	\
			free(res->type##s[i].props_info);			\
		}								\
	} while (0)

	if (res->res) {
		free_properties(res, res, crtc);

		free_resource(res, res, crtc, Crtc);
		free_resource(res, res, encoder, Encoder);

		for (i = 0; i < res->res->count_connectors; i++)
			free(res->connectors[i].name);

		free_resource(res, res, connector, Connector);
		free_resource(res, res, fb, FB);

		drmModeFreeResources(res->res);
	}

	if (res->plane_res) {
		free_properties(res, plane_res, plane);

		free_resource(res, plane_res, plane, Plane);

		drmModeFreePlaneResources(res->plane_res);
	}

	free(res);
}

static struct resources *get_resources(struct device *dev)
{
	struct resources *res;
	int i;

	res = calloc(1, sizeof(*res));
	if (res == 0)
		return NULL;

	drmSetClientCap(dev->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	res->res = drmModeGetResources(dev->fd);
	if (!res->res) {
		fprintf(stderr, "drmModeGetResources failed: %s\n",
			strerror(errno));
		goto error;
	}

	res->crtcs = calloc(res->res->count_crtcs, sizeof(*res->crtcs));
	res->encoders = calloc(res->res->count_encoders, sizeof(*res->encoders));
	res->connectors = calloc(res->res->count_connectors, sizeof(*res->connectors));
	res->fbs = calloc(res->res->count_fbs, sizeof(*res->fbs));

	if (!res->crtcs || !res->encoders || !res->connectors || !res->fbs)
		goto error;

#define get_resource(_res, __res, type, Type)					\
	do {									\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			(_res)->type##s[i].type =				\
				drmModeGet##Type(dev->fd, (_res)->__res->type##s[i]); \
			if (!(_res)->type##s[i].type)				\
				fprintf(stderr, "could not get %s %i: %s\n",	\
					#type, (_res)->__res->type##s[i],	\
					strerror(errno));			\
		}								\
	} while (0)

	get_resource(res, res, crtc, Crtc);
	get_resource(res, res, encoder, Encoder);
	get_resource(res, res, connector, Connector);
	get_resource(res, res, fb, FB);

	/* Set the name of all connectors based on the type name and the per-type ID. */
	for (i = 0; i < res->res->count_connectors; i++) {
		struct connector *connector = &res->connectors[i];
		drmModeConnector *conn = connector->connector;
		int num;

		num = asprintf(&connector->name, "%s-%u",
			 util_lookup_connector_type_name(conn->connector_type),
			 conn->connector_type_id);
		if (num < 0)
			goto error;
	}

#define get_properties(_res, __res, type, Type)					\
	do {									\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			struct type *obj = &res->type##s[i];			\
			unsigned int j;						\
			obj->props =						\
				drmModeObjectGetProperties(dev->fd, obj->type->type##_id, \
							   DRM_MODE_OBJECT_##Type); \
			if (!obj->props) {					\
				fprintf(stderr,					\
					"could not get %s %i properties: %s\n", \
					#type, obj->type->type##_id,		\
					strerror(errno));			\
				continue;					\
			}							\
			obj->props_info = calloc(obj->props->count_props,	\
						 sizeof(*obj->props_info));	\
			if (!obj->props_info)					\
				continue;					\
			for (j = 0; j < obj->props->count_props; ++j)		\
				obj->props_info[j] =				\
					drmModeGetProperty(dev->fd, obj->props->props[j]); \
		}								\
	} while (0)

	get_properties(res, res, crtc, CRTC);
	get_properties(res, res, connector, CONNECTOR);

	for (i = 0; i < res->res->count_crtcs; ++i)
		res->crtcs[i].mode = &res->crtcs[i].crtc->mode;

	res->plane_res = drmModeGetPlaneResources(dev->fd);
	if (!res->plane_res) {
		fprintf(stderr, "drmModeGetPlaneResources failed: %s\n",
			strerror(errno));
		return res;
	}

	res->planes = calloc(res->plane_res->count_planes, sizeof(*res->planes));
	if (!res->planes)
		goto error;

	get_resource(res, plane_res, plane, Plane);
	get_properties(res, plane_res, plane, PLANE);

	return res;

error:
	free_resources(res);
	return NULL;
}

static void set_property(struct device *dev, struct property_arg *p)
{
	drmModeObjectProperties *props = NULL;
	drmModePropertyRes **props_info = NULL;
	const char *obj_type;
	int ret;
	int i;

	p->obj_type = 0;
	p->prop_id = 0;

#define find_object(_res, __res, type, Type)					\
	do {									\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			struct type *obj = &(_res)->type##s[i];			\
			if (obj->type->type##_id != p->obj_id)			\
				continue;					\
			p->obj_type = DRM_MODE_OBJECT_##Type;			\
			obj_type = #Type;					\
			props = obj->props;					\
			props_info = obj->props_info;				\
		}								\
	} while(0)								\

	find_object(dev->resources, res, crtc, CRTC);
	if (p->obj_type == 0)
		find_object(dev->resources, res, connector, CONNECTOR);
	if (p->obj_type == 0)
		find_object(dev->resources, plane_res, plane, PLANE);
	if (p->obj_type == 0) {
		fprintf(stderr, "Object %i not found, can't set property\n",
			p->obj_id);
			return;
	}

	if (!props) {
		fprintf(stderr, "%s %i has no properties\n",
			obj_type, p->obj_id);
		return;
	}

	for (i = 0; i < (int)props->count_props; ++i) {
		if (!props_info[i])
			continue;
		if (strcmp(props_info[i]->name, p->name) == 0)
			break;
	}

	if (i == (int)props->count_props) {
		fprintf(stderr, "%s %i has no %s property\n",
			obj_type, p->obj_id, p->name);
		return;
	}

	p->prop_id = props->props[i];

	if (!dev->use_atomic)
		ret = drmModeObjectSetProperty(dev->fd, p->obj_id, p->obj_type,
									   p->prop_id, p->value);
	else
		ret = drmModeAtomicAddProperty(dev->req, p->obj_id, p->prop_id, p->value);

	if (ret < 0)
		fprintf(stderr, "failed to set %s %i property %s to %" PRIu64 ": %s\n",
			obj_type, p->obj_id, p->name, p->value, strerror(errno));
}

/* -------------------------------------------------------------------------- */
static bool format_support(const drmModePlanePtr ovr, uint32_t fmt)
{
	unsigned int i;

	for (i = 0; i < ovr->count_formats; ++i) {
		if (ovr->formats[i] == fmt)
			return true;
	}

	return false;
}

static void plane_vblank_handler(int fd, unsigned int frame, unsigned int sec,
                           unsigned int usec, void *data)
{
        drmVBlank vbl;
        struct timeval end;
        struct plane_info *pi = data;
	struct plane_arg *p = pi->p;
	int id =  pi->swap_count % PLANE_FLIP_NUM;
        double t;

        pi->vbl_count++;
        pi->swap_count++;

	if (pi->draw_flip)
		bo_fill_pattern(pi->bo[id], p->fourcc, p->w, p->h, pi->pattern[id]);

	/* set next plane */
	if (drmModeSetPlane(fd, p->plane_id, p->crtc_id, pi->fb_id[id],
		    pi->flags, pi->crtc_x, pi->crtc_y, pi->crtc_w, pi->crtc_h,
		    0, 0, p->w << 16, p->h << 16)) {
		fprintf(stderr, "failed to set plane fb[%d]:%d (counts %d): %s\n",
			pi->swap_count % PLANE_FLIP_NUM, 
			pi->fb_id[pi->swap_count % PLANE_FLIP_NUM],
			pi->swap_count, strerror(errno));
	}

	/* send wait for event signal with read/DRM_EVENT_VBLANK */
        vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
        vbl.request.sequence = 1;
        vbl.request.signal = (unsigned long)data;

	drmWaitVBlank(fd, &vbl);

        if (pi->vbl_count == 60) {
                gettimeofday(&end, NULL);
                t = end.tv_sec + end.tv_usec * 1e-6 -
                        (pi->start.tv_sec + pi->start.tv_usec * 1e-6);
                fprintf(stderr, "freq: %.02fHz\n", pi->vbl_count / t);
                pi->vbl_count = 0;
                pi->start = end;
        }
}

static int test_plane_flip(struct device *dev, struct plane_arg *p,
			unsigned int flags, int draw_flip)
{
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
	uint32_t plane_id;
	uint32_t plane_flags = 0;
	int crtc_x, crtc_y, crtc_w, crtc_h;
	struct crtc *crtc = NULL;
	unsigned int pipe;
	unsigned int i;
	drmEventContext evctx;
	drmVBlank vbl;
	struct plane_info *pi;
	int ret;
	/* UTIL_PATTERN_TILES, UTIL_PATTERN_PLAIN, UTIL_PATTERN_SMPTE */
	int fill_pattern[3] = {
		UTIL_PATTERN_TILES, UTIL_PATTERN_SMPTE, UTIL_PATTERN_PLAIN
	};

	pi = malloc((sizeof *pi));
	if (pi == NULL) {
		fprintf(stderr, "memory allocation failed\n");
		return 1;
	}
	memset(pi, 0, (sizeof *pi));

	p->flip = pi;
	pi->p = p;

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
		drmModePlane *ovr = dev->resources->planes[i].plane;
		if (!ovr)
			continue;

		if (plane_id && plane_id != ovr->plane_id)
			continue;

		if (!format_support(ovr, p->fourcc))
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

	for (i = 0; i < PLANE_FLIP_NUM; i++) {
		pi->pattern[i] = fill_pattern[(i % 3)];
		pi->bo[i] = bo_create(dev->fd, p->fourcc, p->w, p->h, handles,
			     		pitches, offsets, pi->pattern[i]);
		if (pi->bo[i] == NULL)
			return -1;

		/* just use single plane format for now.. */
		if (drmModeAddFB2(dev->fd, p->w, p->h, p->fourcc,
				handles, pitches, offsets, &pi->fb_id[i], plane_flags)) {
			fprintf(stderr, "failed to add fb: %s\n", strerror(errno));
			return -1;
		}
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

	/* Get current count first */
	vbl.request.type = DRM_VBLANK_RELATIVE;
	vbl.request.sequence = 0;
	ret = drmWaitVBlank(dev->fd, &vbl);
	if (ret != 0) {
		printf("drmWaitVBlank (relative) failed ret: %i\n", ret);
		return -1;
	}
	printf("starting count: %d\n", vbl.request.sequence);

	/* Queue an event for frame + 1 */
	vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
	vbl.request.sequence = 1;
	vbl.request.signal = (unsigned long)pi;
	ret = drmWaitVBlank(dev->fd, &vbl);
	if (ret != 0) {
		printf("drmWaitVBlank (relative, event) failed ret: %i\n", ret);
		return -1;
	}

	/* note src coords (last 4 args) are in Q16 format */
	if (drmModeSetPlane(dev->fd, plane_id, crtc->crtc->crtc_id, pi->fb_id[0],
			    flags, crtc_x, crtc_y, crtc_w, crtc_h,
			    0, 0, p->w << 16, p->h << 16)) {
		fprintf(stderr, "failed to enable plane: %s\n",
			strerror(errno));
		return -1;
	}

	pi->crtc_x = crtc_x;
	pi->crtc_y = crtc_y;
	pi->crtc_w = crtc_w;
	pi->crtc_h = crtc_h;
	pi->flags = flags;
	pi->vbl_count = 0;
	pi->draw_flip = draw_flip;

	gettimeofday(&pi->start, NULL);

	/* Set up our event handler */
	memset(&evctx, 0, sizeof evctx);
	evctx.version = DRM_EVENT_CONTEXT_VERSION;
	evctx.vblank_handler = plane_vblank_handler;
	evctx.page_flip_handler = NULL;

	while (1) {
		struct timeval timeout = { .tv_sec = 3, .tv_usec = 0 };
		fd_set fds;

		/* wait signal from drm/kms */
		FD_ZERO(&fds);
		FD_SET(0, &fds);
		FD_SET(dev->fd, &fds);
		ret = select(dev->fd + 1, &fds, NULL, NULL, &timeout);
		if (ret <= 0) {
			fprintf(stderr, "select timed out or error (ret %d)\n",
				ret);
			continue;
		} else if (FD_ISSET(0, &fds)) {
			break;
		}

		/* get vblank event and run handler: DRM_EVENT_VBLANK */
		ret = drmHandleEvent(dev->fd, &evctx);
	     	if (ret != 0) {
			printf("drmHandleEvent failed: %i\n", ret);
			return -1;
	     	}
	}

	return 0;
}

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

		if (!format_support(ovr, p->fourcc))
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

	plane_bo = bo_create(dev->fd, p->fourcc, p->w, p->h, handles,
			     pitches, offsets, UTIL_PATTERN_TILES);
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

static void clear_plane_flip(struct device *dev, struct plane_arg *p)
{
	struct plane_info *pi = p->flip;
	unsigned int i;
 
	if (!p->flip)
		return;

	for (i = 0; i < PLANE_FLIP_NUM; i++) {
		if (pi->fb_id[i])
			drmModeRmFB(dev->fd, pi->fb_id[i]);
		if (pi->bo[i])
			bo_destroy(pi->bo[i]);
	}

	free(p->flip);
}

static void clear_plane(struct device *dev, struct plane_arg *p)
{
	if (p->fb_id)
		drmModeRmFB(dev->fd, p->fb_id);

	if (p->bo)
		bo_destroy(p->bo);
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

static void usage(char *name)
{
	fprintf(stderr, "usage: %s [-acDdefMPpsCvw]\n", name);

	fprintf(stderr, "\n Test options:\n\n");
	fprintf(stderr, "\t-P <plane_id>@<crtc_id>:<w>x<h>[+<x>+<y>][*<scale>][@<format>]\tset a plane\n");
	fprintf(stderr, "\t-v\ttest vsynced page flipping\n");
	fprintf(stderr, "\t-u\tdraw flipping\n");
	fprintf(stderr, "\t-w <obj_id>:<prop_name>:<value>\tset property\n");

	fprintf(stderr, "\n Generic options:\n\n");
	fprintf(stderr, "\t-M module\tuse the given driver\n");
	fprintf(stderr, "\t-D device\tuse the given device\n");

	fprintf(stderr, "\n\tDefault is to dump all info.\n");
	exit(0);
}

static char optstr[] = "aD:M:P:s:vw:u";

int main(int argc, char **argv)
{
	struct device dev;

	int c;
	int test_flip = 0;
	char *device = NULL;
	char *module = NULL;
	unsigned int i;
	unsigned int prop_count = 0;
	struct plane_arg *plane_arg = NULL;
	struct property_arg *prop_args = NULL;
	int draw_flip = 0;
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
		case 'v':
			test_flip = 1;
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
		case 'u':
			draw_flip = 1;
			break;
		default:
			usage(argv[0]);
			break;
		}
	}

	dev.fd = util_open(device, module);
	if (dev.fd < 0)
		return -1;

	dev.resources = get_resources(&dev);
	if (!dev.resources) {
		drmClose(dev.fd);
		return 1;
	}

	for (i = 0; i < prop_count; ++i)
		set_property(&dev, &prop_args[i]);

	if (plane_arg) {
		uint64_t cap = 0;

		ret = drmGetCap(dev.fd, DRM_CAP_DUMB_BUFFER, &cap);
		if (ret || cap == 0) {
			fprintf(stderr, "driver doesn't support the dumb buffer API\n");
			return 1;
		}

		if (test_flip)
			test_plane_flip(&dev, plane_arg, DRM_MODE_ATOMIC_NONBLOCK, draw_flip);
		else
			set_plane(&dev, plane_arg);

		getchar();

		if (test_flip)
			clear_plane_flip(&dev, plane_arg);
		else
			clear_plane(&dev, plane_arg);
	}

	free_resources(dev.resources);

	return 0;
}
