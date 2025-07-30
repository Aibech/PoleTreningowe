#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#define DEVICE_PATH "/dev/dri/card1"  //Your actual GPU, may it be diffrent ending e. g. .../card0
#define BMP_HEADER_SIZE 54

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
    int height = *(int*)&header[18];
    int width = *(int*)&header[22];
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


//-------------------------------------------------------------------------------------------------------------------------------------
//-------INT-MAIN()--------------------------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    //Variables
   int opt;
   char *image_path = NULL;
   int image_idle=0;

   //getopt loop
   while((opt =getopt(argc,argv, "f:t:h")) != -1){
    switch (opt){

    case 'f' :
        printf("Input file is: %s\n",optarg);
        image_path=strdup(optarg);
        break;
    case 't':
        image_idle=atoi(optarg);
        printf("Image will be on screen for %d seconds\n",image_idle);
        
        break;
    case 'h':
    case '?': //Both options do the same operations
        printf("\nThis app is used for showing image on screen.\n");
        printf("Format: ./app [ -f FILENAME ] [ -t TIME_ON_SCREEN_IN_SEC ] [ -h  ]\n");
        printf("-f : Indicates path to the file which will be shown on a screen\n");
        printf("-t : Indicates how much time in seconds will be shown the image on screen.\n");
        printf("-h : Help about this app.\n");
        return 1;
        break;
    default:
        printf("Please add option -h at the end of the command\n");
        return 1;
    }

   }

    printf("Open device DRM: %s\n", DEVICE_PATH);
    int fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open DRM");
        return 1;
    }
    printf("DRM has been opened\n");

    drmModeRes* res = drmModeGetResources(fd);
    if (!res) {
        perror("drmModeGetResources");
        close(fd);
        return 1;
    }

    drmModeConnector* connector = NULL;
    drmModeModeInfo mode;
    uint32_t connector_id = 0;

    // We're looking for connected monitor
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            connector = conn;
            connector_id = conn->connector_id;
            mode = conn->modes[0];
            printf("Connected monitor has been found (connector ID: %d)\n", connector_id);
            break;
        }
        drmModeFreeConnector(conn);
    }

    if (!connector) {
        fprintf(stderr, "There's not any connected display.\n");
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    drmModeEncoder* encoder = drmModeGetEncoder(fd, connector->encoder_id);
    if (!encoder) {
        fprintf(stderr, "Encoder couldn't be downloaded.\n");
        drmModeFreeConnector(connector);
        drmModeFreeResources(res);
        close(fd);
        return 1;
    }

    uint32_t crtc_id = encoder->crtc_id;
    printf("CRTC ID: %d\n", crtc_id);

    // Creating dumb buffer
    struct drm_mode_create_dumb creq = {
        .width = mode.hdisplay,     //small issue potentially change width with height to repair issue with non-square images, again changed to normal""
        .height = mode.vdisplay,
        .bpp = 32,
    };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        return 1;
    }
    printf("Dumb buffer has been created (pitch: %u, size: %u)\n", creq.pitch, creq.size);

    // Adding framebuffer
    struct drm_mode_fb_cmd fb = {
        .width = mode.hdisplay,     //small issue potentially change width with height to repair issue with non-square images, again changed to normal""
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
    printf("Framebuffer has been added (FB ID: %u)\n", fb.fb_id);

    // Mapping memory
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
///////////////////////////////////////////////RELATIVE PATH//////////////////
	// Load file BMP
	uint8_t* bmp_data = NULL;
	int bmp_width = 0, bmp_height = 0;
    if (image_path !=NULL)
    {
        if (load_bmp(image_path, &bmp_data, &bmp_height, &bmp_width) != 0) {
            fprintf(stderr, "Error during loading BMP\n");
            return 1;
        }
    }
	else if (load_bmp("image.bmp", &bmp_data, &bmp_height, &bmp_width) != 0) {
		fprintf(stderr, "Error during loading BMP\n");
		return 1;
	}
	     //Fill the screen with color
    for (int y = 0; y < mode.vdisplay; y++) {
        for (int x = 0; x < mode.hdisplay; x++) {
            int offset = y * creq.pitch + x * 4;
            map[offset + 0] = 0xFF; // Blue
            map[offset + 1] = 0xFF; // Green
            map[offset + 2] = 0xFF; // Red
            map[offset + 3] = 0x00;
        }
    }

	//Show Image
	// 25.07

	int offset_x = 0;
	int offset_y = 0;

	if (bmp_width < mode.hdisplay && bmp_height < mode.vdisplay) {
    // Set in the middle of the screen if image is smaller than display
    		offset_x = (mode.hdisplay - bmp_width) / 2;
    		offset_y = (mode.vdisplay - bmp_height) / 2;
    		printf("Center of image: offset_x = %d, offset_y = %d\n", offset_x, offset_y);
		}
	else {
		printf("Resolution of image is equal to display resolution");
    }
	blit_bmp_original_size(map, creq.pitch,
                       mode.hdisplay, mode.vdisplay,
                       bmp_data, bmp_width, bmp_height,
                       offset_x, offset_y);


	free(bmp_data);
/*
	 //Fill the screen with color
    for (int y = 0; y < mode.vdisplay; y++) {
        for (int x = 0; x < mode.hdisplay; x++) {
            int offset = y * creq.pitch + x * 4;
            map[offset + 0] = 0x00; // Blue
            map[offset + 1] = 0x00; // Green
            map[offset + 2] = 0xFF; // Red
            map[offset + 3] = 0x00;
        }
    }
    printf("✔ Display shows colors.\n");
*/
    // CRTC setting
    if (drmModeSetCrtc(fd, crtc_id, fb.fb_id, 0, 0, &connector_id, 1, &mode) != 0) {
        perror("drmModeSetCrtc");
        return 1;
    }
    printf("CRTC set — you should see image.\n");

    if (image_idle != 0)
    {
        sleep(image_idle);
    }
    

    // Cleaning
    munmap(map, creq.size);
    drmModeFreeEncoder(encoder);
    drmModeFreeConnector(connector);
    drmModeFreeResources(res);
    close(fd);
    free(image_path);
    return 0;
}
