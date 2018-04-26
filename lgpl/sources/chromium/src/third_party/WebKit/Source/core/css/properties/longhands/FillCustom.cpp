// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/properties/longhands/Fill.h"

#include "core/css/properties/CSSParsingUtils.h"
#include "core/css/properties/ComputedStyleUtils.h"
#include "core/style/ComputedStyle.h"

namespace blink {
namespace CSSLonghand {

const CSSValue* Fill::ParseSingleValue(CSSParserTokenRange& range,
                                       const CSSParserContext& context,
                                       const CSSParserLocalContext&) const {
  return CSSParsingUtils::ParsePaintStroke(range, context);
}

const CSSValue* Fill::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle& svg_style,
    const LayoutObject*,
    Node* styled_node,
    bool allow_visited_style) const {
  return ComputedStyleUtils::AdjustSVGPaintForCurrentColor(
      svg_style.FillPaintType(), svg_style.FillPaintUri(),
      svg_style.FillPaintColor(), style.GetColor());
}

}  // namespace CSSLonghand
}  // namespace blink
