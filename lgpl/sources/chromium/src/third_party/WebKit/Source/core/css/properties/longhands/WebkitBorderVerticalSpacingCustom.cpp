// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/properties/longhands/WebkitBorderVerticalSpacing.h"

#include "core/css/ZoomAdjustedPixelValue.h"
#include "core/css/parser/CSSParserContext.h"
#include "core/css/parser/CSSPropertyParserHelpers.h"

namespace blink {
namespace CSSLonghand {

const CSSValue* WebkitBorderVerticalSpacing::ParseSingleValue(
    CSSParserTokenRange& range,
    const CSSParserContext& context,
    const CSSParserLocalContext&) const {
  return CSSPropertyParserHelpers::ConsumeLength(range, context.Mode(),
                                                 kValueRangeNonNegative);
}

const CSSValue* WebkitBorderVerticalSpacing::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node*,
    bool allow_visited_style) const {
  return ZoomAdjustedPixelValue(style.VerticalBorderSpacing(), style);
}

}  // namespace CSSLonghand
}  // namespace blink
