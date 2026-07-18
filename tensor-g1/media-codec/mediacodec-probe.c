#include <stdio.h>

#ifdef __ANDROID__

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

struct codec_case {
   const char *label;
   const char *mime;
};

static int
probe_decoder(const struct codec_case *test)
{
   AMediaCodec *codec = AMediaCodec_createDecoderByType(test->mime);
   if (!codec) {
      printf("%-5s %-24s create: unavailable\n", test->label, test->mime);
      return 1;
   }

   char *name = NULL;
   media_status_t name_status = AMediaCodec_getName(codec, &name);

   AMediaFormat *format = AMediaFormat_new();
   AMediaFormat_setString(format, "mime", test->mime);
   AMediaFormat_setInt32(format, "width", 1920);
   AMediaFormat_setInt32(format, "height", 1080);
   AMediaFormat_setInt32(format, "max-input-size", 4 * 1024 * 1024);

   media_status_t configure =
      AMediaCodec_configure(codec, format, NULL, NULL, 0);
   media_status_t start = AMEDIA_ERROR_UNKNOWN;
   media_status_t stop = AMEDIA_ERROR_UNKNOWN;

   if (configure == AMEDIA_OK) {
      start = AMediaCodec_start(codec);
      if (start == AMEDIA_OK)
         stop = AMediaCodec_stop(codec);
   }

   printf("%-5s %-24s codec=%s configure=%d start=%d stop=%d\n",
          test->label, test->mime,
          name_status == AMEDIA_OK && name ? name : "(unknown)", configure,
          start, stop);

   if (name)
      AMediaCodec_releaseName(codec, name);
   AMediaFormat_delete(format);
   AMediaCodec_delete(codec);

   return configure != AMEDIA_OK || start != AMEDIA_OK || stop != AMEDIA_OK;
}

int
main(void)
{
   static const struct codec_case tests[] = {
      {"H264", "video/avc"},
      {"HEVC", "video/hevc"},
      {"VP9", "video/x-vnd.on2.vp9"},
      {"AV1", "video/av01"},
   };

   int failures = 0;
   for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
      failures += probe_decoder(&tests[i]);

   return failures ? 1 : 0;
}

#else

int
main(void)
{
   fputs("mediacodec-probe must be built for Android\n", stderr);
   return 77;
}

#endif
