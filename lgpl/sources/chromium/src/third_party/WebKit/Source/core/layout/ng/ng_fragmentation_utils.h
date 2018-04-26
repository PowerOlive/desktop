// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NGFragmentationUtils_h
#define NGFragmentationUtils_h

#include "core/layout/ng/ng_layout_input_node.h"
#include "core/style/ComputedStyleConstants.h"
#include "platform/LayoutUnit.h"

namespace blink {

class NGConstraintSpace;
class NGPhysicalFragment;

// Return the total amount of block space spent on a node by fragments
// preceding this one (but not including this one).
LayoutUnit PreviouslyUsedBlockSpace(const NGConstraintSpace&,
                                    const NGPhysicalFragment&);

// Return true if the specified fragment is the first generated fragment of
// some node.
bool IsFirstFragment(const NGConstraintSpace&, const NGPhysicalFragment&);

// Return true if the specified fragment is the final fragment of some node.
bool IsLastFragment(const NGPhysicalFragment&);

// Join two adjacent break values specified on break-before and/or break-
// after. avoid* values win over auto values, and forced break values win over
// avoid* values. |first_value| is specified on an element earlier in the flow
// than |second_value|. This method is used at class A break points [1], to join
// the values of the previous break-after and the next break-before, to figure
// out whether we may, must, or should not break at that point. It is also used
// when propagating break-before values from first children and break-after
// values on last children to their container.
//
// [1] https://drafts.csswg.org/css-break/#possible-breaks
EBreakBetween JoinFragmentainerBreakValues(EBreakBetween first_value,
                                           EBreakBetween second_value);

// Return true if the specified break value has a forced break effect in the
// current fragmentation context.
bool IsForcedBreakValue(const NGConstraintSpace&, EBreakBetween);

// Return true if we are to ignore the block-start margin of the child. At the
// start of fragmentainers, in-flow block-start margins are ignored, unless
// we're right after a forced break.
// https://drafts.csswg.org/css-break/#break-margins
bool ShouldIgnoreBlockStartMargin(const NGConstraintSpace&,
                                  NGLayoutInputNode,
                                  const NGBreakToken*);

}  // namespace blink

#endif  // NGFragmentationUtils_h
