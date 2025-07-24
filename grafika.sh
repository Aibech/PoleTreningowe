#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <errno.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#define BMP_HEADER_SIZE 54

uint8_t* load_bmp(const char* path, int* width, int* height) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror("Nie można otworzyć pliku BMP");
        return NULL;
    }

    uint8_t header[BMP_HEADER_SIZE];
    fread(header, sizeof(uint8_t), BMP_HEADER_SIZE, f);

    *width = *(int*)&header[18];
    *height = *(int*)&header[22];

    int row_padded = (*width * 3 + 3) & (~3);
    uint8_t* data = malloc(row_padded * (*height));
    if (!data) {
        fprintf(stderr, "Nie udało się zaalokować pamięci na dane BMP\n");
        fclose(f);
        return NULL;
    }

    fseek(f, *(int*)&header[10], SEEK_SET);
    fread(data, sizeof(uint8_t), row_padded * (*height), f);
    fclose(f);

    printf("Załadowano obraz BMP: %dx%d\n", *width, *height);
    return data;
}

int main() {
    printf("[1] Otwieranie urządzenia DRM...\n");
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/dri/card0");
        return 1;
    }
    printf("    ✔️  Otworzono DRM\n");

    drmModeRes *resources = drmModeGetResources(fd);
    if (!resources) {
        fprintf(stderr, "❌ Nie udało się pobrać zasobów DRM\n");
        return 1;
    }

    drmModeConnector *connector = NULL;
    drmModeModeInfo mode;
    uint32_t connector_id = 0;

    printf("[2] Szukanie podłączonego złącza...\n");
    for (int i = 0; i < resources->count_connectors; i++) {
        connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (connector && connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
            connector_id = connector->connector_id;
            mode = connector->modes[0];
            printf("    ✔️  Znaleziono podłączony ekran: %dx%d\n", mode.hdisplay, mode.vdisplay);
            break;
        }
        drmModeFreeConnector(connector);
    }

    if (!connector_id) {
        fprintf(stderr, "❌ Nie znaleziono podłączonego złącza\n");
        return 1;
    }

    drmModeEncoder *encoder = drmModeGetEncoder(fd, connector->encoder_id);
    if (!encoder) {
        fprintf(stderr, "❌ Nie udało się pobrać encodera\n");
        return 1;
    }

    uint32_t crtc_id = encoder->crtc_id;

    printf("[3] Tworzenie dumb buffer...\n");
    struct drm_mode_create_dumb creq = {
        .width = mode.hdisplay,
        .height
