#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#define DEVICE_PATH "/dev/dri/card1"  // 👈 Twój faktyczny GPU
#define BMP_HEADER_SIZE 54

// Wczytywanie BMP (24-bit RGB, bez kompresji)
uint8_t* load_bmp(const char* path, int* width, int* height) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror("fopen BMP");
        return NULL;
    }

    uint8_t header[BMP_HEADER_SIZE];
    fread(header, sizeof(uint8_t), BMP_HEADER_SIZE, f);

    *width = *(int*)&header[18];
    *height = *(int*)&header[22];

    int row_padded = (*width * 3 + 3) & (~3);
    uint8_t* data = malloc(row_padded * (*height));
    if (!data) {
        perror("malloc BMP");
        fclose(f);
        return NULL;
    }

    fseek(f, *(int*)&header[10], SEEK_SET);
    fread(data, sizeof(uint8_t), row_padded * (*height), f);
    fclose(f);
    return data;
}

int main() {
    printf("Otwieram urządzenie DRM: %s\n", DEVICE_PATH);
    int fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open DRM");
        return 1;
    }
    printf("✔️ Otworzono DRM\n");

    drmModeRes* res = drmModeGetResources(fd);
    if (!res) {
        perror("drmModeGetResources");
        close(fd);
        return 1;
    }

    drmModeConnector* connector = NULL;
    drmModeModeInfo mode;
    uint32_t connector_id = 0;

    // Szukamy podłączonego wyświetlacza
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            connector = conn;
            connector_id = conn->connector_id;
            mode = conn->modes[0];
            printf("✔️ Znaleziono podłączony ekran (connector ID: %d)\n", connector_id);
            break;
        }
        drmModeFreeConnector(conn);
    }

    if (!connector) {
        fprintf(stderr, "❌ Nie znaleziono podłączonego wyświetlacza.\n");
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    drmModeEncoder* encoder = drmModeGetEncoder(fd, connector->encoder_id);
    if (!encoder) {
        fprintf(stderr, "❌ Nie udało się pobrać encodera.\n");
        drmModeFreeConnector(connector);
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    uint32_t crtc_id = encoder->crtc_id;
    printf("✔️ Używamy CRTC ID: %d\n", crtc_id);

    // Tworzenie dumb buffer
    struct drm_mode_create_dumb creq = {
        .width = mode.hdisplay,
        .height = mode.vdisplay,
        .bpp = 32,
    };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        return 1;
    }
    printf("✔️ Utworzono dumb buffer (pitch: %u, size: %u)\n", creq.pitch, creq.size);

    // Dodanie framebuffer
    struct drm_mode_fb_cmd fb = {
        .width = mode.hdisplay,
        .height = mode.vdisplay,
        .pitch = creq.pitch,
        .bpp = 32,
        .depth = 24,
        .handle = creq.handle,
    };
    if (drmIoctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) != 0) {
        perror("DRM_IOCTL_MODE_ADDFB");
        return 1;
    }
    printf("✔️ Dodano framebuffer (FB ID: %u)\n", fb.fb_id);

    // Mapowanie pamięci
    struct drm_mode_map_dumb mreq = { .handle = creq.handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) != 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        return 1;
    }

    uint8_t* map = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    // TEST: wypełnij ekran na czerwono
    for (int y = 0; y < mode.vdisplay; y++) {
        for (int x = 0; x < mode.hdisplay; x++) {
            int offset = y * creq.pitch + x * 4;
            map[offset + 0] = 0x00; // Blue
            map[offset + 1] = 0x00; // Green
            map[offset + 2] = 0xFF; // Red
            map[offset + 3] = 0x00;
        }
    }
    printf("✔️ Ekran wypełniono czerwonym kolorem\n");

    // Ustawienie CRTC
    if (drmModeSetCrtc(fd, crtc_id, fb.fb_id, 0, 0, &connector_id, 1, &mode) != 0) {
        perror("drmModeSetCrtc");
        return 1;
    }
    printf("✔️ Ustawiono CRTC — powinieneś zobaczyć czerwony ekran\n");

    sleep(5);

    // Sprzątanie
    munmap(map, creq.size);
    drmModeFreeEncoder(encoder);
    drmModeFreeConnector(connector);
    drmModeFreeResources(res);
    close(fd);
    return 0;
}
