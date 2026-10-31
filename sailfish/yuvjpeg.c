/* yuvjpeg.c -- a JPEG from a planar 4:2:0 YUV frame, with libjpeg(-turbo).
 * The Y, U and V planes go to libjpeg as they are (raw data in): no
 * conversion back to RGB, and no second loss of colour resolution. */
#include "yuvjpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <jpeglib.h>

typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; } err_t;
static void on_error(j_common_ptr c) { longjmp(((err_t *)c->err)->jb, 1); }

int yuv_encode_jpeg(const ir_yuv_t *f, int quality, uint8_t **jpg, unsigned long *len)
{
    struct jpeg_compress_struct c;
    err_t err;
    JSAMPROW rows_y[16], rows_u[8], rows_v[8];
    JSAMPARRAY planes[3] = { rows_y, rows_u, rows_v };
    unsigned char *mem = NULL;
    unsigned long memlen = 0;
    int i;
    if (!f || !f->y || !f->u || !f->v || f->w < 16 || f->h < 16 || (f->w & 1) || (f->h & 1) || !jpg || !len) return -1;
    *jpg = NULL; *len = 0;
    c.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_error;
    if (setjmp(err.jb)) { jpeg_destroy_compress(&c); free(mem); return -2; }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &mem, &memlen);
    c.image_width = (JDIMENSION)f->w;
    c.image_height = (JDIMENSION)f->h;
    c.input_components = 3;
    c.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&c);
    jpeg_set_colorspace(&c, JCS_YCbCr);
    c.raw_data_in = TRUE;
    c.dct_method = JDCT_ISLOW;
    c.comp_info[0].h_samp_factor = 2; c.comp_info[0].v_samp_factor = 2;   /* 4:2:0, as the planes */
    c.comp_info[1].h_samp_factor = 1; c.comp_info[1].v_samp_factor = 1;
    c.comp_info[2].h_samp_factor = 1; c.comp_info[2].v_samp_factor = 1;
    jpeg_set_quality(&c, quality, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {         /* 16 luma rows, 8 chroma rows */
        JDIMENSION y0 = c.next_scanline;
        for (i = 0; i < 16; ++i) {
            JDIMENSION y = y0 + i < c.image_height ? y0 + i : c.image_height - 1;   /* repeat the last row */
            rows_y[i] = (JSAMPROW)(f->y + (size_t)y * f->w);
        }
        for (i = 0; i < 8; ++i) {
            JDIMENSION y = y0 / 2 + i < c.image_height / 2 ? y0 / 2 + i : c.image_height / 2 - 1;
            rows_u[i] = (JSAMPROW)(f->u + (size_t)y * (f->w / 2));
            rows_v[i] = (JSAMPROW)(f->v + (size_t)y * (f->w / 2));
        }
        jpeg_write_raw_data(&c, planes, 16);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    *jpg = mem; *len = memlen;
    return 0;
}
