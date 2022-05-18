/*
 * Copyright (C) 2015-2016 Thomas <tomas123 @ EEVblog Electronics Community Forum>
 * Copyright (C) 2022 Instituto de Pesquisas Eldorado <eldorado.org.br>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fcntl.h>
#include <libusb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "font5x7.h"
#include "jpeglib.h"
#include "plank.h"

/* defines */

// color visible image
#define FRAME_WIDTH1    640
#define FRAME_HEIGHT1   480

// colorized thermal image
#define FRAME_WIDTH2    frame_width2
#define FRAME_HEIGHT2   frame_height2
// original width/height
#define FRAME_OWIDTH2   frame_owidth2
#define FRAME_OHEIGHT2  frame_oheight2

// max chars in line
#define MAX_CHARS2      (FRAME_WIDTH2 / 6 + (flirone_pro ? 0 : 1))

#define FRAME_FORMAT1   V4L2_PIX_FMT_MJPEG
#define FRAME_FORMAT2   V4L2_PIX_FMT_RGB24

#define FONT_COLOR_DFLT 0xff

/* USB defs */
#define VENDOR_ID 0x09cb
#define PRODUCT_ID 0x1996

/* These are just to make USB requests easier to read */
#define REQ_TYPE    1
#define REQ         0xb
#define V_STOP      0
#define V_START     1
#define INDEX(i)    (i)
#define LEN(l)      (l)

// buffer for EP 0x85 chunks
#define BUF85SIZE   1048576     // size got from android app

/* global data */

static char video_device1[64];
static char video_device2[64];
static int frame_width2 = 80;
static int frame_height2 = 80;
static int frame_owidth2 = 80;
static int frame_oheight2 = 60;

static char flirone_pro = 0;
static char pal_inverse = 0;
static char pal_colors = 0;

static int FFC = 0;    // detect FFC

static int fdwr1 = 0;
static int fdwr2 = 0;
static struct libusb_device_handle *devh = NULL;
static unsigned buf85pointer = 0;
static unsigned char buf85[BUF85SIZE];

/* functions */

void print_format(struct v4l2_format*vid_format)
{
    printf("     vid_format->type                =%d\n",     vid_format->type );
    printf("     vid_format->fmt.pix.width       =%d\n",     vid_format->fmt.pix.width );
    printf("     vid_format->fmt.pix.height      =%d\n",     vid_format->fmt.pix.height );
    printf("     vid_format->fmt.pix.pixelformat =%d\n",     vid_format->fmt.pix.pixelformat);
    printf("     vid_format->fmt.pix.sizeimage   =%u\n",     vid_format->fmt.pix.sizeimage );
    printf("     vid_format->fmt.pix.field       =%d\n",     vid_format->fmt.pix.field );
    printf("     vid_format->fmt.pix.bytesperline=%d\n",     vid_format->fmt.pix.bytesperline );
    printf("     vid_format->fmt.pix.colorspace  =%d\n",     vid_format->fmt.pix.colorspace );
}

void font_write(unsigned char *fb, int x, int y, const char *string,
    unsigned char color)
{
    int rx, ry, pos;

    while (*string) {
        for (ry = 0; ry < 7; ry++) {
            for (rx = 0; rx < 5; rx++) {
                int v = (font5x7_basic[(*string & 0x7F) - CHAR_OFFSET][rx] >> (ry)) & 1;
                pos = (y + ry) * FRAME_WIDTH2 + (x + rx);
                fb[pos] = v ? color : fb[pos];  // transparent
            }
        }
        string++;
        x += 6;
    }
}

static double raw2temperature(unsigned short RAW)
{
    // mystery correction factor
    RAW *= 4;
    // calc amount of radiance of reflected objects ( Emissivity < 1 )
    double RAWrefl=PlanckR1/(PlanckR2*(exp(PlanckB/(TempReflected+273.15))-PlanckF))-PlanckO;
    // get displayed object temp max/min
    double RAWobj=(RAW-(1-Emissivity)*RAWrefl)/Emissivity;
    // calc object temperature
    return PlanckB/log(PlanckR1/(PlanckR2*(RAWobj+PlanckO))+PlanckF)-273.15;
}

static void startv4l2()
{
    int ret_code = 0;
    struct v4l2_capability vid_caps1 = {}, vid_caps2 = {};
    struct v4l2_format vid_format1 = {}, vid_format2 = {};
    size_t linewidth1 = 0, framesize1 = 0;
    size_t linewidth2 = 0, framesize2 = 0;

    //open video_device1
    printf("using output device: %s\n", video_device1);

    fdwr1 = open(video_device1, O_RDWR);
    assert(fdwr1 >= 0);

    ret_code = ioctl(fdwr1, VIDIOC_QUERYCAP, &vid_caps1);
    assert(ret_code != -1);

    memset(&vid_format1, 0, sizeof(vid_format1));

    ret_code = ioctl(fdwr1, VIDIOC_G_FMT, &vid_format1);

    linewidth1 = FRAME_WIDTH1;
    framesize1 = FRAME_WIDTH1 * FRAME_HEIGHT1;

    vid_format1.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    vid_format1.fmt.pix.width = FRAME_WIDTH1;
    vid_format1.fmt.pix.height = FRAME_HEIGHT1;
    vid_format1.fmt.pix.pixelformat = FRAME_FORMAT1;
    vid_format1.fmt.pix.sizeimage = framesize1;
    vid_format1.fmt.pix.field = V4L2_FIELD_NONE;
    vid_format1.fmt.pix.bytesperline = linewidth1;
    vid_format1.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;

    // set data format
    ret_code = ioctl(fdwr1, VIDIOC_S_FMT, &vid_format1);
    assert(ret_code != -1);

    print_format(&vid_format1);

    //open video_device2
    printf("using output device: %s\n", video_device2);

    fdwr2 = open(video_device2, O_RDWR);
    assert(fdwr2 >= 0);

    ret_code = ioctl(fdwr2, VIDIOC_QUERYCAP, &vid_caps2);
    assert(ret_code != -1);

    memset(&vid_format2, 0, sizeof(vid_format2));

    ret_code = ioctl(fdwr2, VIDIOC_G_FMT, &vid_format2);

    linewidth2 = FRAME_WIDTH2;
    framesize2 = FRAME_WIDTH2 * FRAME_HEIGHT2 * 3; // 8x8x8 Bit

    vid_format2.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    vid_format2.fmt.pix.width = FRAME_WIDTH2;
    vid_format2.fmt.pix.height = FRAME_HEIGHT2;
    vid_format2.fmt.pix.pixelformat = FRAME_FORMAT2;
    vid_format2.fmt.pix.sizeimage = framesize2;
    vid_format2.fmt.pix.field = V4L2_FIELD_NONE;
    vid_format2.fmt.pix.bytesperline = linewidth2;
    vid_format2.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;

    // set data format
    ret_code = ioctl(fdwr2, VIDIOC_S_FMT, &vid_format2);
    assert(ret_code != -1);

    print_format(&vid_format2);
}

static void vframe(char ep[], char EP_error[], int r, int actual_length,
    unsigned char buf[], unsigned char *colormap)
{
    time_t now1;
    char magicbyte[4] = { 0xEF, 0xBE, 0x00, 0x00 };
    uint32_t FrameSize, ThermalSize, JpgSize;
    int v, x, y, pos, disp;
    unsigned short pix[FRAME_OWIDTH2 * FRAME_OHEIGHT2];   // original Flir 16 Bit RAW
    unsigned char *fb_proc, *fb_proc2;
    size_t framesize2 = FRAME_WIDTH2 * FRAME_HEIGHT2 * 3; // 8x8x8 Bit
    int min = 0x10000, max = 0;
    int maxx = -1, maxy = -1;
    int delta, scale, med;
    int hw, hh;
    char st1[100];
    char st2[100];
    struct tm *loctime;

    now1 = time(NULL);
    if (r < 0) {
        if (strcmp(EP_error, libusb_error_name(r)) != 0) {
            strcpy(EP_error, libusb_error_name(r));
            fprintf(stderr, "\n: %s >>>>>>>>>>>>>>>>>bulk transfer (in) %s:%i %s\n",
                ctime(&now1), ep, r, libusb_error_name(r));
            sleep(1);
        }
        return;
    }

    // reset buffer if the new chunk begins with magic bytes or the buffer size limit is exceeded
    if (strncmp((char *)buf, magicbyte, 4) == 0 ||
            buf85pointer + actual_length >= BUF85SIZE)
        buf85pointer = 0;

    memmove(buf85 + buf85pointer, buf, actual_length);
    buf85pointer += actual_length;

    if (strncmp((char *)buf85, magicbyte, 4) != 0) {
        //reset buff pointer
        buf85pointer = 0;
        printf("Reset buffer because of bad Magic Byte!\n");
        return;
    }

    // a quick and dirty job for gcc
    FrameSize   = buf85[ 8] + (buf85[ 9] << 8) + (buf85[10] << 16) + (buf85[11] << 24);
    ThermalSize = buf85[12] + (buf85[13] << 8) + (buf85[14] << 16) + (buf85[15] << 24);
    JpgSize     = buf85[16] + (buf85[17] << 8) + (buf85[18] << 16) + (buf85[19] << 24);

    if (FrameSize + 28 > buf85pointer)
        // wait for next chunk
        return;

    /*
    printf("actual_len=%d, buf85pointer=%d, FrameSize=%d, ThermalSize=%d, JpgSize=%d\n",
        actual_length, buf85pointer, FrameSize, ThermalSize, JpgSize);
     */

    // get a full frame, first print the status
    buf85pointer = 0;

    fb_proc = malloc(FRAME_WIDTH2 * FRAME_HEIGHT2);
    memset(fb_proc, 128, FRAME_WIDTH2 * FRAME_HEIGHT2);
    assert(fb_proc);

    fb_proc2 = malloc(FRAME_WIDTH2 * FRAME_HEIGHT2 * 3); // 8x8x8  Bit RGB buffer
    assert(fb_proc2);

    if (pal_colors) {
        for (y = 0; y < FRAME_HEIGHT2; ++y)
            for (x = 0; x < FRAME_WIDTH2; ++x)
                for (disp = 0; disp < 3; disp++)
                    fb_proc2[3 * y * FRAME_WIDTH2 + 3 * x + disp] =
                        colormap[3 * (y * 256 / FRAME_HEIGHT2) + disp];
        goto render;
    }

    // Make a unsigned short array from what comes from the thermal frame
    // find the max and min values of the array

    hw = FRAME_OWIDTH2 / 2;
    hh = FRAME_OHEIGHT2 / 2;

    for (y = 0; y < FRAME_OHEIGHT2; y++) {
        for (x = 0; x < FRAME_OWIDTH2; x++) {
            if (flirone_pro) {
                pos = 2 * (y * (FRAME_OWIDTH2 + 4) + x) + 32;
                if (x > hw)
                    pos += 4;
            } else {
                /*
                 * 32 - seems to be the header size
                 * +2 - for some reason 2 16-bit values must be skipped at the end
                 *      of each line
                 */
                pos = 2 * (y * (FRAME_OWIDTH2 + 2) + x) + 32;
            }

            v = buf85[pos] | buf85[pos + 1] << 8;
            pix[y * FRAME_OWIDTH2 + x] = v;

            if (v < min)
                min = v;
            if (v > max) {
                max = v;
                maxx = x;
                maxy = y;
            }
        }
    }

    assert(maxx != -1);
    assert(maxy != -1);
    /* printf("min=%d max=%d x=%d y=%d\n", min, max, maxx, maxy); */

    // scale the data in the array
    delta = max - min;
    if (delta == 0)
        delta = 1;   // if max = min we have divide by zero
    scale = 0x10000 / delta;

    for (y = 0; y < FRAME_OHEIGHT2; y++) {
        for (x = 0; x < FRAME_OWIDTH2; x++) {
          int v = (pix[y * FRAME_OWIDTH2 + x] - min) * scale >> 8;

          // fb_proc is the gray scale frame buffer
          fb_proc[y * FRAME_OWIDTH2 + x] = v;   // unsigned char!!
        }
    }

    // calc medium of 2x2 center pixels
    med = pix[(hh - 1) * FRAME_OWIDTH2 + hw - 1] +
          pix[(hh - 1) * FRAME_OWIDTH2 + hw] +
          pix[hh * FRAME_OWIDTH2 + hw - 1] +
          pix[hh * FRAME_OWIDTH2 + hw];
    med /= 4;

    // Print temperatures and time
    loctime = localtime (&now1);

    sprintf(st1,"'C %.1f/%.1f/",
        raw2temperature(min), raw2temperature(med));
    sprintf(st2, "%.1f ", raw2temperature(max));
    strftime(&st2[strlen(st2)], 60, "%H:%M:%S", loctime);

    if (flirone_pro) {
        strcat(st1, st2);
        st1[MAX_CHARS2 - 1] = 0;
        font_write(fb_proc, 1, FRAME_OHEIGHT2, st1, FONT_COLOR_DFLT);
    } else {
        // Print in 2 lines for FLIR ONE G3
        st1[MAX_CHARS2 - 1] = 0;
        st2[MAX_CHARS2 - 1] = 0;
        font_write(fb_proc, 1, FRAME_OHEIGHT2 + 2, st1, FONT_COLOR_DFLT);
        font_write(fb_proc, 1, FRAME_OHEIGHT2 + 12, st2, FONT_COLOR_DFLT);
    }

    // show crosshairs, remove if required
    font_write(fb_proc, hw - 2, hh - 3, "+", FONT_COLOR_DFLT);

    maxx -= 4;
    maxy -= 4;

    if (maxx < 0)
        maxx = 0;
    if (maxy < 0)
        maxy = 0;
    if (maxx > FRAME_OWIDTH2 - 10)
        maxx = FRAME_OWIDTH2 - 10;
    if (maxy > FRAME_OHEIGHT2 - 10)
        maxy = FRAME_OHEIGHT2 - 10;

    font_write(fb_proc, FRAME_OWIDTH2 - 6, maxy, "<", FONT_COLOR_DFLT);
    font_write(fb_proc, maxx, FRAME_OHEIGHT2 - 8, "|", FONT_COLOR_DFLT);

    // build RGB image
    for (y = 0; y < FRAME_HEIGHT2; y++) {
        for (x = 0; x < FRAME_WIDTH2; x++) {
            // fb_proc is the gray scale frame buffer
            v = fb_proc[y * FRAME_OWIDTH2 + x];
            if (pal_inverse)
                v = 255 - v;

            for (disp = 0; disp < 3; disp++)
                // fb_proc2 is a 24bit RGB buffer
                fb_proc2[3 * y * FRAME_OWIDTH2 + 3 * x + disp] =
                    colormap[3 * v + disp];
        }
    }

render:
    // jpg Visual Image
    if (write(fdwr1, &buf85[28 + ThermalSize], JpgSize) != JpgSize) {
        perror("write visual image failed");
        exit(1);
    }

    if (strncmp((char *)&buf85[28 + ThermalSize + JpgSize + 17], "FFC", 3) == 0) {
        printf("drop FFC frame\n");
        FFC = 1;    // drop all FFC frames
    } else {
        if (FFC == 1) {
            printf("drop first frame after FFC\n");
            FFC = 0;  // drop first frame after FFC
        } else {
            // colorized RGB Thermal Image
            if (write(fdwr2, fb_proc2, framesize2) != (ssize_t)framesize2) {
                perror("write thermal image failed");
                exit(1);
            }
        }
    }

    // free memory
    free(fb_proc);      // thermal RAW
    free(fb_proc2);     // visible jpg
}

static int find_lvr_flirusb(void)
{
    devh = libusb_open_device_with_vid_pid(NULL, VENDOR_ID, PRODUCT_ID);
    return devh ? 0 : -EIO;
}

static void usb_exit(void)
{
    //close the device
    libusb_reset_device(devh);
    libusb_close(devh);
    libusb_exit(NULL);
}

static int usb_init(void)
{
    int r;

    r = libusb_init(NULL);
    if (r < 0) {
        fprintf(stderr, "failed to initialise libusb\n");
        exit(1);
    }

    r = find_lvr_flirusb();
    if (r < 0) {
        fprintf(stderr, "Could not find/open device\n");
        goto out;
    }
    printf("Successfully find the Flir One G2/G3/Pro device\n");

    r = libusb_set_configuration(devh, 3);
    if (r < 0) {
       fprintf(stderr, "libusb_set_configuration error %d\n", r);
       goto out;
    }
   printf("Successfully set usb configuration 3\n");

    // Claiming of interfaces is a purely logical operation;
    // it does not cause any requests to be sent over the bus.
    r = libusb_claim_interface(devh, 0);
    if (r < 0) {
        fprintf(stderr, "libusb_claim_interface 0 error %d\n", r);
        goto out;
    }
    r = libusb_claim_interface(devh, 1);
    if (r < 0) {
        fprintf(stderr, "libusb_claim_interface 1 error %d\n", r);
        goto out;
    }
    r = libusb_claim_interface(devh, 2);
    if (r < 0) {
        fprintf(stderr, "libusb_claim_interface 2 error %d\n", r);
        goto out;
    }
    printf("Successfully claimed interface 0, 1, 2\n");
    return 0;

out:
    usb_exit();
    return -1;
}

static int EPloop(unsigned char *colormap)
{
    int r = 0;
    unsigned char buf[BUF85SIZE];
    int actual_length;
    time_t now;
    char EP85_error[50] = "";
    unsigned char data[2] = { 0, 0 }; // only a bad dummy
    int state, timeout;

    if (usb_init() < 0)
        return -1;

    // save last error status to avoid clutter the log
    startv4l2();

    state = 1;

    // don't change timeout=100ms !!
    timeout = 100;
    while (1) {
        switch(state) {
        case 1:
            printf("stop interface 2 FRAME\n");
            r = libusb_control_transfer(devh, REQ_TYPE, REQ, V_STOP, INDEX(2),
                    data, LEN(0), timeout);
            if (r < 0) {
                fprintf(stderr, "Control Out error %d\n", r);
                return r;
            }

            printf("stop interface 1 FILEIO\n");
            r = libusb_control_transfer(devh, REQ_TYPE, REQ, V_STOP, INDEX(1),
                    data, LEN(0), timeout);
            if (r < 0) {
                fprintf(stderr, "Control Out error %d\n", r);
                return r;
            }

            printf("\nstart interface 1 FILEIO\n");
            r = libusb_control_transfer(devh, REQ_TYPE, REQ, V_START,
                    INDEX(1), data, LEN(0), timeout);
            if (r < 0) {
                fprintf(stderr, "Control Out error %d\n", r);
                return r;
            }
            now = time(0);  // Get the system time
            printf("\n:xx %s", ctime(&now));
            state = 2;
            break;

        case 2:
            printf("\nAsk for video stream, start EP 0x85:\n");
            r = libusb_control_transfer(devh, REQ_TYPE, REQ, V_START,
                    INDEX(2), data, LEN(2), timeout * 2);
            if (r < 0) {
                fprintf(stderr, "Control Out error %d\n", r);
                return r;
            }

            state = 3;
            break;

        case 3:
            // endless loop
            // poll Frame Endpoints 0x85
            r = libusb_bulk_transfer(devh, 0x85, buf, sizeof(buf),
                    &actual_length, timeout);
            if (actual_length > 0)
                vframe("0x85", EP85_error, r, actual_length, buf, colormap);
            break;
        }

        // poll Endpoints 0x81, 0x83
        r = libusb_bulk_transfer(devh, 0x81, buf, sizeof(buf), &actual_length, 10);
        r = libusb_bulk_transfer(devh, 0x83, buf, sizeof(buf), &actual_length, 10);
        if (strcmp(libusb_error_name(r), "LIBUSB_ERROR_NO_DEVICE")==0) {
            fprintf(stderr, "EP 0x83 LIBUSB_ERROR_NO_DEVICE -> reset USB\n");
            goto out;
        }
    }

    // never reached ;-)
    libusb_release_interface(devh, 0);

out:
    usb_exit();
    return r >= 0 ? r : -r;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage:\n"
        "\n"
        "./flirone [option]* palletes/<palette.raw>\n"
        "\n"
        "Options:\n"
        "\t-i\tUse inverse pallete colors\n"
        "\t-p\tShow pallete colors instead of sensor data\n"
        "\t--pro\tSelect FLIR ONE PRO camera (default is FLIR ONE G3)\n"
        "\t-v <n>\tUse /dev/video<n> and /dev/video<n+1> devices (default is n=1)\n");
    exit(1);
}

int main(int argc, char **argv)
{
    int i;
    long n;
    const char *arg, *palpath = NULL;
    unsigned char colormap[768];
    char *endptr;
    FILE *fp;

    /* parse args */
    if (argc < 2)
        usage();

    n = 1;
    for (i = 1; i < argc; i++) {
        arg = argv[i];

        if (strcmp(arg, "--pro") == 0) {
            flirone_pro = 1;
            frame_width2 = 160;
            frame_height2 = 128;
            frame_owidth2 = 160;
            frame_oheight2 = 120;

        } else if (strcmp(arg, "-v") == 0) {
            arg = argv[++i];
            n = strtol(arg, &endptr, 10);
            if (n < 0 || n > 65535 || *endptr != 0)
                usage();

        } else if (strcmp(arg, "-i") == 0) {
            pal_inverse = 1;

        } else if (strcmp(arg, "-p") == 0) {
            pal_colors = 1;

        } else {
            if (palpath)
                usage();
            palpath = arg;
        }
    }
    if (palpath == NULL)
        usage();

    sprintf(video_device1, "/dev/video%ld", n);
    sprintf(video_device2, "/dev/video%ld", n + 1);

    fp = fopen(palpath, "rb");
    // read 256 rgb values
    if (fread(colormap, 1, 768, fp) != 768) {
        perror("failed to read colormap");
        exit(1);
    }
    fclose(fp);

    while (1)
        EPloop(colormap);
}
