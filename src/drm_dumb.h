#ifndef __DRM_DUMB__
#define __DRM_DUMB__

#include <xf86drm.h>
#include <xf86drmMode.h>

typedef struct dumb_buffer {
    int fd;
    uint32_t chosen_connector_id;
    drmModeModeInfo chosen_mode;

    drmModeCrtcPtr orig_crtc;
    drmModeModeInfo orig_mode;

    uint32_t fb;
    struct drm_mode_create_dumb creq;
    struct drm_mode_destroy_dumb dreq;
    struct drm_mode_map_dumb mreq;

    void * map;
} dumb_buffer_t;

dumb_buffer_t * InitDrmDumb();
void CleanupDrmDumb(dumb_buffer_t *);

#endif

