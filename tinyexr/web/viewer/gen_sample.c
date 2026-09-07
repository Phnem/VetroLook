/* One-off generator for web/viewer/sample.exr — a small HDR test pattern that
 * showcases the viewer's exposure / gamma / sRGB tone-mapping controls.
 *
 *   cc gen_sample.c -o gen_sample -lm   (from web/viewer/, with tinyexr.h at repo root)
 *   ./gen_sample
 *
 * Not part of the build; kept for reproducibility of sample.exr. */
#include <math.h>
#include <stdlib.h>

#define TINYEXR_IMPLEMENTATION
#include "../../tinyexr.h"

int main(void) {
  const int W = 192, H = 120;
  float *rgba = (float *)malloc((size_t)W * H * 4 * sizeof(float));

  for (int y = 0; y < H; ++y) {
    float v = (float)y / (float)(H - 1);            /* 0..1 top->bottom */
    for (int x = 0; x < W; ++x) {
      float u = (float)x / (float)(W - 1);          /* 0..1 left->right */
      float *p = &rgba[((size_t)y * W + x) * 4];

      /* Base: a hue sweep across x, brightness ramp down y. */
      float r = 0.5f + 0.5f * sinf(6.2831853f * (u + 0.00f));
      float g = 0.5f + 0.5f * sinf(6.2831853f * (u + 0.33f));
      float b = 0.5f + 0.5f * sinf(6.2831853f * (u + 0.66f));
      float scale = 1.0f - 0.9f * v;

      r *= scale; g *= scale; b *= scale;

      /* A few bright HDR "lights" so exposure pulldown is visible. */
      float cx[3] = {0.20f, 0.50f, 0.80f};
      float cy = 0.30f;
      float hdr[3] = {32.0f, 16.0f, 64.0f};
      for (int k = 0; k < 3; ++k) {
        float dx = (u - cx[k]) * (float)W / (float)H;
        float dy = (v - cy);
        float d2 = dx * dx + dy * dy;
        float glow = expf(-d2 * 600.0f) * hdr[k];
        r += glow; g += glow * 0.9f; b += glow * 0.8f;
      }

      p[0] = r; p[1] = g; p[2] = b; p[3] = 1.0f;
    }
  }

  const char *err = NULL;
  int ret = SaveEXR(rgba, W, H, 4, /*save_as_fp16=*/1, "sample.exr", &err);
  free(rgba);
  if (ret != TINYEXR_SUCCESS) {
    if (err) { FreeEXRErrorMessage(err); }
    return 1;
  }
  return 0;
}
