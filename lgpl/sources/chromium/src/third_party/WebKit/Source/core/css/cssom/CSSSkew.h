// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CSSSkew_h
#define CSSSkew_h

#include "base/macros.h"
#include "core/css/cssom/CSSNumericValue.h"
#include "core/css/cssom/CSSTransformComponent.h"

namespace blink {

class DOMMatrix;
class ExceptionState;

// Represents a skew value in a CSSTransformValue used for properties like
// "transform".
// See CSSSkew.idl for more information about this class.
class CORE_EXPORT CSSSkew final : public CSSTransformComponent {
  DEFINE_WRAPPERTYPEINFO();

 public:
  // Constructor defined in the IDL.
  static CSSSkew* Create(CSSNumericValue*, CSSNumericValue*, ExceptionState&);
  static CSSSkew* Create(CSSNumericValue* ax, CSSNumericValue* ay) {
    return new CSSSkew(ax, ay);
  }

  // Internal ways of creating CSSSkews.
  static CSSSkew* FromCSSValue(const CSSFunctionValue&);

  // Getters and setters for the ax and ay attributes defined in the IDL.
  CSSNumericValue* ax() { return ax_.Get(); }
  CSSNumericValue* ay() { return ay_.Get(); }
  void setAx(CSSNumericValue*, ExceptionState&);
  void setAy(CSSNumericValue*, ExceptionState&);

  // From CSSTransformComponent
  // Setting is2D for CSSSkew does nothing.
  // https://drafts.css-houdini.org/css-typed-om/#dom-cssskew-is2d
  void setIs2D(bool is2D) final {}

  // Internal methods - from CSSTransformComponent.
  const DOMMatrix* AsMatrix(ExceptionState&) const override;
  TransformComponentType GetType() const override { return kSkewType; }
  const CSSFunctionValue* ToCSSValue() const override;

  virtual void Trace(blink::Visitor* visitor) {
    visitor->Trace(ax_);
    visitor->Trace(ay_);
    CSSTransformComponent::Trace(visitor);
  }

 private:
  CSSSkew(CSSNumericValue* ax, CSSNumericValue* ay);

  Member<CSSNumericValue> ax_;
  Member<CSSNumericValue> ay_;
  DISALLOW_COPY_AND_ASSIGN(CSSSkew);
};

}  // namespace blink

#endif
