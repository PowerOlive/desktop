// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef VectorMathMSA_h
#define VectorMathMSA_h

#include <algorithm>

#include "platform/audio/VectorMathScalar.h"
#include "platform/cpu/mips/CommonMacrosMSA.h"

namespace blink {
namespace VectorMath {
namespace MSA {

static ALWAYS_INLINE void Vadd(const float* source1p,
                               int source_stride1,
                               const float* source2p,
                               int source_stride2,
                               float* dest_p,
                               int dest_stride,
                               size_t frames_to_process) {
  int n = frames_to_process;

  if (source_stride1 == 1 && source_stride2 == 1 && dest_stride == 1) {
    v4f32 vSrc1P0, vSrc1P1, vSrc1P2, vSrc1P3, vSrc1P4, vSrc1P5, vSrc1P6,
        vSrc1P7;
    v4f32 vSrc2P0, vSrc2P1, vSrc2P2, vSrc2P3, vSrc2P4, vSrc2P5, vSrc2P6,
        vSrc2P7;
    v4f32 vDst0, vDst1, vDst2, vDst3, vDst4, vDst5, vDst6, vDst7;

    for (; n >= 32; n -= 32) {
      LD_SP8(source1p, 4, vSrc1P0, vSrc1P1, vSrc1P2, vSrc1P3, vSrc1P4, vSrc1P5,
             vSrc1P6, vSrc1P7);
      LD_SP8(source2p, 4, vSrc2P0, vSrc2P1, vSrc2P2, vSrc2P3, vSrc2P4, vSrc2P5,
             vSrc2P6, vSrc2P7);
      ADD4(vSrc1P0, vSrc2P0, vSrc1P1, vSrc2P1, vSrc1P2, vSrc2P2, vSrc1P3,
           vSrc2P3, vDst0, vDst1, vDst2, vDst3);
      ADD4(vSrc1P4, vSrc2P4, vSrc1P5, vSrc2P5, vSrc1P6, vSrc2P6, vSrc1P7,
           vSrc2P7, vDst4, vDst5, vDst6, vDst7);
      ST_SP8(vDst0, vDst1, vDst2, vDst3, vDst4, vDst5, vDst6, vDst7, dest_p, 4);
    }
  }

  Scalar::Vadd(source1p, source_stride1, source2p, source_stride2, dest_p,
               dest_stride, n);
}

static ALWAYS_INLINE void Vclip(const float* source_p,
                                int source_stride,
                                const float* low_threshold_p,
                                const float* high_threshold_p,
                                float* dest_p,
                                int dest_stride,
                                size_t frames_to_process) {
  int n = frames_to_process;

  if (source_stride == 1 && dest_stride == 1) {
    v4f32 vSrc0, vSrc1, vSrc2, vSrc3, vSrc4, vSrc5, vSrc6, vSrc7;
    v4f32 vDst0, vDst1, vDst2, vDst3, vDst4, vDst5, vDst6, vDst7;
    v4f32 vLowThr, vHighThr;
    FloatInt lowThr, highThr;

    lowThr.floatVal = *low_threshold_p;
    highThr.floatVal = *high_threshold_p;
    vLowThr = (v4f32)__msa_fill_w(lowThr.intVal);
    vHighThr = (v4f32)__msa_fill_w(highThr.intVal);

    for (; n >= 32; n -= 32) {
      LD_SP8(source_p, 4, vSrc0, vSrc1, vSrc2, vSrc3, vSrc4, vSrc5, vSrc6,
             vSrc7);
      VCLIP4(vSrc0, vSrc1, vSrc2, vSrc3, vLowThr, vHighThr, vDst0, vDst1, vDst2,
             vDst3);
      VCLIP4(vSrc4, vSrc5, vSrc6, vSrc7, vLowThr, vHighThr, vDst4, vDst5, vDst6,
             vDst7);
      ST_SP8(vDst0, vDst1, vDst2, vDst3, vDst4, vDst5, vDst6, vDst7, dest_p, 4);
    }
  }

  Scalar::Vclip(source_p, source_stride, low_threshold_p, high_threshold_p,
                dest_p, dest_stride, n);
}

static ALWAYS_INLINE void Vmaxmgv(const float* source_p,
                                  int source_stride,
                                  float* max_p,
                                  size_t frames_to_process) {
  int n = frames_to_process;

  if (source_stride == 1) {
    v4f32 vMax = {
        0,
    };
    v4f32 vSrc0, vSrc1, vSrc2, vSrc3, vSrc4, vSrc5, vSrc6, vSrc7;
    const v16i8 vSignBitMask = (v16i8)__msa_fill_w(0x7FFFFFFF);

    for (; n >= 32; n -= 32) {
      LD_SP8(source_p, 4, vSrc0, vSrc1, vSrc2, vSrc3, vSrc4, vSrc5, vSrc6,
             vSrc7);
      AND_W4_SP(vSrc0, vSrc1, vSrc2, vSrc3, vSignBitMask);
      VMAX_W4_SP(vSrc0, vSrc1, vSrc2, vSrc3, vMax);
      AND_W4_SP(vSrc4, vSrc5, vSrc6, vSrc7, vSignBitMask);
      VMAX_W4_SP(vSrc4, vSrc5, vSrc6, vSrc7, vMax);
    }

    *max_p = std::max(*max_p, vMax[0]);
    *max_p = std::max(*max_p, vMax[1]);
    *max_p = std::max(*max_p, vMax[2]);
    *max_p = std::max(*max_p, vMax[3]);
  }

  Scalar::Vmaxmgv(source_p, source_stride, max_p, n);
}

static ALWAYS_INLINE void Vmul(const float* source1p,
                               int source_stride1,
                               const float* source2p,
                               int source_stride2,
                               float* dest_p,
                               int dest_stride,
                               size_t frames_to_process) {
  int n = frames_to_process;

  if (source_stride1 == 1 && source_stride2 == 1 && dest_stride == 1) {
    v4f32 vSrc1P0, vSrc1P1, vSrc1P2, vSrc1P3, vSrc1P4, vSrc1P5, vSrc1P6,
        vSrc1P7;
    v4f32 vSrc2P0, vSrc2P1, vSrc2P2, vSrc2P3, vSrc2P4, vSrc2P5, vSrc2P6,
        vSrc2P7;
    v4f32 vDst0, vDst1, vDst2, vDst3, vDst4, vDst5, vDst6, vDst7;

    for (; n >= 32; n -= 32) {
      LD_SP8(source1p, 4, vSrc1P0, vSrc1P1, vSrc1P2, vSrc1P3, vSrc1P4, vSrc1P5,
             vSrc1P6, vSrc1P7);
      LD_SP8(source2p, 4, vSrc2P0, vSrc2P1, vSrc2P2, vSrc2P3, vSrc2P4, vSrc2P5,
             vSrc2P6, vSrc2P7);
      MUL4(vSrc1P0, vSrc2P0, vSrc1P1, vSrc2P1, vSrc1P2, vSrc2P2, vSrc1P3,
           vSrc2P3, vDst0, vDst1, vDst2, vDst3);
      MUL4(vSrc1P4, vSrc2P4, vSrc1P5, vSrc2P5, vSrc1P6, vSrc2P6, vSrc1P7,
           vSrc2P7, vDst4, vDst5, vDst6, vDst7);
      ST_SP8(vDst0, vDst1, vDst2, vDst3, vDst4, vDst5, vDst6, vDst7, dest_p, 4);
    }
  }

  Scalar::Vmul(source1p, source_stride1, source2p, source_stride2, dest_p,
               dest_stride, n);
}

static ALWAYS_INLINE void Vsma(const float* source_p,
                               int source_stride,
                               const float* scale,
                               float* dest_p,
                               int dest_stride,
                               size_t frames_to_process) {
  int n = frames_to_process;

  if (source_stride == 1 && dest_stride == 1) {
    float* destPCopy = dest_p;
    v4f32 vScale;
    v4f32 vSrc0, vSrc1, vSrc2, vSrc3, vSrc4, vSrc5, vSrc6, vSrc7;
    v4f32 vDst0, vDst1, vDst2, vDst3, vDst4, vDst5, vDst6, vDst7;
    FloatInt scaleVal;

    scaleVal.floatVal = *scale;
    vScale = (v4f32)__msa_fill_w(scaleVal.intVal);

    for (; n >= 32; n -= 32) {
      LD_SP8(source_p, 4, vSrc0, vSrc1, vSrc2, vSrc3, vSrc4, vSrc5, vSrc6,
             vSrc7);
      LD_SP8(destPCopy, 4, vDst0, vDst1, vDst2, vDst3, vDst4, vDst5, vDst6,
             vDst7);
      VSMA4(vSrc0, vSrc1, vSrc2, vSrc3, vDst0, vDst1, vDst2, vDst3, vScale);
      VSMA4(vSrc4, vSrc5, vSrc6, vSrc7, vDst4, vDst5, vDst6, vDst7, vScale);
      ST_SP8(vDst0, vDst1, vDst2, vDst3, vDst4, vDst5, vDst6, vDst7, dest_p, 4);
    }
  }

  Scalar::Vsma(source_p, source_stride, scale, dest_p, dest_stride, n);
}

static ALWAYS_INLINE void Vsmul(const float* source_p,
                                int source_stride,
                                const float* scale,
                                float* dest_p,
                                int dest_stride,
                                size_t frames_to_process) {
  int n = frames_to_process;

  if (source_stride == 1 && dest_stride == 1) {
    v4f32 vScale;
    v4f32 vSrc0, vSrc1, vSrc2, vSrc3, vSrc4, vSrc5, vSrc6, vSrc7;
    v4f32 vDst0, vDst1, vDst2, vDst3, vDst4, vDst5, vDst6, vDst7;
    FloatInt scaleVal;

    scaleVal.floatVal = *scale;
    vScale = (v4f32)__msa_fill_w(scaleVal.intVal);

    for (; n >= 32; n -= 32) {
      LD_SP8(source_p, 4, vSrc0, vSrc1, vSrc2, vSrc3, vSrc4, vSrc5, vSrc6,
             vSrc7);
      VSMUL4(vSrc0, vSrc1, vSrc2, vSrc3, vDst0, vDst1, vDst2, vDst3, vScale);
      VSMUL4(vSrc4, vSrc5, vSrc6, vSrc7, vDst4, vDst5, vDst6, vDst7, vScale);
      ST_SP8(vDst0, vDst1, vDst2, vDst3, vDst4, vDst5, vDst6, vDst7, dest_p, 4);
    }
  }

  Scalar::Vsmul(source_p, source_stride, scale, dest_p, dest_stride, n);
}

static ALWAYS_INLINE void Vsvesq(const float* source_p,
                                 int source_stride,
                                 float* sum_p,
                                 size_t frames_to_process) {
  Scalar::Vsvesq(source_p, source_stride, sum_p, frames_to_process);
}

static ALWAYS_INLINE void Zvmul(const float* real1p,
                                const float* imag1p,
                                const float* real2p,
                                const float* imag2p,
                                float* real_dest_p,
                                float* imag_dest_p,
                                size_t frames_to_process) {
  Scalar::Zvmul(real1p, imag1p, real2p, imag2p, real_dest_p, imag_dest_p,
                frames_to_process);
}

}  // namespace MSA
}  // namespace VectorMath
}  // namespace blink

#endif  // VectorMathMSA_h
