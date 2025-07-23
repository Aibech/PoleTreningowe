#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "framebuffer.h"

#define CARD "/dev/dri/card0"

typedef struct {
    int width;
    int height;
    uint8_t *data;
} BMPImage;

static BMPImage *load_bmp(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;

    uint8_t header[54];
    fread(header, sizeof(uint8_t), 54, f);

    int width = *(int*)&header[18];
    int height = *(int*)&header[22];
    int offset = *(int*)&header[10];
    int size = *(int*)&header[34];

    uint8_t *data = malloc(size);
    fseek(f, offset, SEEK_SET);
    fread(data, sizeof(uint8_t), size, f);
    fclose(f);

    BMPImage *img = malloc(sizeof(BMPImage));
    img->width = width;
    img->height = height;
    img->data = data;
    return img;
}

int drm_display_bmp(const char *filename) {
    int fd = open(CARD, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open DRM device");
        return -1;
    }

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) {
        perror("drmModeGetResources");
        close(fd);
        return -1;
    }

    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        conn = drmModeGetConnector(fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
            break;
        drmModeFreeConnector(conn);
        conn = NULL;
    }

    if (!conn) {
        fprintf(stderr, "No connected connector found\n");
        drmModeFreeResources(res);
        close(fd);
        return -1;
    }

    drmModeModeInfo mode = conn->modes[0];
    drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
    uint32_t crtc_id = enc->crtc_id;

    struct drm_mode_create_dumb create = {0};
    create.width = mode.hdisplay;
    create.height = mode.vdisplay;
    create.bpp = 32;

    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        return -1;
    }

    struct drm_mode_map_dumb map = {0};
    map.handle = create.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        return -1;
    }

    uint8_t *fb_ptr = mmap(0, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    if (fb_ptr == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    uint32_t fb_id;
    if (drmModeAddFB(fd, create.width, create.height, 24, 32, create.pitch, create.handle, &fb_id)) {
        perror("drmModeAddFB");
        return -1;
    }

    if (drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &conn->connector_id, 1, &mode)) {
        perror("drmModeSetCrtc");
        return -1;
    }

    BMPImage *img = load_bmp(filename);
    if (!img) {
        fprintf(stderr, "Failed to load BMP\n");
        return -1;
    }

    // Copy BMP data to framebuffer (assuming 24-bit BMP)
    for (int y = 0; y < img->height && y < mode.vdisplay; y++) {
        for (int x = 0; x < img->width && x < mode.hdisplay; x++) {
            int fb_index = (y * create.pitch) + x * 4;
            int bmp_index = ((img->height - y - 1) * img->width + x) * 3;

            fb_ptr[fb_index + 0] = img->data[bmp_index + 0]; // Blue
            fb_ptr[fb_index + 1] = img->data[bmp_index + 1]; // Green
            fb_ptr[fb_index + 2] = img->data[bmp_index + 2]; // Red
            fb_ptr[fb_index + 3] = 0xFF;                     // Alpha
        }
    }

    sleep(5); // Wyświetl obraz przez 5 sekund

    munmap(fb_ptr, create.size);
    drmModeRmFB(fd, fb_id);
    drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    close(fd);
    free(img->data);
    free(img);
    return 0;
}
