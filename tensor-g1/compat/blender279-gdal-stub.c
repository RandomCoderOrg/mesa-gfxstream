/*
 * Minimal GDAL ABI stub for Debian Buster's OpenCV 3.2 image-codec library.
 *
 * Blender 2.79 itself does not use GDAL.  Debian's OpenImageIO dependency
 * pulls in OpenCV, whose image-codec library eagerly resolves these GDAL
 * symbols even when no geospatial image is opened.  Shipping Buster's real
 * libgdal.so.20 would otherwise require a large, obsolete dependency graph.
 *
 * This stub deliberately makes GDAL image opening fail.  It is suitable only
 * for the isolated Blender 2.79 compatibility bundle; do not install it as a
 * system library.
 */

#include <stddef.h>

enum {
   GDAL_CE_FAILURE = 3,
   GDAL_GDT_UNKNOWN = 0,
   GDAL_GPI_GRAY = 0,
};

void
GDALAllRegister(void)
{
}

void *
GDALOpen(const char *path, int access)
{
   (void)path;
   (void)access;
   return NULL;
}

void
GDALClose(void *dataset)
{
   (void)dataset;
}

const char *
GDALGetDataTypeName(int type)
{
   (void)type;
   return "Unknown";
}

#define STUB_INT(name, value) \
   int name(void *self) __asm__(#name); \
   int name(void *self) { (void)self; return value; }

#define STUB_PTR_INT(name) \
   void *name(void *self, int index) __asm__(#name); \
   void *name(void *self, int index) \
   { \
      (void)self; \
      (void)index; \
      return NULL; \
   }

STUB_PTR_INT(_ZN11GDALDataset13GetRasterBandEi)
STUB_INT(_ZN11GDALDataset14GetRasterCountEv, 0)
STUB_INT(_ZN11GDALDataset14GetRasterXSizeEv, 0)
STUB_INT(_ZN11GDALDataset14GetRasterYSizeEv, 0)
STUB_INT(_ZN14GDALRasterBand17GetRasterDataTypeEv, GDAL_GDT_UNKNOWN)
STUB_INT(_ZN14GDALRasterBand8GetXSizeEv, 0)
STUB_INT(_ZN14GDALRasterBand8GetYSizeEv, 0)
STUB_PTR_INT(_ZNK14GDALColorTable13GetColorEntryEi)
STUB_INT(_ZNK14GDALColorTable24GetPaletteInterpretationEv, GDAL_GPI_GRAY)

int
gdal_raster_io(void *self, ...) __asm__(
   "_ZN14GDALRasterBand8RasterIOE10GDALRWFlagiiiiPvii12GDALDataTypexxP20GDALRasterIOExtraArg");

int
gdal_raster_io(void *self, ...)
{
   (void)self;
   return GDAL_CE_FAILURE;
}
