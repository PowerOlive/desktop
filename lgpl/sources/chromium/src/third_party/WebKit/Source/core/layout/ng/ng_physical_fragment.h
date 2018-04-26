// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NGPhysicalFragment_h
#define NGPhysicalFragment_h

#include "base/memory/scoped_refptr.h"
#include "core/CoreExport.h"
#include "core/layout/ng/geometry/ng_physical_offset.h"
#include "core/layout/ng/geometry/ng_physical_size.h"
#include "core/layout/ng/ng_break_token.h"
#include "platform/geometry/LayoutRect.h"

namespace blink {

class ComputedStyle;
class LayoutObject;
class Node;
struct NGPhysicalOffsetRect;
struct NGPixelSnappedPhysicalBoxStrut;

class NGPhysicalFragment;

struct CORE_EXPORT NGPhysicalFragmentTraits {
  static void Destruct(const NGPhysicalFragment*);
};

// The NGPhysicalFragment contains the output geometry from layout. The
// fragment stores all of its information in the physical coordinate system for
// use by paint, hit-testing etc.
//
// The fragment keeps a pointer back to the LayoutObject which generated it.
// Once we have transitioned fully to LayoutNG it should be a const pointer
// such that paint/hit-testing/etc don't modify it.
//
// Layout code should only access geometry information through the
// NGFragment wrapper classes which transforms information into the logical
// coordinate system.
class CORE_EXPORT NGPhysicalFragment
    : public RefCounted<NGPhysicalFragment, NGPhysicalFragmentTraits> {
 public:
  enum NGFragmentType {
    kFragmentBox = 0,
    kFragmentText = 1,
    kFragmentLineBox = 2
    // When adding new values, make sure the bit size of |type_| is large
    // enough to store.
  };
  enum NGBoxType {
    kNormalBox,
    kAnonymousBox,
    kInlineBlock,
    kFloating,
    kOutOfFlowPositioned,
    // When adding new values, make sure the bit size of |box_type_| is large
    // enough to store.

    // Also, add after kMinimumBlockLayoutRoot if the box type is a block layout
    // root, or before otherwise. See IsBlockLayoutRoot().
    kMinimumBlockLayoutRoot = kInlineBlock
  };

  ~NGPhysicalFragment();

  NGFragmentType Type() const { return static_cast<NGFragmentType>(type_); }
  bool IsContainer() const {
    return Type() == NGFragmentType::kFragmentBox ||
           Type() == NGFragmentType::kFragmentLineBox;
  }
  bool IsBox() const { return Type() == NGFragmentType::kFragmentBox; }
  bool IsText() const { return Type() == NGFragmentType::kFragmentText; }
  bool IsLineBox() const { return Type() == NGFragmentType::kFragmentLineBox; }

  // Returns the box type of this fragment.
  NGBoxType BoxType() const { return static_cast<NGBoxType>(box_type_); }
  // Returns whether the fragment is old layout root.
  bool IsOldLayoutRoot() const { return is_old_layout_root_; }
  // An inline block is represented as a kFragmentBox.
  // TODO(eae): This isn't true for replaces elements at the moment.
  bool IsInlineBlock() const { return BoxType() == NGBoxType::kInlineBlock; }
  bool IsFloating() const { return BoxType() == NGBoxType::kFloating; }
  bool IsOutOfFlowPositioned() const {
    return BoxType() == NGBoxType::kOutOfFlowPositioned;
  }
  bool IsBlockFlow() const;

  // A box fragment that do not exist in LayoutObject tree. Its LayoutObject is
  // co-owned by other fragments.
  bool IsAnonymousBox() const { return BoxType() == NGBoxType::kAnonymousBox; }
  // A block sub-layout starts on this fragment. Inline blocks, floats, out of
  // flow positioned objects are such examples. This is also true on NG/legacy
  // boundary.
  bool IsBlockLayoutRoot() const {
    return BoxType() >= NGBoxType::kMinimumBlockLayoutRoot || IsOldLayoutRoot();
  }

  // |Offset()| is reliable only when this fragment was placed by LayoutNG
  // parent. When the parent is not LayoutNG, the parent may move the
  // |LayoutObject| after this fragment was placed. See comments in
  // |LayoutNGBlockFlow::UpdateBlockLayout()| and crbug.com/788590
  bool IsPlacedByLayoutNG() const;

  // The accessors in this class shouldn't be used by layout code directly,
  // instead should be accessed by the NGFragmentBase classes. These accessors
  // exist for paint, hit-testing, etc.

  // Returns the border-box size.
  NGPhysicalSize Size() const { return size_; }

  // Bitmask for border edges, see NGBorderEdges::Physical.
  unsigned BorderEdges() const { return border_edge_; }
  NGPixelSnappedPhysicalBoxStrut BorderWidths() const;

  // Returns the offset relative to the parent fragment's content-box.
  NGPhysicalOffset Offset() const {
    DCHECK(is_placed_);
    return offset_;
  }

  NGBreakToken* BreakToken() const { return break_token_.get(); }
  const ComputedStyle& Style() const;
  Node* GetNode() const;

  // Whether there is a PaintLayer associated with the fragment.
  bool HasLayer() const;

  // GetLayoutObject should only be used when necessary for compatibility
  // with LegacyLayout.
  LayoutObject* GetLayoutObject() const { return layout_object_; }

  // VisualRect of itself, not including contents, in the local coordinate.
  NGPhysicalOffsetRect SelfVisualRect() const;

  // VisualRect of itself including contents, in the local coordinate.
  NGPhysicalOffsetRect VisualRectWithContents() const;

  // Unite visual rect to propagate to parent's ContentsVisualRect.
  void PropagateContentsVisualRect(NGPhysicalOffsetRect*) const;

  // Should only be used by the parent fragment's layout.
  void SetOffset(NGPhysicalOffset offset) {
    DCHECK(!is_placed_);
    offset_ = offset;
    is_placed_ = true;
  }

  bool IsPlaced() const { return is_placed_; }

  scoped_refptr<NGPhysicalFragment> CloneWithoutOffset() const;

  String ToString() const;

  enum DumpFlag {
    DumpHeaderText = 0x1,
    DumpSubtree = 0x2,
    DumpIndentation = 0x4,
    DumpType = 0x8,
    DumpOffset = 0x10,
    DumpSize = 0x20,
    DumpTextOffsets = 0x40,
    DumpAll = -1
  };
  typedef int DumpFlags;

  String DumpFragmentTree(DumpFlags, unsigned indent = 2) const;

#ifndef NDEBUG
  void ShowFragmentTree() const;
#endif

 protected:
  NGPhysicalFragment(LayoutObject* layout_object,
                     const ComputedStyle& style,
                     NGPhysicalSize size,
                     NGFragmentType type,
                     scoped_refptr<NGBreakToken> break_token = nullptr);

  LayoutObject* layout_object_;
  scoped_refptr<const ComputedStyle> style_;
  NGPhysicalSize size_;
  NGPhysicalOffset offset_;
  scoped_refptr<NGBreakToken> break_token_;

  unsigned type_ : 2;  // NGFragmentType
  unsigned box_type_ : 3;  // NGBoxType
  unsigned is_old_layout_root_ : 1;
  unsigned is_placed_ : 1;
  unsigned border_edge_ : 4;  // NGBorderEdges::Physical

 private:
  friend struct NGPhysicalFragmentTraits;
  void Destroy() const;
};

// Used for return value of traversing fragment tree.
struct CORE_EXPORT NGPhysicalFragmentWithOffset {
  DISALLOW_NEW_EXCEPT_PLACEMENT_NEW();

  scoped_refptr<const NGPhysicalFragment> fragment;
  NGPhysicalOffset offset_to_container_box;

  NGPhysicalOffsetRect RectInContainerBox() const;
};

}  // namespace blink

#endif  // NGPhysicalFragment_h
