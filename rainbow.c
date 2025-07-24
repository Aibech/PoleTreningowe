#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <termios.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#define DEVICE_PATH "/dev/dri/card1"
#define FRAME_TIME_US 500000  // 0.5 sekundy
#define DURATION_SEC 10

// Kolory RGB (BGR kolejność w framebufferze)
const uint8_t rainbow[][3] = {
    { 0x00, 0x00, 0xFF },  // Czerwony
    { 0x00, 0x7F, 0xFF },  // Pomarańczowy
    { 0x00, 0xFF, 0xFF },  // Żółty
    { 0x00, 0xFF, 0x00 },  // Zielony
    { 0xFF, 0x00, 0x00 },  // Niebieski
    { 0x82, 0x00, 0x4B },  // Indygo
    { 0xD3, 0x00, 0x94 },  // Fioletowy
};

void fill_color(uint8_t* map, uint32_t pitch, uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int offset = y * pitch + x * 4;
            map[offset + 0] = b;
            map[offset + 1] = g;
            map[offset + 2] = r;
            map[offset + 3] = 0;
        }
    }
}

int load_bmp(const char* filename, uint8_t** out_data, int* out_width, int* out_height) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        perror("fopen");
        return -1;
    }

    uint8_t header[54];
    fread(header, sizeof(uint8_t), 54, f);

    if (header[0] != 'B' || header[1] != 'M') {
        printf("Not a BMP file\n");
        fclose(f);
        return -1;
    }

    int dataOffset = *(int*)&header[10];
    int width = *(int*)&header[18];
    int height = *(int*)&header[22];
    int bpp = *(short*)&header[28];

    if (bpp != 24) {
        printf("Only 24-bit BMP supported\n");
        fclose(f);
        return -1;
    }

    int row_padded = (width * 3 + 3) & (~3);
    uint8_t* data = malloc(row_padded * height);
    if (!data) {
        fclose(f);
        return -1;
    }

    fseek(f, dataOffset, SEEK_SET);
    fread(data, sizeof(uint8_t), row_padded * height, f);
    fclose(f);

    *out_data = data;
    *out_width = width;
    *out_height = height;
    return 0;
}

void blit_bmp(uint8_t* fb, uint32_t fb_pitch, uint32_t fb_width, uint32_t fb_height,
              uint8_t* bmp, int bmp_width, int bmp_height) {
    int row_padded = (bmp_width * 3 + 3) & (~3);

    for (int y = 0; y < bmp_height && y < fb_height; y++) {
        for (int x = 0; x < bmp_width && x < fb_width; x++) {
            int bmp_y = bmp_height - 1 - y;  // BMP is bottom-up
            int bmp_offset = bmp_y * row_padded + x * 3;
            int fb_offset = y * fb_pitch + x * 4;

            fb[fb_offset + 0] = bmp[bmp_offset + 0];  // Blue
            fb[fb_offset + 1] = bmp[bmp_offset + 1];  // Green
            fb[fb_offset + 2] = bmp[bmp_offset + 2];  // Red
            fb[fb_offset + 3] = 0;
        }
    }
}

// Wyłączenie trybu echo/kanonicznego (żeby zignorować klawisze)
void disable_terminal_input() {
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

void restore_terminal_input() {
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag |= ICANON | ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

int main() {
    disable_terminal_input();

    int fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    drmModeRes* res = drmModeGetResources(fd);
    drmModeConnector* connector = NULL;
    drmModeModeInfo mode;
    uint32_t connector_id = 0;

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            connector = conn;
            connector_id = conn->connector_id;
            mode = conn->modes[0];
            break;
        }
        drmModeFreeConnector(conn);
    }

    drmModeEncoder* encoder = drmModeGetEncoder(fd, connector->encoder_id);
    uint32_t crtc_id = encoder->crtc_id;

    struct drm_mode_create_dumb creq = {
        .width = mode.hdisplay,
        .height = mode.vdisplay,
        .bpp = 32,
    };
    drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);

    struct drm_mode_fb_cmd fb = {
        .width = mode.hdisplay,
        .height = mode.vdisplay,
        .pitch = creq.pitch,
        .bpp = 32,
        .depth = 24,
        .handle = creq.handle,
    };
    drmIoctl(fd, DRM_IOCTL_MODE_ADDFB, &fb);

    struct drm_mode_map_dumb mreq = { .handle = creq.handle };
    drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);

    uint8_t* map = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);

    drmModeSetCrtc(fd, crtc_id, fb.fb_id, 0, 0, &connector_id, 1, &mode);

    int num_colors = sizeof(rainbow) / sizeof(rainbow[0]);
    int frame_count = DURATION_SEC * 1000000 / FRAME_TIME_US;

    for (int i = 0; i < frame_count; i++) {
        int color_index = i % num_colors;
        uint8_t r = rainbow[color_index][2];
        uint8_t g = rainbow[color_index][1];
        uint8_t b = rainbow[color_index][0];
        fill_color(map, creq.pitch, mode.hdisplay, mode.vdisplay, r, g, b);
        usleep(FRAME_TIME_US);
    }

    munmap(map, creq.size);
    drmModeFreeEncoder(encoder);
    drmModeFreeConnector(connector);
    drmModeFreeResources(res);
    close(fd);
    restore_terminal_input();
    return 0;
}
