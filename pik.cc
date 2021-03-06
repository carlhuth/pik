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

#include "pik.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "adaptive_quantization.h"
#include "arch_specific.h"
#include "bit_buffer.h"
#include "bits.h"
#include "byte_order.h"
#include "butteraugli_comparator.h"
#include "compiler_specific.h"
#include "compressed_image.h"
#include "header.h"
#include "image_io.h"
#include "opsin_image.h"
#include "pik_alpha.h"
#include "quantizer.h"

// If true, prints the quantization maps at each iteration.
bool FLAGS_dump_quant_state = false;

namespace pik {
namespace {

void EncodeU32(const uint32_t val, uint8_t* dest) {
#if PIK_BYTE_ORDER_LITTLE
  memcpy(dest, &val, sizeof(val));
#else
  dest[0] = (val >>  0) & 0xff;
  dest[1] = (val >>  8) & 0xff;
  dest[2] = (val >> 16) & 0xff;
  dest[3] = (val >> 24) & 0xff;
#endif
}

uint32_t DecodeU32(const uint8_t* src) {
  uint32_t val;
#if PIK_BYTE_ORDER_LITTLE
  memcpy(&val, src, sizeof(val));
#else
  val = src[3];
  val <<= 8;
  val += src[2];
  val <<= 8;
  val += src[1];
  val <<= 8;
  val += src[0];
#endif
  return val;
}

inline int Clamp(int minval, int maxval, int val) {
  return std::min(maxval, std::max(minval, val));
}

ImageF TileDistMap(const butteraugli::ImageF& distmap, int tile_size) {
  const int tile_xsize = (distmap.xsize() + tile_size - 1) / tile_size;
  const int tile_ysize = (distmap.ysize() + tile_size - 1) / tile_size;
  ImageF tile_distmap(tile_xsize, tile_ysize);
  for (int tile_y = 0; tile_y < tile_ysize; ++tile_y) {
    for (int tile_x = 0; tile_x < tile_xsize; ++tile_x) {
      int x_max = std::min<int>(distmap.xsize(), tile_size * (tile_x + 1));
      int y_max = std::min<int>(distmap.ysize(), tile_size * (tile_y + 1));
      float max_dist = 0.0;
      for (int y = tile_size * tile_y; y < y_max; ++y) {
        const float* const PIK_RESTRICT row = distmap.Row(y);
        for (int x = tile_size * tile_x; x < x_max; ++x) {
          max_dist = std::max(max_dist, row[x]);
        }
      }
      tile_distmap.Row(tile_y)[tile_x] = max_dist;
    }
  }
  return tile_distmap;
}

ImageF DistToPeakMap(const ImageF& field, float peak_min,
                     int local_radius, float peak_weight) {
  ImageF result(field.xsize(), field.ysize(), -1.0f);
  for (int y0 = 0; y0 < field.ysize(); ++y0) {
    for (int x0 = 0; x0 < field.xsize(); ++x0) {
      int x_min = std::max(0, x0 - local_radius);
      int y_min = std::max(0, y0 - local_radius);
      int x_max = std::min<int>(field.xsize(), x0 + 1 + local_radius);
      int y_max = std::min<int>(field.ysize(), y0 + 1 + local_radius);
      float local_max = peak_min;
      for (int y = y_min; y < y_max; ++y) {
        for (int x = x_min; x < x_max; ++x) {
          local_max = std::max(local_max, field.Row(y)[x]);
        }
      }
      if (field.Row(y0)[x0] >
          (1.0f - peak_weight) * peak_min + peak_weight * local_max) {
        for (int y = y_min; y < y_max; ++y) {
          for (int x = x_min; x < x_max; ++x) {
            float dist = std::max(std::abs(y - y0), std::abs(x - x0));
            float cur_dist = result.Row(y)[x];
            if (cur_dist < 0.0 || cur_dist > dist) {
              result.Row(y)[x] = dist;
            }
          }
        }
      }
    }
  }
  return result;
}

bool AdjustQuantVal(float* const PIK_RESTRICT q,
                    const float d, const float factor,
                    const float quant_max) {
  if (*q >= 0.999f * quant_max) return false;
  const float inv_q = 1.0f / *q;
  const float adj_inv_q = inv_q - factor / (d + 1.0f);
  *q = 1.0f / std::max(1.0f / quant_max, adj_inv_q);
  return true;
}

void DumpHeatmap(const PikInfo* info, const std::string& label,
                 const std::vector<float>& vals, size_t xsize, size_t ysize,
                 float good_threshold, float bad_threshold) {
  std::vector<uint8_t> heatmap(3 * xsize * ysize);
  butteraugli::CreateHeatMapImage(vals, good_threshold, bad_threshold, xsize,
                                  ysize, &heatmap);
  char pathname[200];
  snprintf(pathname, sizeof(pathname), "%s%s%05d.png",
           info->debug_prefix.c_str(), label.c_str(),
           info->num_butteraugli_iters);
  WriteImage(ImageFormatPNG(),
             Image3FromInterleaved(&heatmap[0], xsize, ysize, 3 * xsize),
             pathname);
}

void DumpHeatmaps(const PikInfo* info,
                  size_t xsize, size_t ysize, int qres,
                  float ba_target,
                  const ImageF& quant_field,
                  const ImageF& tile_heatmap) {
  if (info->debug_prefix.empty()) return;
  std::vector<float> qmap(xsize * ysize);
  std::vector<float> dmap(xsize * ysize);
  for (int y = 0; y < quant_field.ysize(); ++y) {
    auto row_q = quant_field.Row(y);
    auto row_d = tile_heatmap.Row(y);
    for (int x = 0; x < quant_field.xsize(); ++x) {
      for (int dy = 0; dy < qres; ++dy) {
        for (int dx = 0; dx < qres; ++dx) {
          int px = qres * x + dx;
          int py = qres * y + dy;
          if (px < xsize && py < ysize) {
            qmap[py * xsize + px] = 1.0f / row_q[x];  // never zero
            dmap[py * xsize + px] = row_d[x];
          }
        }
      }
    }
  }
  DumpHeatmap(info, "quant_heatmap", qmap, xsize, ysize,
              4.0f * ba_target, 6.0f * ba_target);
  DumpHeatmap(info, "tile_heatmap", dmap, xsize, ysize,
              ba_target, 1.5f * ba_target);
}

template <class CompressedImageT>
void FindBestQuantization(const Image3F& opsin_orig,
                          float butteraugli_target,
                          int max_butteraugli_iters,
                          CompressedImageT* img,
                          PikInfo* aux_out) {
  ButteraugliComparator comparator(opsin_orig);
  AdaptiveQuantParams quant_params = img->adaptive_quant_params();
  const float kInitialQuantDC =
      quant_params.initial_quant_val_dc / butteraugli_target;
  const float kInitialQuantAC =
      quant_params.initial_quant_val_ac / butteraugli_target;
  ImageF quant_field(img->block_xsize(), img->block_ysize(), kInitialQuantAC);
  ImageF tile_distmap;
  static const int kMaxOuterIters = 3;
  int outer_iter = 0;
  int butteraugli_iter = 0;
  float quant_max = 4.0f;
  for (;;) {
    if (FLAGS_dump_quant_state) {
      printf("\nQuantization field:\n");
      for (int y = 0; y < img->block_ysize(); ++y) {
        for (int x = 0; x < img->block_xsize(); ++x) {
          printf(" %.5f", quant_field.Row(y)[x]);
        }
        printf("\n");
      }
      printf("max_butteraugli_iters = %d\n", max_butteraugli_iters);
    }
    if (img->quantizer().SetQuantField(kInitialQuantDC, quant_field)) {
      img->Quantize();
      if (butteraugli_iter >= max_butteraugli_iters) {
        break;
      }
      Image3B srgb = img->ToSRGB();
      comparator.Compare(srgb);
      tile_distmap = TileDistMap(comparator.distmap(), kBlockEdge);
      ++butteraugli_iter;
      if (aux_out) {
        DumpHeatmaps(aux_out, opsin_orig.xsize(), opsin_orig.ysize(),
                     kBlockEdge, butteraugli_target, quant_field, tile_distmap);
        if (!aux_out->debug_prefix.empty()) {
          char pathname[200];
          snprintf(pathname, 200, "%s%s%05d.png", aux_out->debug_prefix.c_str(),
                   "rgb_out", aux_out->num_butteraugli_iters);
          WriteImage(ImageFormatPNG(), srgb, pathname);
        }
        ++aux_out->num_butteraugli_iters;
      }
      if (FLAGS_dump_quant_state) {
        printf("\nButteraugli iter: %d\n", butteraugli_iter);
        printf("Butteraugli distance: %f\n", comparator.distance());
        printf("quant_max: %f\n", quant_max);
        img->quantizer().DumpQuantizationMap();
      }
    }
    bool changed = false;
    while (!changed && comparator.distance() > butteraugli_target) {
      for (int radius = 1; radius <= 4 && !changed; ++radius) {
        ImageF dist_to_peak_map = DistToPeakMap(
            tile_distmap, butteraugli_target, radius, 0.65);
        for (int y = 0; y < img->block_ysize(); ++y) {
          float* const PIK_RESTRICT row_q = quant_field.Row(y);
          const float* const PIK_RESTRICT row_dist = dist_to_peak_map.Row(y);
          for (int x = 0; x < img->block_xsize(); ++x) {
            if (row_dist[x] >= 0.0f) {
              static const float kAdjSpeed[kMaxOuterIters] =
                  { 0.1, 0.05, 0.025 };
              const float factor =
                  kAdjSpeed[outer_iter] * tile_distmap.Row(y)[x];
              if (AdjustQuantVal(&row_q[x], row_dist[x], factor, quant_max)) {
                changed = true;
              }
            }
          }
        }
      }
      if (quant_max >= 8.0f) break;
      if (!changed) quant_max += 0.5f;
    }
    if (!changed) {
      if (++outer_iter == kMaxOuterIters) break;
      static const float kQuantScale[kMaxOuterIters] = { 0.0, 0.8, 0.9 };
      for (int y = 0; y < img->block_ysize(); ++y) {
        for (int x = 0; x < img->block_xsize(); ++x) {
          quant_field.Row(y)[x] *= kQuantScale[outer_iter];
        }
      }
    }
  }
}

struct EvalGlobalYToB {
  void SetVal(int ytob) const {
    img->SetYToBDC(ytob);
    for (int tiley = 0; tiley < img->tile_ysize(); ++tiley) {
      for (int tilex = 0; tilex < img->tile_xsize(); ++tilex) {
        img->SetYToBAC(tilex, tiley, ytob);
      }
    }
    img->Quantize();
  }
  size_t operator()(int ytob) const {
    SetVal(ytob);
    CoeffProcessor dc_processor(1);
    ACBlockProcessor ac_processor;
    HistogramBuilder dc_histo(dc_processor.num_contexts());
    HistogramBuilder ac_histo(ac_processor.num_contexts());
    ProcessImage3(PredictDC(img->coeffs()), &dc_processor, &dc_histo);
    ProcessImage3(img->coeffs(), &ac_processor, &ac_histo);
    return dc_histo.EncodedSize(1, 2) + ac_histo.EncodedSize(1, 2);
  }
  CompressedImage* img;
};

struct EvalLocalYToB {
  explicit EvalLocalYToB(CompressedImage* image) :
      img(image), dc_processor(1),
      dc_histo(dc_processor.num_contexts()),
      ac_histo(ac_processor.num_contexts()),
      dc_residuals(PredictDC(img->coeffs())) {
    ProcessImage3(dc_residuals, &dc_processor, &dc_histo);
    ProcessImage3(img->coeffs(), &ac_processor, &ac_histo);
  }
  void SetTile(int tx, int ty) {
    tilex = tx;
    tiley = ty;
  }
  void SetVal(int ytob) {
    img->SetYToBAC(tilex, tiley, ytob);
    const int factor = kTileToBlockRatio;
    for (int iy = 0; iy < factor; ++iy) {
      for (int ix = 0; ix < factor; ++ix) {
        int block_y = factor * tiley + iy;
        int block_x = factor * tilex + ix;
        int offset = block_x * kBlockSize;
        int xmin = kBlockEdge * block_x;
        int ymin = kBlockEdge * block_y;
        if (xmin >= img->xsize() || ymin >= img->ysize()) continue;
        ac_processor.Reset();
        ac_histo.set_weight(-1);
        for (int c = 0; c < 3; ++c) {
          ac_processor.ProcessBlock(&img->coeffs().Row(block_y)[c][offset],
                                    block_x, block_y, c, &ac_histo);
        }
        img->QuantizeBlock(block_x, block_y);
        ac_processor.Reset();
        ac_histo.set_weight(1);
        for (int c = 0; c < 3; ++c) {
          ac_processor.ProcessBlock(&img->coeffs().Row(block_y)[c][offset],
                                    block_x, block_y, c, &ac_histo);
        }
      }
    }
  }
  size_t operator()(int ytob) {
    SetVal(ytob);
    return dc_histo.EncodedSize(1, 2) + ac_histo.EncodedSize(1, 2);
  }
  CompressedImage* img;
  CoeffProcessor dc_processor;
  ACBlockProcessor ac_processor;
  HistogramBuilder dc_histo;
  HistogramBuilder ac_histo;
  Image3W dc_residuals;
  int tilex;
  int tiley;
};

template <class Eval>
int Optimize(Eval* eval, int minval, int maxval,
             int best_val, size_t* best_objval) {
  int start = minval;
  int end = maxval;
  for (int resolution = 16; resolution >= 1; resolution /= 4) {
    for (int val = start; val <= end; val += resolution) {
      size_t objval = (*eval)(val);
      if (objval < *best_objval) {
        best_val = val;
        *best_objval = objval;
      }
    }
    start = std::max(minval, best_val - resolution + 1);
    end = std::min(maxval, best_val + resolution - 1);
  }
  eval->SetVal(best_val);
  return best_val;
}

void FindBestYToBCorrelation(CompressedImage* img) {
  static const int kStartYToB = 120;
  EvalGlobalYToB eval_global{img};
  size_t best_size = eval_global(kStartYToB);
  int global_ytob = Optimize(&eval_global, 0, 255, kStartYToB, &best_size);
  EvalLocalYToB eval_local(img);
  for (int tiley = 0; tiley < img->tile_ysize(); ++tiley) {
    for (int tilex = 0; tilex < img->tile_xsize(); ++tilex) {
      eval_local.SetTile(tilex, tiley);
      Optimize(&eval_local, 0, 255, global_ytob, &best_size);
    }
  }
}

std::string CompressToButteraugliDistance(const Image3F& opsin_orig,
                                          const CompressParams& params,
                                          PikInfo* info) {
  CompressedImage img = CompressedImage::FromOpsinImage(opsin_orig, info);
  img.quantizer().SetQuant(1.0);
  img.Quantize();
  FindBestYToBCorrelation(&img);
  FindBestQuantization(opsin_orig, params.butteraugli_distance,
                       params.max_butteraugli_iters, &img, info);
  return img.Encode();
}

std::string CompressFast(const Image3F& opsin_orig,
                         const CompressParams& params,
                         PikInfo* info) {
  const float kQuantDC = 0.76953163840390082;
  const float kQuantAC = 1.52005680264295;
  CompressedImage img = CompressedImage::FromOpsinImage(opsin_orig, info);
  ImageF qf = AdaptiveQuantizationMap(opsin_orig.plane(1), kBlockEdge);
  img.quantizer().SetQuantField(kQuantDC, ScaleImage(kQuantAC, qf));
  img.Quantize();
  return img.EncodeFast();
}

template <typename CompressedImageT>
bool ScaleQuantizationMap(const float quant_dc,
                          const ImageF& quant_field_ac,
                          float scale,
                          CompressedImageT* img) {
  float scale_dc = 0.8 * scale + 0.2;
  bool changed = img->quantizer().SetQuantField(
      scale_dc * quant_dc, ScaleImage(scale, quant_field_ac));
  if (FLAGS_dump_quant_state) {
    printf("\nScaling quantization map with scale %f\n", scale);
    img->quantizer().DumpQuantizationMap();
  }
  img->Quantize();
  return changed;
}

template <typename CompressedImageT>
std::string CompressToTargetSize(const Image3F& opsin_orig, size_t target_size,
                                 CompressedImageT* img, PikInfo* aux_out) {
  float quant_dc;
  ImageF quant_ac;
  img->quantizer().GetQuantField(&quant_dc, &quant_ac);
  float scale_bad = 1.0;
  float scale_good = 1.0;
  std::string candidate;
  std::string compressed;
  for (int i = 0; i < 10; ++i) {
    ScaleQuantizationMap(quant_dc, quant_ac, scale_good, img);
    candidate = img->Encode();
    if (candidate.size() <= target_size) {
      compressed = candidate;
      break;
    }
    scale_bad = scale_good;
    scale_good *= 0.5;
  }
  if (compressed.empty()) {
    // We could not make the compressed size small enough, so we return the
    // last candidate;
    return candidate;
  }
  if (scale_good == 1.0) {
    // We dont want to go below butteraugli distance 1.0
    return compressed;
  }
  for (int i = 0; i < 16; ++i) {
    float scale = 0.5 * (scale_bad + scale_good);
    if (!ScaleQuantizationMap(quant_dc, quant_ac, scale, img)) {
      break;
    }
    candidate = img->Encode();
    if (candidate.size() <= target_size) {
      compressed = candidate;
      scale_good = scale;
    } else {
      scale_bad = scale;
    }
  }
  return compressed;
}

std::string CompressToTargetSize(const Image3F& opsin_orig,
                                 const CompressParams& params,
                                 size_t target_size, PikInfo* aux_out) {
  CompressedImage img = CompressedImage::FromOpsinImage(opsin_orig, aux_out);
  img.quantizer().SetQuant(1.0);
  img.Quantize();
  FindBestYToBCorrelation(&img);
  FindBestQuantization(opsin_orig, 1.0, params.max_butteraugli_iters,
                       &img, aux_out);
  return CompressToTargetSize(opsin_orig, target_size, &img, aux_out);
}




void ToImage3(const CompressedImage& compressed, Image3B* image) {
  *image = compressed.ToSRGB();
}

void ToImage3(const CompressedImage& compressed, Image3U* image) {
  *image = compressed.ToSRGB16();
}

void ToImage3(const CompressedImage& compressed, Image3F* image) {
  *image = compressed.ToLinear();
}
}  // namespace


template<typename T>
bool AlphaToPik(const CompressParams& params, const MetaImage<T>& image,
                 PaddedBytes* compressed, PikInfo* aux_out) {
  if (!image.HasAlpha()) {
    return PIK_FAILURE("Must have alpha if alpha_channel set");
  }
  size_t bytepos = compressed->size();
  if (!AlphaToPik(params, image.GetAlpha(), &bytepos, compressed)) {
    return false;
  }
  return true;
}

template<typename T>
bool AlphaToPik(const CompressParams& params, const Image3<T>& image,
                 PaddedBytes* compressed, PikInfo* aux_out) {
  return PIK_FAILURE("Alpha not supported for Image3");
}

template<typename T>
Image3F OpsinDynamicsImage(const MetaImage<T>& image) {
  return OpsinDynamicsImage(image.GetColor());
}

template<typename Image>
bool PixelsToPikT(const CompressParams& params, const Image& image,
                  PaddedBytes* compressed, PikInfo* aux_out) {
  if (image.xsize() == 0 || image.ysize() == 0) {
    return PIK_FAILURE("Empty image");
  }
  if (!OpsinToPik(params, OpsinDynamicsImage(image), compressed, aux_out)) {
    return false;
  }
  if (params.alpha_channel) {
    if (!AlphaToPik(params, image, compressed, aux_out)) {
      return false;
    }
  }
  return true;
}

bool PixelsToPik(const CompressParams& params, const Image3B& image,
                 PaddedBytes* compressed, PikInfo* aux_out) {
  return PixelsToPikT(params, image, compressed, aux_out);
}

bool PixelsToPik(const CompressParams& params, const Image3F& image,
                 PaddedBytes* compressed, PikInfo* aux_out) {
  return PixelsToPikT(params, image, compressed, aux_out);
}

bool PixelsToPik(const CompressParams& params, const MetaImageB& image,
                 PaddedBytes* compressed, PikInfo* aux_out) {
  return PixelsToPikT(params, image, compressed, aux_out);
}

bool PixelsToPik(const CompressParams& params, const MetaImageF& image,
                 PaddedBytes* compressed, PikInfo* aux_out) {
  return PixelsToPikT(params, image, compressed, aux_out);
}

bool OpsinToPik(const CompressParams& params, const Image3F& opsin,
                PaddedBytes* compressed, PikInfo* aux_out) {
  if (opsin.xsize() == 0 || opsin.ysize() == 0) {
    return PIK_FAILURE("Empty image");
  }
  const size_t xsize = opsin.xsize();
  const size_t ysize = opsin.ysize();
  // OpsinDynamics code path.
  std::string compressed_data;
  if (params.butteraugli_distance >= 0.0) {
    compressed_data = CompressToButteraugliDistance(opsin, params, aux_out);
  } else if (params.target_bitrate > 0.0) {
    size_t target_size =
        opsin.xsize() * opsin.ysize() * params.target_bitrate / 8.0;
    compressed_data = CompressToTargetSize(opsin, params, target_size,
                                           aux_out);
  } else if (params.uniform_quant > 0.0) {
    CompressedImage img = CompressedImage::FromOpsinImage(opsin, aux_out);
    img.quantizer().SetQuant(params.uniform_quant);
    img.Quantize();
    compressed_data = img.Encode();
  } else if (params.fast_mode) {
    compressed_data = CompressFast(opsin, params, aux_out);
  } else {
    return PIK_FAILURE("Not implemented");
  }

  Header header;
  header.xsize = xsize;
  header.ysize = ysize;
  if (params.alpha_channel) {
    header.flags |= Header::kAlpha;
  }
  compressed->resize(MaxCompressedHeaderSize() + compressed_data.size());
  BitSink sink(compressed->data());
  if (!StoreHeader(header, &sink)) return false;
  const size_t header_size = sink.Finalize() - compressed->data();
  compressed->resize(header_size + compressed_data.size());  // no copy!
  memcpy(compressed->data() + header_size, compressed_data.data(),
         compressed_data.size());
  return true;
}


template <typename T>
bool PikToPixelsT(const DecompressParams& params, const PaddedBytes& compressed,
                  MetaImage<T>* image, PikInfo* aux_out) {
  if (compressed.size() == 0) {
    return PIK_FAILURE("Empty input.");
  }
  Image3<T> planes;
  const uint8_t* const compressed_end = compressed.data() + compressed.size();

  Header header;
  BitSource source(compressed.data());
  if (!LoadHeader(&source, &header)) return false;
  const uint8_t* const PIK_RESTRICT header_end = source.Finalize();
  if (header_end > compressed_end) {
    return PIK_FAILURE("Truncated header.");
  }
  size_t byte_pos = header_end - compressed.data();

  if (header.flags & Header::kWebPLossless) {
    return PIK_FAILURE("Invalid format code");
  } else {  // Pik
    if (header.xsize == 0 || header.ysize == 0) {
      return PIK_FAILURE("Empty image.");
    }
    static const uint32_t kMaxWidth = (1 << 25) - 1;
    if (header.xsize > kMaxWidth) {
      return PIK_FAILURE("Image too wide.");
    }
    uint64_t num_pixels = static_cast<uint64_t>(header.xsize) * header.ysize;
    if (num_pixels > params.max_num_pixels) {
      return PIK_FAILURE("Image too big.");
    }
    CompressedImage img(header.xsize, header.ysize, aux_out);
    size_t bytes_read;
    if (!img.Decode(header_end, compressed.size() - byte_pos, &bytes_read)) {
      return PIK_FAILURE("Pik decoding failed.");
    }
    byte_pos += bytes_read;
    ToImage3(img, &planes);
    image->SetColor(std::move(planes));

    if (header.flags & Header::kAlpha) {
      image->AddAlpha();
      size_t bytes_read;
      if (!PikToAlpha(params, byte_pos, compressed, &bytes_read,
                      &image->GetAlpha())) {
        return false;
      }
      byte_pos += bytes_read;
    }
  }
  if (params.check_decompressed_size && byte_pos != compressed.size()) {
    return PIK_FAILURE("Pik compressed data size mismatch.");
  }
  if (aux_out != nullptr) {
    aux_out->decoded_size = byte_pos;
  }
  return true;
}

bool PikToPixels(const DecompressParams& params, const PaddedBytes& compressed,
                 MetaImageB* image, PikInfo* aux_out) {
  return PikToPixelsT(params, compressed, image, aux_out);
}

bool PikToPixels(const DecompressParams& params, const PaddedBytes& compressed,
                 MetaImageU* image, PikInfo* aux_out) {
  return PikToPixelsT(params, compressed, image, aux_out);
}

bool PikToPixels(const DecompressParams& params, const PaddedBytes& compressed,
                 MetaImageF* image, PikInfo* aux_out) {
  return PikToPixelsT(params, compressed, image, aux_out);
}

template<typename T>
bool PikToPixelsT(const DecompressParams& params, const PaddedBytes& compressed,
                 Image3<T>* image, PikInfo* aux_out) {
  MetaImage<T> temp;
  if (!PikToPixelsT(params, compressed, &temp, aux_out)) {
    return false;
  }
  if (temp.HasAlpha()) {
    return PIK_FAILURE("Unable to output alpha channel");
  }
  *image = std::move(temp.GetColor());
  return true;
}

bool PikToPixels(const DecompressParams& params,
                 const PaddedBytes& compressed,
                 Image3B* image, PikInfo* aux_out) {
  return PikToPixelsT(params, compressed, image, aux_out);
}
bool PikToPixels(const DecompressParams& params,
                 const PaddedBytes& compressed,
                 Image3U* image, PikInfo* aux_out) {
  return PikToPixelsT(params, compressed, image, aux_out);
}
bool PikToPixels(const DecompressParams& params,
                 const PaddedBytes& compressed,
                 Image3F* image, PikInfo* aux_out) {
  return PikToPixelsT(params, compressed, image, aux_out);
}

}  // namespace pik
