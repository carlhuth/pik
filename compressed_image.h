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

#ifndef COMPRESSED_IMAGE_H_
#define COMPRESSED_IMAGE_H_

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <string>
#include <utility>

#include "compiler_specific.h"
#include "image.h"
#include "opsin_codec.h"
#include "pik_info.h"
#include "quantizer.h"

namespace pik {

static const int kBlockEdge = 8;
static const int kTileToBlockRatio = 8;
static const int kBlockSize = kBlockEdge * kBlockEdge;
static const int kTileEdge = kBlockEdge * kTileToBlockRatio;

// Represents both the quantized and transformed original version of an image.
// This class is used in both the encoder and decoder.
class CompressedImage {
 public:
  // The image is in an undefined state until Decode or Quantize are called.
  CompressedImage(int xsize, int ysize, PikInfo* info);

  // Creates a compressed image from an opsin-dynamics image original.
  // The compressed image is in an undefined state until Quantize() is called.
  static CompressedImage FromOpsinImage(const Image3F& opsin, PikInfo* info);

  // Replaces *this with a compressed image from the bitstream.
  // Sets *compressed_size to the number of bytes read from the data buffer.
  bool Decode(const uint8_t* data, const size_t data_size,
              size_t* compressed_size);

  int xsize() const { return xsize_; }
  int ysize() const { return ysize_; }
  int block_xsize() const { return block_xsize_; }
  int block_ysize() const { return block_ysize_; }
  int tile_xsize() const { return tile_xsize_; }
  int tile_ysize() const { return tile_ysize_; }

  Quantizer& quantizer() { return quantizer_; }
  const Quantizer& quantizer() const { return quantizer_; }

  void QuantizeBlock(int block_x, int block_y);
  void Quantize();

  void DequantizeBlock(const int block_x, const int block_y,
                       float* const PIK_RESTRICT block) const;

  AdaptiveQuantParams adaptive_quant_params() const {
    AdaptiveQuantParams p;
    p.initial_quant_val_dc = 1.0625;
    p.initial_quant_val_ac = 0.5625;
    return p;
  }

  // Returns the SRGB image based on the quantization values and the quantized
  // coefficients.
  Image3B ToSRGB() const;
  Image3U ToSRGB16() const;

  // Returns the image as linear (gamma expanded) sRGB
  Image3F ToLinear() const;

  const Image3W& coeffs() const { return dct_coeffs_; }

  // Returns a lossless encoding of the quantized coefficients.
  std::string Encode() const;
  std::string EncodeFast() const;

  // Getters and setters for adaptive Y-to-blue correlation.
  // (Clang generates a floating-point multiply.)
  float YToBDC() const { return ytob_dc_ / 128.0f; }
  float YToBAC(int tx, int ty) const { return ytob_ac_.Row(ty)[tx] / 128.0f; }
  void SetYToBDC(int ytob) { ytob_dc_ = ytob; }
  void SetYToBAC(int tx, int ty, int val) { ytob_ac_.Row(ty)[tx] = val; }

 private:
  void QuantizeDC();
  void ComputeOpsinOverlay();

  const int xsize_;
  const int ysize_;
  const int block_xsize_;
  const int block_ysize_;
  const int tile_xsize_;
  const int tile_ysize_;
  const int num_blocks_;
  Quantizer quantizer_;
  Image3W dct_coeffs_;
  // Transformed version of the original image, only present if the image
  // was constructed with FromOpsinImage().
  std::unique_ptr<Image3F> opsin_image_;
  // Pixel space overlay image computed from quantized dct coefficients in
  // both the encoder and the decoder.
  std::unique_ptr<ImageF> opsin_overlay_;
  int ytob_dc_;
  Image<int> ytob_ac_;
  // Not owned, used to report additional statistics to the callers of
  // PixelsToPik() and PikToPixels().
  PikInfo* pik_info_;
};

}  // namespace pik

#endif  // COMPRESSED_IMAGE_H_
