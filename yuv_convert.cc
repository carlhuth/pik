// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "yuv_convert.h"

#include <stdint.h>
#include <algorithm>
#include <array>
#include <type_traits>

#include "compiler_specific.h"
#include "gamma_correct.h"

namespace pik {

// Conversion matrices and functions between 8 or 16 bit sRGB and
// 8, 10 or 12 bit YUV Rec 709 color spaces.

constexpr double kWeightR = 0.2126;
constexpr double kWeightB = 0.0722;
constexpr double kWeightG = 1.0 - kWeightR - kWeightB;
constexpr double kWeightBc = 1.0 - kWeightB;
constexpr double kWeightRc = 1.0 - kWeightR;
constexpr double kScaleY = 219.0 / 255.0;
constexpr double kScaleUV = 112.0 / 255.0;

constexpr double RGBtoYUVMatrix[9] = {
  kWeightR * kScaleY,
  kWeightG * kScaleY,
  kWeightB * kScaleY,
  (-kWeightR / kWeightBc) * kScaleUV,
  (-kWeightG / kWeightBc) * kScaleUV,
  kScaleUV,
  kScaleUV,
  (-kWeightG / kWeightRc) * kScaleUV,
  (-kWeightB / kWeightRc) * kScaleUV,
};

constexpr double RGBtoYUVMatrixAdd[3] = { .0625, .5, .5 };

constexpr double YUVtoRGBMatrix[9] = {
  1.0 / kScaleY,
  0.0,
  kWeightRc / kScaleUV,
  1.0 / kScaleY,
  -kWeightBc * kWeightB / kWeightG / kScaleUV,
  -kWeightRc * kWeightR / kWeightG / kScaleUV,
  1.0 / kScaleY,
  kWeightBc / kScaleUV,
  0.0
};

#define clamp(V, M) (uint16_t)((V) < 0 ? 0 : ((V) > (M) ? (M) : V))

// Input range:  [0 .. (1<<bits)-1]
// Output range: [0.0 .. 1.0]
void YUVPixelToRGB(uint16_t yv, uint16_t uv, uint16_t vv, int bits,
                   double* r, double* g, double* b) {
  const double norm = 1. / ((1 << bits) - 1);
  const double y = yv * norm - RGBtoYUVMatrixAdd[0];
  const double u = uv * norm - RGBtoYUVMatrixAdd[1];
  const double v = vv * norm - RGBtoYUVMatrixAdd[2];
  *r = YUVtoRGBMatrix[0] * y + YUVtoRGBMatrix[1] * u + YUVtoRGBMatrix[2] * v;
  *g = YUVtoRGBMatrix[3] * y + YUVtoRGBMatrix[4] * u + YUVtoRGBMatrix[5] * v;
  *b = YUVtoRGBMatrix[6] * y + YUVtoRGBMatrix[7] * u + YUVtoRGBMatrix[8] * v;
}

// Input range:  [0 .. (1<<bits)-1]
template <typename T>
void YUVPixelToRGB(uint16_t yv, uint16_t uv, uint16_t vv, int bits,
                   T *r, T *g, T *b) {
  const int maxv_out = (1 << (8 * sizeof(T))) - 1;
  double rd, gd, bd;
  YUVPixelToRGB(yv, uv, vv, bits, &rd, &gd, &bd);
  *r = clamp(.5 + maxv_out * rd, maxv_out);
  *g = clamp(.5 + maxv_out * gd, maxv_out);
  *b = clamp(.5 + maxv_out * bd, maxv_out);
}

// Input range:  [0.0 .. 1.0]
// Output range: [0 .. (1<<bits)-1]
void RGBPixelToYUV(double r, double g, double b, int bits,
                   uint16_t *y, uint16_t *u, uint16_t *v) {
  const double maxv = (1 << bits) - 1;
  const double Y = RGBtoYUVMatrixAdd[0] +
                     RGBtoYUVMatrix[0] * r +
                     RGBtoYUVMatrix[1] * g +
                     RGBtoYUVMatrix[2] * b;
  const double U = RGBtoYUVMatrixAdd[1] +
                     RGBtoYUVMatrix[3] * r +
                     RGBtoYUVMatrix[4] * g +
                     RGBtoYUVMatrix[5] * b;
  const double V = RGBtoYUVMatrixAdd[2] +
                     RGBtoYUVMatrix[6] * r +
                     RGBtoYUVMatrix[7] * g +
                     RGBtoYUVMatrix[8] * b;
  *y = clamp(.5 + maxv * Y, maxv);
  *u = clamp(.5 + maxv * U, maxv);
  *v = clamp(.5 + maxv * V, maxv);
}

// Output range: [0 .. (1<<bits)-1]
template <typename T>
void RGBPixelToYUV(T r, T g, T b, int bits,
                   uint16_t *y, uint16_t *u, uint16_t *v) {
  const double norm = 1. / ((1 << (8 * sizeof(T))) - 1);
  const double rd = r * norm;
  const double gd = g * norm;
  const double bd = b * norm;
  RGBPixelToYUV(rd, gd, bd, bits, y, u, v);
}

//
// Wrapper functions to convert between 8-bit, 16-bit or linear sRGB images
// and 8, 10 or 12 bit YUV Rec 709 images.
//

template <typename T>
void YUVRec709ImageToRGB(const Image3U& yuv, int bit_depth, Image3<T>* rgb) {
  for (int y = 0; y < yuv.ysize(); ++y) {
    auto row_yuv = yuv.Row(y);
    auto row_rgb = rgb->Row(y);
    for (int x = 0; x < yuv.xsize(); ++x) {
      YUVPixelToRGB(row_yuv[0][x], row_yuv[1][x], row_yuv[2][x], bit_depth,
                    &row_rgb[0][x], &row_rgb[1][x], &row_rgb[2][x]);
    }
  }
}

Image3B RGB8ImageFromYUVRec709(const Image3U& yuv, int bit_depth) {
  Image3B rgb(yuv.xsize(), yuv.ysize());
  YUVRec709ImageToRGB(yuv, bit_depth, &rgb);
  return rgb;
}

Image3U RGB16ImageFromYUVRec709(const Image3U& yuv, int bit_depth) {
  Image3U rgb(yuv.xsize(), yuv.ysize());
  YUVRec709ImageToRGB(yuv, bit_depth, &rgb);
  return rgb;
}

Image3F RGBLinearImageFromYUVRec709(const Image3U& yuv, int bit_depth) {
  Image3F rgb(yuv.xsize(), yuv.ysize());
  for (int y = 0; y < yuv.ysize(); ++y) {
    const auto row_yuv = yuv.ConstRow(y);
    auto row_linear = rgb.Row(y);
    for (int x = 0; x < yuv.xsize(); ++x) {
      double rd, gd, bd;
      YUVPixelToRGB(row_yuv[0][x], row_yuv[1][x], row_yuv[2][x], bit_depth,
                    &rd, &gd, &bd);
      row_linear[0][x] = Srgb8ToLinearDirect(rd * 255.0);
      row_linear[1][x] = Srgb8ToLinearDirect(gd * 255.0);
      row_linear[2][x] = Srgb8ToLinearDirect(bd * 255.0);
    }
  }
  return rgb;
}

template <typename T>
void RGBImageToYUVRec709(const Image3<T>& rgb, int bit_depth, Image3U* yuv) {
  for (int y = 0; y < rgb.ysize(); ++y) {
    const auto row_rgb = rgb.ConstRow(y);
    auto row_yuv = yuv->Row(y);
    for (int x = 0; x < rgb.xsize(); ++x) {
      RGBPixelToYUV(row_rgb[0][x], row_rgb[1][x], row_rgb[2][x], bit_depth,
                    &row_yuv[0][x], &row_yuv[1][x], &row_yuv[2][x]);
    }
  }
}

Image3U YUVRec709ImageFromRGB8(const Image3B& rgb, int out_bit_depth) {
  Image3U yuv(rgb.xsize(), rgb.ysize());
  RGBImageToYUVRec709(rgb, out_bit_depth, &yuv);
  return yuv;
}

Image3U YUVRec709ImageFromRGB16(const Image3U& rgb, int out_bit_depth) {
  Image3U yuv(rgb.xsize(), rgb.ysize());
  RGBImageToYUVRec709(rgb, out_bit_depth, &yuv);
  return yuv;
}

Image3U YUVRec709ImageFromRGBLinear(const Image3F& rgb, int out_bit_depth) {
  Image3U yuv(rgb.xsize(), rgb.ysize());
  const double norm = 1. / 255.;
  for (int y = 0; y < yuv.ysize(); ++y) {
    auto row_yuv = yuv.Row(y);
    const auto row_linear = rgb.ConstRow(y);
    for (int x = 0; x < yuv.xsize(); ++x) {
      double rd = LinearToSrgb8Direct(row_linear[0][x]) * norm;
      double gd = LinearToSrgb8Direct(row_linear[1][x]) * norm;
      double bd = LinearToSrgb8Direct(row_linear[2][x]) * norm;
      RGBPixelToYUV(rd, gd, bd, out_bit_depth,
                    &row_yuv[0][x], &row_yuv[1][x], &row_yuv[2][x]);
    }
  }
  return yuv;
}

}  // namespace pik
