/* Compile-only check (CI): pm_hw_va.h's declarations match the real libva 2.x headers. */
#include <stddef.h>
#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_drmcommon.h>
#include "../../src/pm_hw_va.h"
#define SAME(a, b) _Static_assert(sizeof(a) == sizeof(b), #a " size")
#define OFF(a, b, m) _Static_assert(offsetof(a, m) == offsetof(b, m), #a "." #m)
SAME(pmVAImageFormat, VAImageFormat);
OFF(pmVAImageFormat, VAImageFormat, fourcc); OFF(pmVAImageFormat, VAImageFormat, bits_per_pixel);
SAME(pmVAImage, VAImage);
OFF(pmVAImage, VAImage, image_id); OFF(pmVAImage, VAImage, format); OFF(pmVAImage, VAImage, buf);
OFF(pmVAImage, VAImage, width); OFF(pmVAImage, VAImage, height); OFF(pmVAImage, VAImage, data_size);
OFF(pmVAImage, VAImage, num_planes); OFF(pmVAImage, VAImage, pitches); OFF(pmVAImage, VAImage, offsets);
SAME(pmVAGenericID, VASurfaceID);
SAME(pmVAStatus, VAStatus);
_Static_assert(PM_VA_RT_FORMAT_YUV420 == VA_RT_FORMAT_YUV420, "rt420");
_Static_assert(PM_VA_RT_FORMAT_YUV420_10 == VA_RT_FORMAT_YUV420_10, "rt420_10");
_Static_assert(PM_VA_FOURCC_NV12 == VA_FOURCC_NV12, "nv12");
_Static_assert(PM_VA_FOURCC_P010 == VA_FOURCC_P010, "p010");
_Static_assert(PM_VA_INVALID_ID == VA_INVALID_ID, "invalid id");
_Static_assert(offsetof(struct VADisplayContext, vadpy_magic) == 0, "the display magic comes first");
#ifdef VA_DISPLAY_MAGIC
_Static_assert(PM_VA_DISPLAY_MAGIC == VA_DISPLAY_MAGIC, "display magic");
#endif
SAME(pmVADRMPRIMESurfaceDescriptor, VADRMPRIMESurfaceDescriptor);
OFF(pmVADRMPRIMESurfaceDescriptor, VADRMPRIMESurfaceDescriptor, num_objects); OFF(pmVADRMPRIMESurfaceDescriptor, VADRMPRIMESurfaceDescriptor, objects);
OFF(pmVADRMPRIMESurfaceDescriptor, VADRMPRIMESurfaceDescriptor, num_layers); OFF(pmVADRMPRIMESurfaceDescriptor, VADRMPRIMESurfaceDescriptor, layers);
_Static_assert(PM_VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, "prime2");
_Static_assert(PM_VA_EXPORT_SURFACE_READ_WRITE == VA_EXPORT_SURFACE_READ_WRITE, "rw");
_Static_assert(PM_VA_EXPORT_SURFACE_SEPARATE_LAYERS == VA_EXPORT_SURFACE_SEPARATE_LAYERS, "separate");
int main(void) { return 0; }
