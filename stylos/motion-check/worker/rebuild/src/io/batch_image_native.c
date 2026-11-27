#include "batch_image_native.h"
#include <png.h>
#include <stdio.h>
#include <jpeglib.h>
_Static_assert(BITS_IN_JSAMPLE==8,"Batch JPEG requires8-bit JSAMPLE");
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdint.h>
#ifdef _MSC_VER
__declspec(align(16)) union png_allocation {size_t size;};
#else
union png_allocation {max_align_t alignment;size_t size;};
#endif
static int reserve_buffer(struct retas_image_buffer* out,size_t bytes){
#ifdef RETAS_RENDER_ALLOCATOR
    if(!retas_reserve_codec(bytes)){out->budget_rejected=1;return 0;}
    out->budget_bytes+=bytes;
#else
    (void)out;(void)bytes;
#endif
    return 1;
}
struct png_memory {size_t used;};
static png_voidp bounded_png_malloc(png_structp png,png_alloc_size_t size){struct png_memory* memory=png_get_mem_ptr(png);if(size>32u*1024*1024-memory->used||size>SIZE_MAX-sizeof(union png_allocation))return 0;union png_allocation* allocation=malloc(sizeof(*allocation)+(size_t)size);if(!allocation)return 0;allocation->size=(size_t)size;memory->used+=(size_t)size;return allocation+1;}
static void bounded_png_free(png_structp png,png_voidp pointer){if(pointer){struct png_memory* memory=png_get_mem_ptr(png);union png_allocation* allocation=(union png_allocation*)pointer-1;memory->used-=allocation->size;free(allocation);}}
struct png_input {const unsigned char* data;size_t size,at;};
static void png_input_read(png_structp png,png_bytep out,png_size_t n){struct png_input* p=png_get_io_ptr(png);if(n>p->size-p->at)png_error(png,"Truncated PNG");memcpy(out,p->data+p->at,n);p->at+=n;}
static void png_failure(png_structp png,png_const_charp message){struct retas_image_buffer* out=png_get_error_ptr(png);snprintf(out->error,sizeof(out->error),"%s",message);longjmp(png_jmpbuf(png),1);}
static void png_warning_ignore(png_structp png,png_const_charp message){(void)png;(void)message;}
int retas_read_png(const unsigned char* bytes,size_t size,struct retas_image_buffer* out){
    if(!reserve_buffer(out,32u*1024*1024))return 0;
    struct png_memory* memory=calloc(1,sizeof(*memory));if(!memory)return 0;png_structp png=png_create_read_struct_2(PNG_LIBPNG_VER_STRING,out,png_failure,png_warning_ignore,memory,bounded_png_malloc,bounded_png_free);if(!png){free(memory);return 0;}png_infop info=png_create_info_struct(png);if(!info){png_destroy_read_struct(&png,0,0);free(memory);return 0;}
    if(setjmp(png_jmpbuf(png))){free(out->data);out->data=0;png_destroy_read_struct(&png,&info,0);free(memory);return 0;}
    struct png_input input={bytes,size,0};png_set_read_fn(png,&input,png_input_read);png_set_user_limits(png,100000,100000);png_set_chunk_cache_max(png,128);png_set_chunk_malloc_max(png,8u*1024*1024);png_read_info(png,info);
    const png_uint_32 w=png_get_image_width(png,info),h=png_get_image_height(png,info);const int depth=png_get_bit_depth(png,info),type=png_get_color_type(png,info);
    if(!w||!h||(unsigned long long)w*h>64ull*1024*1024)png_error(png,"PNG exceeds64Mi pixels");
    png_uint_32 xppm,yppm;int unit;out->dpi=144;if(png_get_pHYs(png,info,&xppm,&yppm,&unit)&&unit==PNG_RESOLUTION_METER&&xppm)out->dpi=xppm*.0254;
    if(depth==16)png_set_strip_16(png);if(type==PNG_COLOR_TYPE_PALETTE)png_set_palette_to_rgb(png);if(type==PNG_COLOR_TYPE_GRAY&&depth<8)png_set_expand_gray_1_2_4_to_8(png);
    const int transparent=png_get_valid(png,info,PNG_INFO_tRNS)!=0;if(transparent)png_set_tRNS_to_alpha(png);if(type==PNG_COLOR_TYPE_GRAY||type==PNG_COLOR_TYPE_GRAY_ALPHA)png_set_gray_to_rgb(png);if(!(type&PNG_COLOR_MASK_ALPHA)&&!transparent)png_set_add_alpha(png,255,PNG_FILLER_AFTER);
    const int passes=png_set_interlace_handling(png);png_read_update_info(png,info);if(png_get_rowbytes(png,info)!=(size_t)w*4)png_error(png,"Invalid PNG row size");
    out->size=(size_t)w*h*4;if(!reserve_buffer(out,out->size))png_error(png,"Renderer memory limit");out->data=calloc(1,out->size);if(!out->data)png_error(png,"PNG allocation failed");out->width=(int)w;out->height=(int)h;
    for(int p=0;p<passes;++p)for(png_uint_32 y=0;y<h;++y)png_read_row(png,out->data+(size_t)y*w*4,0);png_read_end(png,info);png_destroy_read_struct(&png,&info,0);free(memory);return 1;
}
struct jpeg_error {struct jpeg_error_mgr base;jmp_buf jump;struct retas_image_buffer* out;};
static void jpeg_failure(j_common_ptr codec){struct jpeg_error* error=(struct jpeg_error*)codec->err;(*codec->err->format_message)(codec,error->out->error);longjmp(error->jump,1);}
int retas_read_jpeg(const unsigned char* bytes,size_t size,struct retas_image_buffer* out){
    struct jpeg_decompress_struct* codec=calloc(1,sizeof(*codec));struct jpeg_error* error=calloc(1,sizeof(*error));if(!codec||!error){free(codec);free(error);return 0;}codec->err=jpeg_std_error(&error->base);error->base.error_exit=jpeg_failure;error->out=out;
    if(setjmp(error->jump)){free(out->data);out->data=0;jpeg_destroy_decompress(codec);free(codec);free(error);return 0;}
    jpeg_create_decompress(codec);codec->mem->max_memory_to_use=32L*1024*1024;jpeg_mem_src(codec,bytes,(unsigned long)size);jpeg_read_header(codec,TRUE);codec->out_color_space=JCS_RGB;
    if(!codec->image_width||!codec->image_height||(unsigned long long)codec->image_width*codec->image_height>64ull*1024*1024){snprintf(out->error,sizeof(out->error),"JPEG exceeds64Mi pixels");jpeg_destroy_decompress(codec);free(codec);free(error);return 0;}
    out->dpi=144;if(codec->density_unit==1&&codec->X_density)out->dpi=codec->X_density;else if(codec->density_unit==2&&codec->X_density)out->dpi=codec->X_density*2.54;
    // Charge decoded output plus coefficient/row storage before libjpeg allocates it.
    if(!reserve_buffer(out,(size_t)codec->image_width*codec->image_height*(3u+2u*codec->num_components)+1048576)){
        jpeg_destroy_decompress(codec);free(codec);free(error);return 0;
    }
    jpeg_start_decompress(codec);out->width=(int)codec->output_width;out->height=(int)codec->output_height;out->size=(size_t)out->width*out->height*3;out->data=malloc(out->size);
    if(!out->data){jpeg_destroy_decompress(codec);free(codec);free(error);return 0;}
    while(codec->output_scanline<codec->output_height){JSAMPROW row=out->data+(size_t)codec->output_scanline*out->width*3;jpeg_read_scanlines(codec,&row,1);}jpeg_finish_decompress(codec);jpeg_destroy_decompress(codec);free(codec);free(error);return 1;
}
int retas_write_jpeg(const unsigned char* rgb,int width,int height,int quality,struct retas_image_buffer* out){
    struct jpeg_compress_struct* codec=calloc(1,sizeof(*codec));struct jpeg_error* error=calloc(1,sizeof(*error));unsigned long size=0;if(!codec||!error){free(codec);free(error);return 0;}codec->err=jpeg_std_error(&error->base);error->base.error_exit=jpeg_failure;error->out=out;
    if(setjmp(error->jump)){free(out->data);out->data=0;jpeg_destroy_compress(codec);free(codec);free(error);return 0;}
    jpeg_create_compress(codec);jpeg_mem_dest(codec,&out->data,&size);codec->image_width=width;codec->image_height=height;codec->input_components=3;codec->in_color_space=JCS_RGB;jpeg_set_defaults(codec);jpeg_set_quality(codec,quality,FALSE);jpeg_start_compress(codec,TRUE);
    while(codec->next_scanline<codec->image_height){JSAMPROW row=(JSAMPROW)(rgb+(size_t)codec->next_scanline*width*3);jpeg_write_scanlines(codec,&row,1);}jpeg_finish_compress(codec);out->size=size;jpeg_destroy_compress(codec);free(codec);free(error);return 1;
}
