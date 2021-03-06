/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
   Dual demosaicing has been implemented by Ingo Weyrich <heckflosse67@gmx.de> for
   rawtherapee under GNU General Public License Version 3
   and has been modified to work for darktable by Hanno Schwalm (hanno@schwalm-bremen.de).
   Also the code for fast_blur has been taken from rawtherapee capturesharpening,
   implemented also by Ingo Weyrich.
*/

static INLINE float calcBlendFactor(float val, float threshold)
{
    // sigmoid function
    // result is in ]0;1] range
    // inflexion point is at (x, y) (threshold, 0.5)
    return 1.0f / (1.0f + dt_fast_expf(16.0f - (16.0f / threshold) * val));
}

static void fast_blur(float *const restrict src, float *const restrict out, const int width, const int height, const float sigma)
{
  // For a blurring sigma of 2.0f a 13x13 kernel would be optimally required but the 9x9 is by far good enough here 
  float kernel[9][9];
  const double temp = -2.0f * sqrf(sigma);
  float sum = 0.0f;
  for(int i = -4; i <= 4; i++)
  {
    for(int j = -4; j <= 4; j++)
    {
      kernel[i + 4][j + 4] = expf( (sqrf(i) + sqrf(j)) / temp);
      sum += kernel[i + 4][j + 4];
    }
  }
  for(int i = 0; i < 9; i++)
  {
    for(int j = 0; j < 9; j++)
      kernel[i][j] /= sum;
  }
  const float c42 = kernel[0][2];
  const float c41 = kernel[0][3];
  const float c40 = kernel[0][4];
  const float c33 = kernel[1][1];
  const float c32 = kernel[1][2];
  const float c31 = kernel[1][3];
  const float c30 = kernel[1][4];
  const float c22 = kernel[2][2];
  const float c21 = kernel[2][3];
  const float c20 = kernel[2][4];
  const float c11 = kernel[3][3];
  const float c10 = kernel[3][4];
  const float c00 = kernel[4][4];
  const int w1 = width;
  const int w2 = 2*width;
  const int w3 = 3*width;
  const int w4 = 4*width;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(src, out) \
  dt_omp_sharedconst(c42, c41, c40, c33, c32, c31, c30, c22, c21, c20, c11, c10, c00, w1, w2, w3, w4, width, height) \
  schedule(simd:static) aligned(src, out : 64) 
 #endif
  for(int row = 4; row < height - 4; row++)
  {
#if defined(__clang__)
        #pragma clang loop vectorize(assume_safety)
#elif defined(__GNUC__)
        #pragma GCC ivdep
#endif
    for(int col = 4; col < width - 4; col++)
    {
      const int i = row * width + col;
      const float val = c42 * (src[i - w4 - 2] + src[i - w4 + 2] + src[i - w2 - 4] + src[i - w2 + 4] + src[i + w2 - 4] + src[i + w2 + 4] + src[i + w4 - 2] + src[i + w4 + 2]) +
                        c41 * (src[i - w4 - 1] + src[i - w4 + 1] + src[i - w1 - 4] + src[i - w1 + 4] + src[i + w1 - 4] + src[i + w1 + 4] + src[i + w4 - 1] + src[i + w4 + 1]) +
                        c40 * (src[i - w4] + src[i - 4] + src[i + 4] + src[i + w4]) +
                        c33 * (src[i - w3 - 3] + src[i - w3 + 3] + src[i + w3 - 3] + src[i + w3 + 3]) +
                        c32 * (src[i - w3 - 2] + src[i - w3 + 2] + src[i - w2 - 3] + src[i - w2 + 3] + src[i + w2 - 3] + src[i + w2 + 3] + src[i + w3 - 2] + src[i + w3 + 2]) +
                        c31 * (src[i - w3 - 1] + src[i - w3 + 1] + src[i - w1 - 3] + src[i - w1 + 3] + src[i + w1 - 3] + src[i + w1 + 3] + src[i + w3 - 1] + src[i + w3 + 1]) +
                        c30 * (src[i - w3] + src[i - 3] + src[i + 3] + src[i + w3]) +
                        c22 * (src[i - w2 - 2] + src[i - w2 + 2] + src[i + w2 - 2] + src[i + w2 + 2]) +
                        c21 * (src[i - w2 - 1] + src[i - w2 + 1] + src[i - w1 - 2] + src[i - w1 + 2] + src[i + w1 - 2] + src[i + w1 + 2] + src[i + w2 - 1] + src[i + w2 + 1]) +
                        c20 * (src[i - w2] + src[i - 2] + src[i + 2] + src[i + w2]) +
                        c11 * (src[i - w1 - 1] + src[i - w1 + 1] + src[i + w1 - 1] + src[i + w1 + 1]) +
                        c10 * (src[i - w1] + src[i - 1] + src[i + 1] + src[i + w1]) +
                        c00 * src[i];
      out[i] = fminf(1.0f, fmaxf(0.0f, val));
    }
  }
  const float filler = 0.5f;
  dt_iop_image_fill(out, filler, width, 4, 1);
  dt_iop_image_fill(&out[(height-4) * width], filler, width, 4, 1);
  for(int row = 4; row < height - 4; row++)
  {
    dt_iop_image_fill(&out[row * width], filler, 4, 1, 1);
    dt_iop_image_fill(&out[row * width + width - 4], filler, 4, 1, 1);
  }
}

static void blend_images(float *const restrict rgb_data, float *const restrict blend, float *const restrict tmp, const int width, const int height, const float threshold, const gboolean dual_mask)
{
  float *const luminance = blend; // re-use this as temporary data
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(luminance, rgb_data, width, height) \
  schedule(simd:static) aligned(luminance, rgb_data : 64) 
#endif
  for(size_t idx =0; idx < (size_t) width * height; idx++)
  {
    luminance[idx] = lab_f(0.3333333f * (rgb_data[4 * idx] + rgb_data[4 * idx + 1] + rgb_data[4 * idx + 2]));
  }
    
  const float scale = 1.0f / 16.0f;
  {
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(luminance, tmp, width, height, threshold, scale) \
  schedule(simd:static) aligned(luminance, tmp : 64) 
 #endif
    for(int row = 2; row < height - 2; row++)
    {
      for(int col = 2, idx = row * width + col; col < width - 2; col++, idx++)
      {
        float contrast = scale * sqrtf(sqrf(luminance[idx+1] - luminance[idx-1]) + sqrf(luminance[idx + width]   - luminance[idx - width]) +
                                       sqrf(luminance[idx+2] - luminance[idx-2]) + sqrf(luminance[idx + 2*width] - luminance[idx - 2*width]));
        tmp[idx] = calcBlendFactor(contrast, threshold);
      }
    }
  }
  for(int row = 0; row < 2; row++)
  {
    for(int col = 2; col < width - 2; col++)
      tmp[row * width + col] = tmp[2 * width + col];
  }
  for(int row = height - 2; row < height; row++)
  {
    for(int col = 2; col < width - 2; col++)
      tmp[row * width + col] = tmp[(height - 3) * width + col];
  }
  for(int row = 0; row < height; row++)
  {
    tmp[row * width]             = tmp[row * width + 1]         = tmp[row * width + 2];         // left border
    tmp[row * width + width - 2] = tmp[row * width + width - 1] = tmp[row * width + width - 3]; // right border
  }
  fast_blur(tmp, blend, width, height, 2.0f);
}

// dual_demosaic is always called **after** the high-frequency demosaicer (rcd, amaze or one of the non-bayer demosaicers)
// and expects the data available in rgb_data as rgba quadruples. 
static void dual_demosaic(dt_dev_pixelpipe_iop_t *piece, float *const restrict rgb_data, const float *const restrict raw_data,
                          dt_iop_roi_t *const roi_out, const dt_iop_roi_t *const roi_in, const uint32_t filters, const uint8_t (*const xtrans)[6],
                          gboolean dual_mask, float dual_threshold)
{
  const int width = roi_in->width;
  const int height = roi_in->height;
  if((width < 16) || (height < 16)) return;

  // If the threshold is zero and we dont't want to see the blend mask we dont't do anything
  if(dual_threshold <= 0.0f) return;

  float *blend = dt_alloc_align_float((size_t) width * height);
  float *tmp = dt_alloc_align_float((size_t) width * height);
  float *vng_image = dt_alloc_align_float((size_t) 4 * width * height);
  if(!blend || !tmp || !vng_image)
  {
    if(tmp) dt_free_align(tmp);
    if(blend) dt_free_align(blend);
    if(vng_image) dt_free_align(vng_image);
    dt_control_log(_("[dual demosaic] can't allocate internal buffers"));
    return;
  }
  const gboolean info = ((darktable.unmuted & (DT_DEBUG_DEMOSAIC | DT_DEBUG_PERF)) && (piece->pipe->type == DT_DEV_PIXELPIPE_FULL));

  vng_interpolate(vng_image, raw_data, roi_out, roi_in, filters, xtrans, DEMOSAIC_FULL_SCALE);

  dt_times_t start_blend = { 0 }, end_blend = { 0 };
  if(info) dt_get_times(&start_blend);

  const float contrastf = dual_threshold / 100.0f;
  blend_images(rgb_data, blend, tmp, width, height, contrastf, dual_mask);

  if(dual_mask)
  {
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(blend, rgb_data, vng_image, width, height) \
  schedule(simd:static) aligned(blend, vng_image, rgb_data : 64) 
#endif
    for(int idx = 0; idx < width * height; idx++)
    {
      for(int c = 0; c < 4; c++)
        rgb_data[idx * 4 + c] = blend[idx];
    }
    const float filler = 0.0f;
    dt_iop_image_fill(rgb_data, filler, width, 4, 4);
    dt_iop_image_fill(&rgb_data[4 * ((height-4) * width)], filler, width, 4, 4);
    for(int row = 4; row < height - 4; row++)
    {
      dt_iop_image_fill(&rgb_data[4 * row * width], filler, 4, 1, 4);
      dt_iop_image_fill(&rgb_data[4 * (row * width + width - 4)], filler, 4, 1, 4);
    }
  }
  else
  {
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(blend, rgb_data, vng_image, width, height) \
  schedule(simd:static) aligned(blend, vng_image, rgb_data : 64) 
#endif
    for(int idx = 0; idx < width * height; idx++)
    {
      const int oidx = 4 * idx;
      for(int c = 0; c < 4; c++)
        rgb_data[oidx + c] = intp(blend[idx], rgb_data[oidx + c], vng_image[oidx + c]);
    }
  }
  if(info)
  {
    dt_get_times(&end_blend);
    fprintf(stderr," [demosaic] dual blending %.4f secs (%.4f CPU)\n", end_blend.clock - start_blend.clock, end_blend.user - start_blend.user);
  }
  dt_free_align(tmp);
  dt_free_align(blend);
  dt_free_align(vng_image);
}
