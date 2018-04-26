// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/cssom/CSSUnitValue.h"

#include "bindings/core/v8/ExceptionState.h"
#include "core/css/CSSCalculationValue.h"
#include "core/css/CSSResolutionUnits.h"
#include "core/css/cssom/CSSMathInvert.h"
#include "core/css/cssom/CSSMathMax.h"
#include "core/css/cssom/CSSMathMin.h"
#include "core/css/cssom/CSSMathProduct.h"
#include "core/css/cssom/CSSMathSum.h"
#include "core/css/cssom/CSSNumericSumValue.h"
#include "platform/wtf/MathExtras.h"

namespace blink {

namespace {

CSSPrimitiveValue::UnitType ToCanonicalUnit(CSSPrimitiveValue::UnitType unit) {
  return CSSPrimitiveValue::CanonicalUnitTypeForCategory(
      CSSPrimitiveValue::UnitTypeToUnitCategory(unit));
}

CSSPrimitiveValue::UnitType ToCanonicalUnitIfPossible(
    CSSPrimitiveValue::UnitType unit) {
  const auto canonical_unit = ToCanonicalUnit(unit);
  if (canonical_unit == CSSPrimitiveValue::UnitType::kUnknown)
    return unit;
  return canonical_unit;
}

}  // namespace

CSSUnitValue* CSSUnitValue::Create(double value,
                                   const String& unit_name,
                                   ExceptionState& exception_state) {
  CSSPrimitiveValue::UnitType unit = UnitFromName(unit_name);
  if (!IsValidUnit(unit)) {
    exception_state.ThrowTypeError("Invalid unit: " + unit_name);
    return nullptr;
  }
  return new CSSUnitValue(value, unit);
}

CSSUnitValue* CSSUnitValue::Create(double value,
                                   CSSPrimitiveValue::UnitType unit) {
  DCHECK(IsValidUnit(unit));
  return new CSSUnitValue(value, unit);
}

CSSUnitValue* CSSUnitValue::FromCSSValue(const CSSPrimitiveValue& value) {
  CSSPrimitiveValue::UnitType unit = value.TypeWithCalcResolved();
  if (unit == CSSPrimitiveValue::UnitType::kInteger)
    unit = CSSPrimitiveValue::UnitType::kNumber;

  if (!IsValidUnit(unit))
    return nullptr;
  return new CSSUnitValue(value.GetDoubleValue(), unit);
}

void CSSUnitValue::setUnit(const String& unit_name,
                           ExceptionState& exception_state) {
  CSSPrimitiveValue::UnitType unit = UnitFromName(unit_name);
  if (!IsValidUnit(unit)) {
    exception_state.ThrowTypeError("Invalid unit: " + unit_name);
    return;
  }

  unit_ = unit;
}

String CSSUnitValue::unit() const {
  if (unit_ == CSSPrimitiveValue::UnitType::kNumber)
    return "number";
  if (unit_ == CSSPrimitiveValue::UnitType::kPercentage)
    return "percent";
  return CSSPrimitiveValue::UnitTypeToString(unit_);
}

CSSStyleValue::StyleValueType CSSUnitValue::GetType() const {
  if (unit_ == CSSPrimitiveValue::UnitType::kNumber)
    return StyleValueType::kNumberType;
  if (unit_ == CSSPrimitiveValue::UnitType::kPercentage)
    return StyleValueType::kPercentType;
  if (CSSPrimitiveValue::IsLength(unit_))
    return StyleValueType::kLengthType;
  if (CSSPrimitiveValue::IsAngle(unit_))
    return StyleValueType::kAngleType;
  if (CSSPrimitiveValue::IsTime(unit_))
    return StyleValueType::kTimeType;
  if (CSSPrimitiveValue::IsFrequency(unit_))
    return StyleValueType::kFrequencyType;
  if (CSSPrimitiveValue::IsResolution(unit_))
    return StyleValueType::kResolutionType;
  if (CSSPrimitiveValue::IsFlex(unit_))
    return StyleValueType::kFlexType;
  NOTREACHED();
  return StyleValueType::kUnknownType;
}

CSSUnitValue* CSSUnitValue::ConvertTo(
    CSSPrimitiveValue::UnitType target_unit) const {
  if (unit_ == target_unit)
    return Create(value_, unit_);

  // Instead of defining the scale factors for every unit to every other unit,
  // we simply convert to the canonical unit and back since we already have
  // the scale factors for canonical units.
  const auto canonical_unit = ToCanonicalUnit(unit_);
  if (canonical_unit != ToCanonicalUnit(target_unit) ||
      canonical_unit == CSSPrimitiveValue::UnitType::kUnknown)
    return nullptr;

  const double scale_factor =
      CSSPrimitiveValue::ConversionToCanonicalUnitsScaleFactor(unit_) /
      CSSPrimitiveValue::ConversionToCanonicalUnitsScaleFactor(target_unit);

  return CSSUnitValue::Create(value_ * scale_factor, target_unit);
}

WTF::Optional<CSSNumericSumValue> CSSUnitValue::SumValue() const {
  CSSNumericSumValue sum;
  CSSNumericSumValue::UnitMap unit_map;
  if (unit_ != CSSPrimitiveValue::UnitType::kNumber)
    unit_map.insert(ToCanonicalUnitIfPossible(unit_), 1);

  sum.terms.emplace_back(
      value_ * CSSPrimitiveValue::ConversionToCanonicalUnitsScaleFactor(unit_),
      std::move(unit_map));
  return sum;
}

bool CSSUnitValue::Equals(const CSSNumericValue& other) const {
  if (!other.IsUnitValue())
    return false;

  const CSSUnitValue& other_unit_value = ToCSSUnitValue(other);
  return value_ == other_unit_value.value_ && unit_ == other_unit_value.unit_;
}

const CSSPrimitiveValue* CSSUnitValue::ToCSSValue() const {
  return CSSPrimitiveValue::Create(value_, unit_);
}

CSSCalcExpressionNode* CSSUnitValue::ToCalcExpressionNode() const {
  return CSSCalcValue::CreateExpressionNode(
      CSSPrimitiveValue::Create(value_, unit_));
}

CSSNumericValue* CSSUnitValue::Negate() {
  return CSSUnitValue::Create(-value_, unit_);
}

CSSNumericValue* CSSUnitValue::Invert() {
  if (unit_ == CSSPrimitiveValue::UnitType::kNumber)
    return CSSUnitValue::Create(1.0 / value_, unit_);
  return CSSMathInvert::Create(this);
}

}  // namespace blink
