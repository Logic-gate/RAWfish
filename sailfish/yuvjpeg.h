/* yuvjpeg.h -- compress a planar 4:2:0 YUV frame (the engine's) to a JPEG. */
#ifndef INDRAW_YUVJPEG_H
#define INDRAW_YUVJPEG_H
#include "../android/yuvframe.h"
/* *jpg is malloc'd (free it); 0 on success. */
int yuv_encode_jpeg(const ir_yuv_t *f, int quality, uint8_t **jpg, unsigned long *len);
#endif
