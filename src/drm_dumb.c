#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h> // close(), sleep()

#include "drm_dumb.h"


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

dumb_buffer_t * InitDrmDumb() {
    drmModeRes *res;
    drmModeConnectorPtr con;

    uint32_t chosen_crtc_id = 0;

    dumb_buffer_t * dumb = malloc(sizeof(dumb_buffer_t));
    if (!dumb) {
        fprintf(stderr, "FAILED: could not allocate a dumb_buffer object.\n");
        return 0;
    }

    // why O_CLOEXEC?
    dumb->fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (dumb->fd < 0) {
        perror("Failed to open card /dev/dri/card0");
        return 0;
    }

    res = drmModeGetResources(dumb->fd);
    if (!res) {
        fprintf(stderr, "FAILED: could not do drmModeGetResources.\n");
        return 0;
    }

    printf("there are %d connectors:\n", res->count_connectors);
    for (int i = 0; i < res->count_connectors; i++) {
        // this is the same ID that I see in 'modetest -c'
        uint32_t id = res->connectors[i];

        // only show currently known information, don't probe the connector (like reading the EDID)
        con = drmModeGetConnectorCurrent(dumb->fd, id);
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

        con = drmModeGetConnectorCurrent(dumb->fd, id);
        if (!con) {
            fprintf(stderr, "FAILED: could not get connector information for connector id %d\n", i);
            return 0;
        }
        if (con->connection == DRM_MODE_CONNECTED) {
            printf("connector ID %u is connected, using this one\n", id);
            found = true;
            dumb->chosen_connector_id = con->connector_id;
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "Could not find an active connector. Connectors with an unknown state are ignored. Aborting\n");
        return 0;
    }

    printf("modes:\n");
    found = false;
    for (int i = 0; i < con->count_modes; i++) {
        bool is_preferred = con->modes[i].type & DRM_MODE_TYPE_PREFERRED;
        printf("  %s%s\n", con->modes[i].name, is_preferred ? " (preferred)" : "");
        if (is_preferred) {
            dumb->chosen_mode = con->modes[i];
            found = true;
        }
    }
    if (!found) {
        fprintf(stderr, "Could not find a preferred mode - aborting");
        return 0;
    }

    for (int i = 0; i < con->count_encoders; i++) {
        drmModeEncoderPtr enc = drmModeGetEncoder(dumb->fd, con->encoders[i]);

        printf("encoder %d, possible CRTCs: 0x%x\n", enc->encoder_id, enc->possible_crtcs);

        /* from 'man 7 drm-kms' */
        /* iterate all global CRTCs */
        for (int j = 0; j < res->count_crtcs; ++j) {
            printf("  at j = %d\n", j);
            /* check whether this CRTC works with the encoder */
            if (!(enc->possible_crtcs & (1 << j)))
                continue;

            drmModeCrtcPtr crtc = drmModeGetCrtc(dumb->fd, res->crtcs[j]);
            printf("    checking CRTC %u\n", res->crtcs[j]);
            printf("    it uses buffer ID %u\n", crtc->buffer_id);
            int in_use = crtc_used_by_connector(dumb->fd, res, crtc);
            if (in_use == 0) {
                printf("    CRTC with ID %u is free\n", crtc->crtc_id);
            } else if (in_use > 0) {
                printf("    CRTC with ID %u is in use\n", crtc->crtc_id);
            } else {
                fprintf(stderr, "Something went wrong here. Aborting\n");
                return 0;
            }

            // evil: ignore the above check and store the first crtc_id that we find for this encoder, then use it later
            if (!chosen_crtc_id && (is_crtc_active_on_encoder(dumb->fd, res, crtc, enc) || !in_use)) {
                chosen_crtc_id = crtc->crtc_id;
                printf("    CRTC with id %u is either free or active on this encoder (this one will be used)\n", crtc->crtc_id);
            }

            drmModeFreeCrtc(crtc);
        }

        drmModeFreeEncoder(enc);
    }
    drmModeFreeConnector(con);

    dumb->orig_crtc = drmModeGetCrtc(dumb->fd, chosen_crtc_id);
    dumb->orig_mode = dumb->orig_crtc->mode;

    /* check this DRM driver's capabilities */
    struct drm_get_cap capreq;
    int ret;

    memset(&capreq, 0, sizeof(capreq));
    capreq.capability = DRM_CAP_DUMB_BUFFER;
    ret = drmIoctl(dumb->fd, DRM_IOCTL_GET_CAP, &capreq);
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
    ret = drmIoctl(dumb->fd, DRM_IOCTL_GET_CAP, &capreq);
    if (ret < 0) {
        fprintf(stderr, "IOCTL failed\n");
        return 0;
    }
    printf("preferred bit depth for dumb buffers (not the same as bpp): %llu\n", capreq.value);

    /* create dumb buffer */
    memset(&dumb->creq, 0, sizeof(dumb->creq));
    dumb->creq.width = dumb->chosen_mode.hdisplay;
    dumb->creq.height = dumb->chosen_mode.vdisplay;
    dumb->creq.bpp = 32;
    ret = drmIoctl(dumb->fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb->creq);
    if (ret < 0) {
        fprintf(stderr, "Could not create a dumb buffer\n");
        return 0;
    }
    /* creq.pitch, creq.handle and creq.size are filled by this ioctl with
     * the requested values and can be used now. */

    printf("DRM created a dumb buffer with width=%u, height=%u\n", dumb->creq.width, dumb->creq.height);
    printf("pitch (number of bytes between two consecutive lines) of the new dumb buffer: %u\n", dumb->creq.pitch);

    /* create framebuffer object for the dumb-buffer */
    ret = drmModeAddFB(dumb->fd, dumb->chosen_mode.hdisplay, dumb->chosen_mode.vdisplay, 24, 32, dumb->creq.pitch, dumb->creq.handle, &dumb->fb);
    if (ret) {
        fprintf(stderr, "Could not create a frame buffer object\n");
        return 0;
    }
    /* the framebuffer "fb" can now used for scanout with KMS */

    printf("Created a framebuffer object with ID %u, using the dumb buffer handle\n", dumb->fb);
    printf("let's see which framebuffers are reported by drmModeGetResources now\n");
    // update resources to find the new framebuffer
    res = drmModeGetResources(dumb->fd);
    for (int i = 0; i < res->count_fbs; i++) {
        drmModeFBPtr fbp = drmModeGetFB(dumb->fd, res->fbs[i]);
        printf("  found fb with ID %u\n", res->fbs[i]);
        drmModeFreeFB(fbp);
    }

    /* prepare buffer for memory mapping */
    memset(&dumb->mreq, 0, sizeof(dumb->mreq));
    dumb->mreq.handle = dumb->creq.handle;
    ret = drmIoctl(dumb->fd, DRM_IOCTL_MODE_MAP_DUMB, &dumb->mreq);
    if (ret) {
        fprintf(stderr, "Could not prepare the dumb buffer mapping\n");
        return 0;
    }
    /* mreq.offset now contains the new offset that can be used with mmap() */

    /* perform actual memory mapping */
    dumb->map = mmap(0, dumb->creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, dumb->fd, dumb->mreq.offset);
    if (dumb->map == MAP_FAILED) {
        fprintf(stderr, "Could not mmap() the memory\n");
        return 0;
    }

    /* clear the framebuffer to 0 */
    memset(dumb->map, 0, dumb->creq.size);

    ret = drmModeSetCrtc(dumb->fd, chosen_crtc_id, dumb->fb, 0, 0, &dumb->chosen_connector_id, 1, &dumb->chosen_mode);
    if (ret) {
        fprintf(stderr, "Could not set the CRTC\n");
        return 0;
    }

    drmModeFreeResources(res);

    return dumb;
}

void CleanupDrmDumb(dumb_buffer_t * dumb) {
    int ret;

    printf("DRM cleaning up\n");

    drmModeSetCrtc(dumb->fd, dumb->orig_crtc->crtc_id, dumb->orig_crtc->buffer_id, dumb->orig_crtc->x, dumb->orig_crtc->y, &dumb->chosen_connector_id, 1, &dumb->orig_mode);
    drmModeRmFB(dumb->fd, dumb->fb);
    munmap(dumb->map, dumb->creq.size);
    /* destroy dumb buffer */
    memset(&dumb->dreq, 0, sizeof(dumb->dreq));
    dumb->dreq.handle = dumb->creq.handle;
    ret = drmIoctl(dumb->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dumb->dreq);
    if (ret < 0) {
        fprintf(stderr, "Could not destroy the dumb buffer\n");
    }

    drmModeFreeCrtc(dumb->orig_crtc);
    close(dumb->fd);

    free(dumb);

    printf("DRM cleanup done\n");
}

