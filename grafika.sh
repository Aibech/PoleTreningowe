#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm.h>
#include <drm_mode.h>

#define BMP_HEADER_SIZE 54

// Funkcja do ładowania BMP (24-bit, bez kompresji)
uint8_t* load_bmp(const char* path, int* width, int* height) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    uint8_t header[BMP_HEADER_SIZE];
    fread(header, sizeof(uint8_t), BMP_HEADER_SIZE, f);

    *width = *(int*)&header[18];
    *height = *(int*)&header[22];

    int row_padded = (*width * 3 + 3) & (~3);
    uint8_t* data = malloc(row_padded * (*height));

    fseek(f, *(int*)&header[10], SEEK_SET);
    fread(data, sizeof(uint8_t), row_padded * (*height), f);
    fclose(f);
    return data;
}

int main() {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    drmModeRes *resources = drmModeGetResources(fd);
    drmModeConnector *connector = NULL;
    drmModeEncoder *encoder = NULL;
    drmModeModeInfo mode;
    uint32_t connector_id;

    // Znajdź pierwszy podłączony connector
    for (int i = 0; i < resources->count_connectors; i++) {
        connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
            connector_id = connector->connector_id;
            mode = connector->modes[0];
            break;
        }
        drmModeFreeConnector(connector);
    }

    if (!connector) {
        fprintf(stderr, "No connected connector found.\n");
        return 1;
    }

    // Utwórz dumb buffer
    struct drm_mode_create_dumb creq = {
        .width = mode.hdisplay,
        .height = mode.vdisplay,
        .bpp = 32,
    };
    drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);

    uint32_t handle = creq.handle;
    uint32_t pitch = creq.pitch;
    uint64_t size = creq.size;

    struct drm_mode_map_dumb mreq = {.handle = handle};
    drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);

    uint8_t* map = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    memset(map, 0, size); // Clear screen

    struct drm_mode_fb_cmd fb = {
        .width = mode.hdisplay,
        .height = mode.vdisplay,
        .pitch = pitch,
        .bpp = 32,
        .depth = 24,
        .handle = handle,
    };
    drmIoctl(fd, DRM_IOCTL_MODE_ADDFB, &fb);

    // Załaduj BMP
    int bmp_w, bmp_h;
    uint8_t* bmp_data = load_bmp("image.bmp", &bmp_w, &bmp_h);

    // Kopiuj piksele (zakładamy: 24bpp BMP do 32bpp framebuffera)
    for (int y = 0; y < bmp_h; y++) {
        for (int x = 0; x < bmp_w; x++) {
            int bmp_index = y * ((bmp_w * 3 + 3) & ~3) + x * 3;
            int fb_index = (bmp_h - y - 1) * pitch + x * 4;

            map[fb_index + 0] = bmp_data[bmp_index + 0]; // Blue
            map[fb_index + 1] = bmp_data[bmp_index + 1]; // Green
            map[fb_index + 2] = bmp_data[bmp_index + 2]; // Red
            map[fb_index + 3] = 0; // Alpha (opcjonalnie)
        }
    }

    // Ustaw CRTC
    drmModeSetCrtc(fd, connector->encoder_id, fb.fb_id, 0, 0, &connector_id, 1, &mode);

    sleep(5); // Zostaw obraz na ekranie przez 5 sekund

    // Posprzątaj
    munmap(map, size);
    free(bmp_data);
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    close(fd);
    return 0;
}
