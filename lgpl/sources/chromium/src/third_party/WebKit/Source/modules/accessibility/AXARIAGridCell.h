/*
 * Copyright (C) 2009 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef AXARIAGridCell_h
#define AXARIAGridCell_h

#include "base/macros.h"
#include "modules/accessibility/AXTableCell.h"

namespace blink {

class AXObjectCacheImpl;

class AXARIAGridCell final : public AXTableCell {
 private:
  AXARIAGridCell(LayoutObject*, AXObjectCacheImpl&);

 public:
  static AXARIAGridCell* Create(LayoutObject*, AXObjectCacheImpl&);
  ~AXARIAGridCell() override;

  // fills in the start location and row span of cell
  bool RowIndexRange(std::pair<unsigned, unsigned>& row_range) const override;
  // fills in the start location and column span of cell
  bool ColumnIndexRange(
      std::pair<unsigned, unsigned>& column_range) const override;
  AccessibilityRole ScanToDecideHeaderRole() final;
  AXRestriction Restriction() const final;
  bool CanSetSelectedAttribute() const final {
    return Restriction() != kDisabled;
  }

 protected:
  bool IsAriaColumnHeader() const;
  bool IsAriaRowHeader() const;
  AXObject* ParentTable() const override;
  AXObject* ParentRow() const override;

  DISALLOW_COPY_AND_ASSIGN(AXARIAGridCell);
};

}  // namespace blink

#endif  // AXARIAGridCell_h
