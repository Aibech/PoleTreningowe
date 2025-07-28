#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
//#include <sys/mman.h>
#include <errno.h>

//#include <xf86drm.h>
//#include <xf86drmMode.h>
//#include <drm/drm.h>
//#include <drm/drm_mode.h>

#define DEVICE_PATH "/dev/dri/card1"  // 👈 Twój faktyczny GPU
#define BMP_HEADER_SIZE 54

/*/ Wczytywanie BMP (24-bit RGB, bez kompresji)
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
*/
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

void blit_bmp_original_size(uint8_t* fb, uint32_t fb_pitch, uint32_t fb_width, uint32_t fb_height,
                            uint8_t* bmp, int bmp_width, int bmp_height,
                            int offset_x, int offset_y) {
    int bmp_row_padded = (bmp_width * 3 + 3) & ~3;

    for (int y = 0; y < bmp_height; y++) {
        int screen_y = offset_y + y;
        if (screen_y >= (int)fb_height) break;

        for (int x = 0; x < bmp_width; x++) {
            int screen_x = offset_x + x;
            if (screen_x >= (int)fb_width) break;

            int bmp_y = bmp_height - 1 - y;  // BMP is bottom-up
            int bmp_offset = bmp_y * bmp_row_padded + x * 3;
            int fb_offset = screen_y * fb_pitch + screen_x * 4;

            fb[fb_offset + 0] = bmp[bmp_offset + 0]; // Blue
            fb[fb_offset + 1] = bmp[bmp_offset + 1]; // Green
            fb[fb_offset + 2] = bmp[bmp_offset + 2]; // Red
            fb[fb_offset + 3] = 0;
        }
    }
}


//----------------------------------------------
//-------INT-MAIN()----------------------------
//---------------------------------------------
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

	// Wczytaj plik BMP
	uint8_t* bmp_data = NULL;
	int bmp_width = 0, bmp_height = 0;
	if (load_bmp("image.bmp", &bmp_data, &bmp_height, &bmp_width) != 0) {
		fprintf(stderr, "Błąd wczytywania BMP\n");
		return 1;
	}

	// Wyświetl obrazek na ekran wersja 
	//Wersja z skalowaniem 25.07
	
	int offset_x = 0;
int offset_y = 0;

	if (bmp_width < mode.hdisplay && bmp_height < mode.vdisplay) {
    // Wyśrodkuj jeśli obrazek jest mniejszy niż ekran
    offset_x = (mode.hdisplay - bmp_width) / 2;
    offset_y = (mode.vdisplay - bmp_height) / 2;
    printf("Centrowanie obrazu: offset_x = %d, offset_y = %d\n", offset_x, offset_y);
	} 
	else {
    // W przeciwnym wypadku pokaż od rogu
			printf("Obraz równej wielkości jak ekran – wyświetlam od lewego górnego rogu\n");
		}
	
	blit_bmp_original_size(map, creq.pitch,
                       mode.hdisplay, mode.vdisplay,
                       bmp_data, bmp_width, bmp_height,
                       offset_x, offset_y);


	free(bmp_data);
    
	
	/*/ TEST: wypełnij ekran na czerwono
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
    */
	
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
