/*
 * The part of libva 2.x's ABI that pm_hw.c uses, declared here because libva is looked up at run time (dlopen of
 * libva.so.2) and the release build (manylinux2014) has no libva 2 headers. CI checks every size and offset against
 * the real <va/va.h> (tests/fakehw/pm_va_layout_check.c).
 */
#ifndef PM_HW_VA_H
#define PM_HW_VA_H
#include <stdint.h>

typedef void *pmVADisplay;
typedef int pmVAStatus;
typedef unsigned int pmVAGenericID;

typedef struct {
    uint32_t fourcc, byte_order, bits_per_pixel, depth, red_mask, green_mask, blue_mask, alpha_mask;
    uint32_t va_reserved[4];
} pmVAImageFormat;

typedef struct {
    pmVAGenericID image_id;
    pmVAImageFormat format;
    pmVAGenericID buf;
    uint16_t width, height;
    uint32_t data_size, num_planes, pitches[3], offsets[3];
    int32_t num_palette_entries, entry_bytes;
    int8_t component_order[4];
    uint32_t va_reserved[4];
} pmVAImage;

#define PM_VA_RT_FORMAT_YUV420 0x00000001
#define PM_VA_RT_FORMAT_YUV420_10 0x00000100
#define PM_VA_FOURCC_NV12 0x3231564E
#define PM_VA_FOURCC_P010 0x30313050
#define PM_VA_INVALID_ID 0xffffffffu
/* va_drmcommon.h: VADRMPRIMESurfaceDescriptor (vaExportSurfaceHandle, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) */
typedef struct {
    uint32_t fourcc, width, height, num_objects;
    struct { int fd; uint32_t size; uint64_t drm_format_modifier; } objects[4];
    uint32_t num_layers;
    struct { uint32_t drm_format, num_planes, object_index[4], offset[4], pitch[4]; } layers[4];
} pmVADRMPRIMESurfaceDescriptor;
#define PM_VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 0x40000000
#define PM_VA_EXPORT_SURFACE_READ_WRITE 0x0003
#define PM_VA_EXPORT_SURFACE_SEPARATE_LAYERS 0x0004

/* libva's VADisplayContext starts with this magic (va_backend.h: VA_DISPLAY_MAGIC) */
#define PM_VA_DISPLAY_MAGIC 0x56414430

#endif
