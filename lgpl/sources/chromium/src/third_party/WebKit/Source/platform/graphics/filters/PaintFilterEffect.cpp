// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/graphics/filters/PaintFilterEffect.h"

#include "platform/graphics/filters/Filter.h"
#include "platform/text/TextStream.h"
#include "third_party/skia/include/effects/SkPaintImageFilter.h"

namespace blink {

PaintFilterEffect::PaintFilterEffect(Filter* filter, const PaintFlags& flags)
    : FilterEffect(filter), flags_(flags) {
  SetOperatingInterpolationSpace(kInterpolationSpaceSRGB);
}

PaintFilterEffect::~PaintFilterEffect() = default;

PaintFilterEffect* PaintFilterEffect::Create(Filter* filter,
                                             const PaintFlags& flags) {
  return new PaintFilterEffect(filter, flags);
}

sk_sp<PaintFilter> PaintFilterEffect::CreateImageFilter() {
  return sk_make_sp<PaintFlagsPaintFilter>(flags_);
}

TextStream& PaintFilterEffect::ExternalRepresentation(TextStream& ts,
                                                      int indent) const {
  WriteIndent(ts, indent);
  ts << "[PaintFilterEffect]\n";
  return ts;
}

}  // namespace blink
