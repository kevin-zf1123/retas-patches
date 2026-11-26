#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifdef RETAS_RENDER_ALLOCATOR
int retas_reserve_codec(size_t bytes);
void retas_release_codec(size_t bytes);
#endif
struct retas_image_buffer {unsigned char* data;size_t size;int width,height;double dpi;char error[256];size_t budget_bytes;int budget_rejected;};
int retas_read_png(const unsigned char*,size_t,struct retas_image_buffer*);
int retas_read_jpeg(const unsigned char*,size_t,struct retas_image_buffer*);
int retas_write_jpeg(const unsigned char*,int,int,int,struct retas_image_buffer*);
#ifdef __cplusplus
}
#endif
