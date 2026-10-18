#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h> // close(), sleep()
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm_dumb.h"

int fd;
drmModeRes *res;
drmModeConnectorPtr con;

uint32_t chosen_crtc_id = 0;
drmModeModeInfoPtr chosen_mode = NULL;

drmModeCrtcPtr orig_crtc;
drmModeModeInfo orig_mode;

uint32_t fb;
struct drm_mode_create_dumb creq;
struct drm_mode_destroy_dumb dreq;
struct drm_mode_map_dumb mreq;

int crtc_used_by_connector(int fd, drmModeRes *res, drmModeCrtcPtr crtc) {

    int ret = 0;

    for (int i = 0; i < res->count_connectors; i++) {
        if (ret)
            break;

        drmModeConnectorPtr con = drmModeGetConnectorCurrent(fd, res->connectors[i]);
        if (!con) {
            fprintf(stderr, "FAILED: could not get connector information for connector index %d\n", res->connectors[i]);
            ret = -1;
            break;
        }
        for (int j = 0; j < con->count_encoders; j++) {
            drmModeEncoderPtr enc = drmModeGetEncoder(fd, con->encoders[j]);
            if (!enc) {
                fprintf(stderr, "FAILED: could not get encoder information for encoder index %d\n", con->encoders[i]);
                ret = -1;
                break;
            }

            if (enc->crtc_id == crtc->crtc_id) {
                printf("    found a match: encoder with id %u uses CRTC with id %u\n", enc->encoder_id, crtc->crtc_id);
                drmModeFreeEncoder(enc);
                ret = 1;
                break;
            }
            drmModeFreeEncoder(enc);
        }
        drmModeFreeConnector(con);
    }
    return ret;
}

int is_crtc_active_on_encoder(int fd, drmModeRes *res, drmModeCrtcPtr crtc, drmModeEncoderPtr enc) {

    if (enc->crtc_id == crtc->crtc_id) {
        return 1;
    } else {
        return 0;
    }
}

void * InitDrmDumb() {

    void * map;

    // why O_CLOEXEC?
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("Failed to open card /dev/dri/card0");
        return 0;
    }

    res = drmModeGetResources(fd);
    if (!res) {
        fprintf(stderr, "FAILED: could not do drmModeGetResources.\n");
        return 0;
    }

    printf("there are %d connectors:\n", res->count_connectors);
    for (int i = 0; i < res->count_connectors; i++) {
        // this is the same ID that I see in 'modetest -c'
        uint32_t id = res->connectors[i];

        // only show currently known information, don't probe the connector (like reading the EDID)
        con = drmModeGetConnectorCurrent(fd, id);
        if (!con) {
            fprintf(stderr, "FAILED: could not get connector information for connector id %d\n", i);
            return 0;
        }
        printf("  id %u\n", id);
        const char *con_name = drmModeGetConnectorTypeName(con->connector_type);
        printf("    type %s\n", con_name ? con_name : "unknown");
        printf("    status: ");
        switch (con->connection) {
            case DRM_MODE_CONNECTED:
                printf("connected\n");
                break;
            case DRM_MODE_DISCONNECTED:
                printf("disconnected\n");
                break;
            case DRM_MODE_UNKNOWNCONNECTION:
                printf("unknown\n");
                break;
            default:
                printf("not a known type of connection status\n");
                break;
        }

        // I should free this after using it in this loop iteration
        drmModeFreeConnector(con);
    }

    // now repeat the same loop, but select the first active connector
    bool found = false;
    for (int i = 0; i < res->count_connectors; i++) {
        uint32_t id = res->connectors[i];

        con = drmModeGetConnectorCurrent(fd, id);
        if (!con) {
            fprintf(stderr, "FAILED: could not get connector information for connector id %d\n", i);
            return 0;
        }
        if (con->connection == DRM_MODE_CONNECTED) {
            printf("connector ID %u is connected, using this one\n", id);
            found = true;
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "Could not find an active connector. Connectors with an unknown state are ignored. Aborting\n");
        return 0;
    }

    printf("modes:\n");
    for (int i = 0; i < con->count_modes; i++) {
        bool is_preferred = con->modes[i].type & DRM_MODE_TYPE_PREFERRED;
        printf("  %s%s\n", con->modes[i].name, is_preferred ? " (preferred)" : "");
        if (is_preferred) {
            chosen_mode = &con->modes[i];
        }
    }
    if (!chosen_mode) {
        fprintf(stderr, "Could not find a preferred mode - aborting");
        return 0;
    }

    for (int i = 0; i < con->count_encoders; i++) {
        drmModeEncoderPtr enc = drmModeGetEncoder(fd, con->encoders[i]);

        printf("encoder %d, possible CRTCs: 0x%x\n", enc->encoder_id, enc->possible_crtcs);

        /* from 'man 7 drm-kms' */
        /* iterate all global CRTCs */
        for (int j = 0; j < res->count_crtcs; ++j) {
            printf("  at j = %d\n", j);
            /* check whether this CRTC works with the encoder */
            if (!(enc->possible_crtcs & (1 << j)))
                continue;

            drmModeCrtcPtr crtc = drmModeGetCrtc(fd, res->crtcs[j]);
            printf("    checking CRTC %u\n", res->crtcs[j]);
            printf("    it uses buffer ID %u\n", crtc->buffer_id);
            int in_use = crtc_used_by_connector(fd, res, crtc);
            if (in_use == 0) {
                printf("    CRTC with ID %u is free\n", crtc->crtc_id);
            } else if (in_use > 0) {
                printf("    CRTC with ID %u is in use\n", crtc->crtc_id);
            } else {
                fprintf(stderr, "Something went wrong here. Aborting\n");
                return 0;
            }

            // evil: ignore the above check and store the first crtc_id that we find for this encoder, then use it later
            if (!chosen_crtc_id && (is_crtc_active_on_encoder(fd, res, crtc, enc) || !in_use)) {
                chosen_crtc_id = crtc->crtc_id;
                printf("    CRTC with id %u is either free or active on this encoder (this one will be used)\n", crtc->crtc_id);
            }

            drmModeFreeCrtc(crtc);
        }

        drmModeFreeEncoder(enc);
    }

    orig_crtc = drmModeGetCrtc(fd, chosen_crtc_id);
    orig_mode = orig_crtc->mode;

    /* check this DRM driver's capabilities */
    struct drm_get_cap capreq;
    int ret;

    memset(&capreq, 0, sizeof(capreq));
    capreq.capability = DRM_CAP_DUMB_BUFFER;
    ret = drmIoctl(fd, DRM_IOCTL_GET_CAP, &capreq);
    if (ret < 0) {
        fprintf(stderr, "IOCTL failed\n");
        return 0;
    }
    if (!capreq.value) {
        fprintf(stderr, "dumb buffers not supported by the driver :(\n");
        return 0;
    }

    memset(&capreq, 0, sizeof(capreq));
    capreq.capability = DRM_CAP_DUMB_PREFERRED_DEPTH;
    ret = drmIoctl(fd, DRM_IOCTL_GET_CAP, &capreq);
    if (ret < 0) {
        fprintf(stderr, "IOCTL failed\n");
        return 0;
    }
    printf("preferred bit depth for dumb buffers (not the same as bpp): %llu\n", capreq.value);

    /* create dumb buffer */
    memset(&creq, 0, sizeof(creq));
    creq.width = chosen_mode->hdisplay;
    creq.height = chosen_mode->vdisplay;
    creq.bpp = 32;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret < 0) {
        fprintf(stderr, "Could not create a dumb buffer\n");
        return 0;
    }
    /* creq.pitch, creq.handle and creq.size are filled by this ioctl with
     * the requested values and can be used now. */

    printf("DRM created a dumb buffer with width=%u, height=%u\n", creq.width, creq.height);
    printf("pitch (number of bytes between two consecutive lines) of the new dumb buffer: %u\n", creq.pitch);

    /* create framebuffer object for the dumb-buffer */
    ret = drmModeAddFB(fd, chosen_mode->hdisplay, chosen_mode->vdisplay, 24, 32, creq.pitch, creq.handle, &fb);
    if (ret) {
        fprintf(stderr, "Could not create a frame buffer object\n");
        return 0;
    }
    /* the framebuffer "fb" can now used for scanout with KMS */

    printf("Created a framebuffer object with ID %u, using the dumb buffer handle\n", fb);
    printf("let's see which framebuffers are reported by drmModeGetResources now\n");
    // update resources to find the new framebuffer
    res = drmModeGetResources(fd);
    for (int i = 0; i < res->count_fbs; i++) {
        drmModeFBPtr fbp = drmModeGetFB(fd, res->fbs[i]);
        printf("  found fb with ID %u\n", res->fbs[i]);
        drmModeFreeFB(fbp);
    }

    /* prepare buffer for memory mapping */
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (ret) {
        fprintf(stderr, "Could not prepare the dumb buffer mapping\n");
        return 0;
    }
    /* mreq.offset now contains the new offset that can be used with mmap() */

    /* perform actual memory mapping */
    map = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (map == MAP_FAILED) {
        fprintf(stderr, "Could not mmap() the memory\n");
        return 0;
    }

    /* clear the framebuffer to 0 */
    memset(map, 0, creq.size);

    ret = drmModeSetCrtc(fd, chosen_crtc_id, fb, 0, 0, &con->connector_id, 1, chosen_mode);
    if (ret) {
        fprintf(stderr, "Could not set the CRTC\n");
        return 0;
    }

    return map;
}

void CleanupDrmDumb(void * map) {
    int ret;

    printf("DRM cleaning up\n");

    drmModeSetCrtc(fd, orig_crtc->crtc_id, orig_crtc->buffer_id, orig_crtc->x, orig_crtc->y, &con->connector_id, 1, &orig_mode);
    drmModeRmFB(fd, fb);
    munmap(map, creq.size);
    /* destroy dumb buffer */
    memset(&dreq, 0, sizeof(dreq));
    dreq.handle = creq.handle;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    if (ret < 0) {
        fprintf(stderr, "Could not destroy the dumb buffer\n");
    }

    drmModeFreeCrtc(orig_crtc);
    drmModeFreeConnector(con);
    drmModeFreeResources(res);
    close(fd);

    printf("DRM cleanup done\n");
}

