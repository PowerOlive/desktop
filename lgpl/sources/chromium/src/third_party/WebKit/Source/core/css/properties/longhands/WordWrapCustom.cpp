// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/properties/longhands/WordWrap.h"

namespace blink {
namespace CSSLonghand {

const CSSValue* WordWrap::CSSValueFromComputedStyleInternal(
    const ComputedStyle& style,
    const SVGComputedStyle&,
    const LayoutObject*,
    Node*,
    bool allow_visited_style) const {
  return CSSIdentifierValue::Create(style.OverflowWrap());
}

}  // namespace CSSLonghand
}  // namespace blink
