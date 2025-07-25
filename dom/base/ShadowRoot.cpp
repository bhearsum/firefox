/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/BindContext.h"
#include "mozilla/dom/ShadowRoot.h"
#include "mozilla/dom/DocumentFragment.h"
#include "ChildIterator.h"
#include "nsContentUtils.h"
#include "nsINode.h"
#include "nsWindowSizes.h"
#include "mozilla/dom/DirectionalityUtils.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/ElementBinding.h"
#include "mozilla/dom/HTMLDetailsElement.h"
#include "mozilla/dom/HTMLSlotElement.h"
#include "mozilla/dom/HTMLSummaryElement.h"
#include "mozilla/dom/MutationObservers.h"
#include "mozilla/dom/Text.h"
#include "mozilla/dom/TreeOrderedArrayInlines.h"
#include "mozilla/dom/TrustedTypeUtils.h"
#include "mozilla/dom/TrustedTypesConstants.h"
#include "mozilla/dom/UnbindContext.h"
#include "mozilla/GlobalStyleSheetCache.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/IdentifierMapEntry.h"
#include "mozilla/PresShell.h"
#include "mozilla/PresShellInlines.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/ServoBindings.h"
#include "mozilla/ServoStyleRuleMap.h"
#include "mozilla/StyleSheet.h"
#include "mozilla/dom/StyleSheetList.h"

using namespace mozilla;
using namespace mozilla::dom;

NS_IMPL_CYCLE_COLLECTION_CLASS(ShadowRoot)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(ShadowRoot, DocumentFragment)
  DocumentOrShadowRoot::Traverse(tmp, cb);
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(ShadowRoot)
  DocumentOrShadowRoot::Unlink(tmp);
NS_IMPL_CYCLE_COLLECTION_UNLINK_END_INHERITED(DocumentFragment)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ShadowRoot)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIContent)
NS_INTERFACE_MAP_END_INHERITING(DocumentFragment)

NS_IMPL_ADDREF_INHERITED(ShadowRoot, DocumentFragment)
NS_IMPL_RELEASE_INHERITED(ShadowRoot, DocumentFragment)

ShadowRoot::ShadowRoot(Element* aElement, ShadowRootMode aMode,
                       Element::DelegatesFocus aDelegatesFocus,
                       SlotAssignmentMode aSlotAssignment,
                       IsClonable aIsClonable, IsSerializable aIsSerializable,
                       Declarative aDeclarative,
                       already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo)
    : DocumentFragment(std::move(aNodeInfo)),
      DocumentOrShadowRoot(this),
      mMode(aMode),
      mDelegatesFocus(aDelegatesFocus),
      mSlotAssignment(aSlotAssignment),
      mIsDetailsShadowTree(aElement->IsHTMLElement(nsGkAtoms::details)),
      mIsAvailableToElementInternals(false),
      mIsDeclarative(aDeclarative),
      mIsClonable(aIsClonable),
      mIsSerializable(aIsSerializable) {
  // nsINode.h relies on this.
  MOZ_ASSERT(static_cast<nsINode*>(this) == reinterpret_cast<nsINode*>(this));
  MOZ_ASSERT(static_cast<nsIContent*>(this) ==
             reinterpret_cast<nsIContent*>(this));

  SetHost(aElement);

  // Nodes in a shadow tree should never store a value
  // in the subtree root pointer, nodes in the shadow tree
  // track the subtree root using GetContainingShadow().
  ClearSubtreeRootPointer();

  SetFlags(NODE_IS_IN_SHADOW_TREE);
  if (Host()->IsInNativeAnonymousSubtree()) {
    // NOTE(emilio): We could consider just propagating the
    // IN_NATIVE_ANONYMOUS_SUBTREE flag (not making this an anonymous root), but
    // that breaks the invariant that if two nodes have the same
    // NativeAnonymousSubtreeRoot() they are in the same DOM tree, which we rely
    // on a couple places and would need extra fixes.
    //
    // We don't hit this case for now anyways, bug 1824886 would start hitting
    // it.
    SetIsNativeAnonymousRoot();
  }
  Bind();

  ExtendedDOMSlots()->mContainingShadow = this;
}

ShadowRoot::~ShadowRoot() {
  if (IsInComposedDoc()) {
    OwnerDoc()->RemoveComposedDocShadowRoot(*this);
  }

  MOZ_DIAGNOSTIC_ASSERT(!OwnerDoc()->IsComposedDocShadowRoot(*this));

  UnsetFlags(NODE_IS_IN_SHADOW_TREE);

  // nsINode destructor expects mSubtreeRoot == this.
  SetSubtreeRootPointer(this);
}

MOZ_DEFINE_MALLOC_SIZE_OF(ShadowRootAuthorStylesMallocSizeOf)
MOZ_DEFINE_MALLOC_ENCLOSING_SIZE_OF(ShadowRootAuthorStylesMallocEnclosingSizeOf)

void ShadowRoot::AddSizeOfExcludingThis(nsWindowSizes& aSizes,
                                        size_t* aNodeSize) const {
  DocumentFragment::AddSizeOfExcludingThis(aSizes, aNodeSize);
  DocumentOrShadowRoot::AddSizeOfExcludingThis(aSizes);
  aSizes.mLayoutShadowDomAuthorStyles += Servo_AuthorStyles_SizeOfIncludingThis(
      ShadowRootAuthorStylesMallocSizeOf,
      ShadowRootAuthorStylesMallocEnclosingSizeOf, mServoStyles.get());
}

JSObject* ShadowRoot::WrapNode(JSContext* aCx,
                               JS::Handle<JSObject*> aGivenProto) {
  return mozilla::dom::ShadowRoot_Binding::Wrap(aCx, this, aGivenProto);
}

void ShadowRoot::NodeInfoChanged(Document* aOldDoc) {
  DocumentFragment::NodeInfoChanged(aOldDoc);
  Document* newDoc = OwnerDoc();
  const bool fromOrToTemplate =
      aOldDoc->GetTemplateContentsOwnerIfExists() == newDoc ||
      newDoc->GetTemplateContentsOwnerIfExists() == aOldDoc;
  if (!fromOrToTemplate) {
    ClearAdoptedStyleSheets();
  }
}

void ShadowRoot::CloneInternalDataFrom(ShadowRoot* aOther) {
  if (aOther->IsRootOfNativeAnonymousSubtree()) {
    SetIsNativeAnonymousRoot();
  }

  if (aOther->IsUAWidget()) {
    SetIsUAWidget();
  }

  CloneAdoptedSheetsFrom(*aOther);
}

nsresult ShadowRoot::Bind() {
  MOZ_ASSERT(!IsInComposedDoc(), "Forgot to unbind?");
  if (Host()->IsInComposedDoc()) {
    SetIsConnected(true);
    Document* doc = OwnerDoc();
    doc->AddComposedDocShadowRoot(*this);
    // If our stylesheets somehow mutated when we were disconnected, we need to
    // ensure that our style data gets flushed as appropriate.
    if (mServoStyles && Servo_AuthorStyles_IsDirty(mServoStyles.get())) {
      doc->RecordShadowStyleChange(*this);
    }
  }

  BindContext context(*this);
  for (nsIContent* child = GetFirstChild(); child;
       child = child->GetNextSibling()) {
    nsresult rv = child->BindToTree(context, *this);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

void ShadowRoot::Unbind() {
  if (IsInComposedDoc()) {
    SetIsConnected(false);
    OwnerDoc()->RemoveComposedDocShadowRoot(*this);
  }

  UnbindContext context(*this);
  for (nsIContent* child = GetFirstChild(); child;
       child = child->GetNextSibling()) {
    child->UnbindFromTree(context);
  }

  MutationObservers::NotifyParentChainChanged(this);
}

void ShadowRoot::Unattach() {
  MOZ_ASSERT(!HasSlots(), "Won't work!");
  if (!GetHost()) {
    // It is possible that we've been unlinked already. In such case host
    // should have called Unbind and ShadowRoot's own unlink.
    return;
  }

  Unbind();
  SetHost(nullptr);
}

void ShadowRoot::InvalidateStyleAndLayoutOnSubtree(Element* aElement) {
  MOZ_ASSERT(aElement);
  Document* doc = GetComposedDoc();
  if (!doc) {
    return;
  }

  if (!aElement->IsInComposedDoc()) {
    // If RemoveSlot is called from UnbindFromTree while we're moving
    // (moveBefore) the slot elsewhere, invalidating styles and layout tree
    // is done explicitly elsewhere.
    return;
  }

  PresShell* presShell = doc->GetPresShell();
  if (!presShell) {
    return;
  }

  presShell->DestroyFramesForAndRestyle(aElement);
}

void ShadowRoot::PartAdded(const Element& aPart) {
  MOZ_ASSERT(aPart.HasPartAttribute());
  MOZ_ASSERT(!mParts.Contains(&aPart));
  mParts.AppendElement(&aPart);
}

void ShadowRoot::PartRemoved(const Element& aPart) {
  MOZ_ASSERT(mParts.Contains(&aPart));
  mParts.RemoveElement(&aPart);
  MOZ_ASSERT(!mParts.Contains(&aPart));
}

void ShadowRoot::AddSlot(HTMLSlotElement* aSlot) {
  MOZ_ASSERT(aSlot);

  // Note that if name attribute missing, the slot is a default slot.
  nsAutoString name;
  aSlot->GetName(name);

  SlotArray& currentSlots = *mSlotMap.GetOrInsertNew(name);

  size_t index = currentSlots.Insert(*aSlot);

  // For Named slots, slottables are inserted into the other slot
  // which has the same name already, however it's not the case
  // for manual slots
  if (index != 0 && SlotAssignment() == SlotAssignmentMode::Named) {
    return;
  }

  InvalidateStyleAndLayoutOnSubtree(aSlot);

  HTMLSlotElement* oldSlot = currentSlots->SafeElementAt(1);
  if (SlotAssignment() == SlotAssignmentMode::Named) {
    if (oldSlot) {
      MOZ_DIAGNOSTIC_ASSERT(oldSlot != aSlot);

      // Move assigned nodes from old slot to new slot.
      InvalidateStyleAndLayoutOnSubtree(oldSlot);
      const nsTArray<RefPtr<nsINode>>& assignedNodes = oldSlot->AssignedNodes();
      bool doEnqueueSlotChange = false;
      while (assignedNodes.Length() > 0) {
        nsINode* assignedNode = assignedNodes[0];

        oldSlot->RemoveAssignedNode(*assignedNode->AsContent());
        aSlot->AppendAssignedNode(*assignedNode->AsContent());
        doEnqueueSlotChange = true;
      }

      if (doEnqueueSlotChange) {
        oldSlot->EnqueueSlotChangeEvent();
        aSlot->EnqueueSlotChangeEvent();
      }
    } else {
      bool doEnqueueSlotChange = false;
      // Otherwise add appropriate nodes to this slot from the host.
      for (nsIContent* child = GetHost()->GetFirstChild(); child;
           child = child->GetNextSibling()) {
        nsAutoString slotName;
        GetSlotNameFor(*child, slotName);
        if (!child->IsSlotable() || !slotName.Equals(name)) {
          continue;
        }
        doEnqueueSlotChange = true;
        aSlot->AppendAssignedNode(*child);
      }

      if (doEnqueueSlotChange) {
        aSlot->EnqueueSlotChangeEvent();
      }
    }
  } else {
    bool doEnqueueSlotChange = false;
    for (const auto& node : aSlot->ManuallyAssignedNodes()) {
      if (GetHost() != node->GetParent()) {
        continue;
      }

      MOZ_ASSERT(node->IsContent(),
                 "Manually assigned nodes should be an element or a text");
      nsIContent* content = node->AsContent();

      aSlot->AppendAssignedNode(*content);
      doEnqueueSlotChange = true;
    }
    if (doEnqueueSlotChange) {
      aSlot->EnqueueSlotChangeEvent();
    }
  }
}

void ShadowRoot::RemoveSlot(HTMLSlotElement* aSlot) {
  MOZ_ASSERT(aSlot);

  nsAutoString name;
  aSlot->GetName(name);

  MOZ_ASSERT(mSlotMap.Get(name));

  SlotArray& currentSlots = *mSlotMap.Get(name);
  MOZ_DIAGNOSTIC_ASSERT(currentSlots->Contains(aSlot),
                        "Slot to de-register wasn't found?");
  if (currentSlots->Length() == 1) {
    MOZ_ASSERT_IF(SlotAssignment() == SlotAssignmentMode::Named,
                  currentSlots->ElementAt(0) == aSlot);

    InvalidateStyleAndLayoutOnSubtree(aSlot);

    mSlotMap.Remove(name);
    if (!aSlot->AssignedNodes().IsEmpty()) {
      aSlot->ClearAssignedNodes();
      aSlot->EnqueueSlotChangeEvent();
    }

    return;
  }
  if (SlotAssignment() == SlotAssignmentMode::Manual) {
    InvalidateStyleAndLayoutOnSubtree(aSlot);
    if (!aSlot->AssignedNodes().IsEmpty()) {
      aSlot->ClearAssignedNodes();
      aSlot->EnqueueSlotChangeEvent();
    }
  }

  const bool wasFirstSlot = currentSlots->ElementAt(0) == aSlot;
  currentSlots.RemoveElement(*aSlot);
  if (!wasFirstSlot || SlotAssignment() == SlotAssignmentMode::Manual) {
    return;
  }

  // Move assigned nodes from removed slot to the next slot in
  // tree order with the same name.
  InvalidateStyleAndLayoutOnSubtree(aSlot);
  HTMLSlotElement* replacementSlot = currentSlots->ElementAt(0);
  const nsTArray<RefPtr<nsINode>>& assignedNodes = aSlot->AssignedNodes();
  if (assignedNodes.IsEmpty()) {
    return;
  }

  InvalidateStyleAndLayoutOnSubtree(replacementSlot);
  while (!assignedNodes.IsEmpty()) {
    nsINode* assignedNode = assignedNodes[0];

    aSlot->RemoveAssignedNode(*assignedNode->AsContent());
    replacementSlot->AppendAssignedNode(*assignedNode->AsContent());
  }

  aSlot->EnqueueSlotChangeEvent();
  replacementSlot->EnqueueSlotChangeEvent();
}

// FIXME(emilio): There's a bit of code duplication between this and the
// equivalent ServoStyleSet methods, it'd be nice to not duplicate it...
void ShadowRoot::RuleAdded(StyleSheet& aSheet, css::Rule& aRule) {
  if (!aSheet.IsApplicable()) {
    return;
  }

  MOZ_ASSERT(mServoStyles);
  if (mStyleRuleMap) {
    mStyleRuleMap->RuleAdded(aSheet, aRule);
  }

  if (aRule.IsIncompleteImportRule()) {
    return;
  }

  Servo_AuthorStyles_ForceDirty(mServoStyles.get());
  ApplicableRulesChanged();
}

void ShadowRoot::RuleRemoved(StyleSheet& aSheet, css::Rule& aRule) {
  if (!aSheet.IsApplicable()) {
    return;
  }

  MOZ_ASSERT(mServoStyles);
  if (mStyleRuleMap) {
    mStyleRuleMap->RuleRemoved(aSheet, aRule);
  }
  Servo_AuthorStyles_ForceDirty(mServoStyles.get());
  ApplicableRulesChanged();
}

void ShadowRoot::RuleChanged(StyleSheet& aSheet, css::Rule*,
                             const StyleRuleChange&) {
  if (!aSheet.IsApplicable()) {
    return;
  }

  MOZ_ASSERT(mServoStyles);
  Servo_AuthorStyles_ForceDirty(mServoStyles.get());
  ApplicableRulesChanged();
}

void ShadowRoot::ImportRuleLoaded(StyleSheet& aSheet) {
  if (mStyleRuleMap) {
    mStyleRuleMap->SheetAdded(aSheet);
  }

  if (!aSheet.IsApplicable()) {
    return;
  }

  // TODO(emilio): Could handle it like a regular sheet insertion, I guess, to
  // avoid throwing away the whole style data.
  Servo_AuthorStyles_ForceDirty(mServoStyles.get());
  ApplicableRulesChanged();
}

// We don't need to do anything else than forwarding to the document if
// necessary.
void ShadowRoot::SheetCloned(StyleSheet& aSheet) {
  if (Document* doc = GetComposedDoc()) {
    if (PresShell* shell = doc->GetPresShell()) {
      shell->StyleSet()->SheetCloned(aSheet);
    }
  }
}

void ShadowRoot::ApplicableRulesChanged() {
  if (Document* doc = GetComposedDoc()) {
    doc->RecordShadowStyleChange(*this);
  }
}

void ShadowRoot::InsertSheetAt(size_t aIndex, StyleSheet& aSheet) {
  DocumentOrShadowRoot::InsertSheetAt(aIndex, aSheet);
  if (aSheet.IsApplicable()) {
    InsertSheetIntoAuthorData(aIndex, aSheet, mStyleSheets);
  }
}

StyleSheet* FirstApplicableAdoptedStyleSheet(
    const nsTArray<RefPtr<StyleSheet>>& aList) {
  size_t i = 0;
  for (StyleSheet* sheet : aList) {
    // Deal with duplicate sheets by only considering the last one.
    if (sheet->IsApplicable() && MOZ_LIKELY(aList.LastIndexOf(sheet) == i)) {
      return sheet;
    }
    i++;
  }
  return nullptr;
}

void ShadowRoot::InsertSheetIntoAuthorData(
    size_t aIndex, StyleSheet& aSheet,
    const nsTArray<RefPtr<StyleSheet>>& aList) {
  MOZ_ASSERT(aSheet.IsApplicable());
  MOZ_ASSERT(aList[aIndex] == &aSheet);
  MOZ_ASSERT(aList.LastIndexOf(&aSheet) == aIndex);
  MOZ_ASSERT(&aList == &mAdoptedStyleSheets || &aList == &mStyleSheets);

  if (!mServoStyles) {
    mServoStyles.reset(Servo_AuthorStyles_Create());
  }

  if (mStyleRuleMap) {
    mStyleRuleMap->SheetAdded(aSheet);
  }

  auto changedOnExit =
      mozilla::MakeScopeExit([&] { ApplicableRulesChanged(); });

  for (size_t i = aIndex + 1; i < aList.Length(); ++i) {
    StyleSheet* beforeSheet = aList.ElementAt(i);
    if (!beforeSheet->IsApplicable()) {
      continue;
    }

    // If this is a duplicate adopted stylesheet that is not in the right
    // position (the last one) then we skip over it. Otherwise we're done.
    if (&aList == &mAdoptedStyleSheets &&
        MOZ_UNLIKELY(aList.LastIndexOf(beforeSheet) != i)) {
      continue;
    }

    Servo_AuthorStyles_InsertStyleSheetBefore(mServoStyles.get(), &aSheet,
                                              beforeSheet);
    return;
  }

  if (mAdoptedStyleSheets.IsEmpty() || &aList == &mAdoptedStyleSheets) {
    Servo_AuthorStyles_AppendStyleSheet(mServoStyles.get(), &aSheet);
    return;
  }

  if (auto* before = FirstApplicableAdoptedStyleSheet(mAdoptedStyleSheets)) {
    Servo_AuthorStyles_InsertStyleSheetBefore(mServoStyles.get(), &aSheet,
                                              before);
  } else {
    Servo_AuthorStyles_AppendStyleSheet(mServoStyles.get(), &aSheet);
  }
}

// FIXME(emilio): This needs to notify document observers and such,
// presumably.
void ShadowRoot::StyleSheetApplicableStateChanged(StyleSheet& aSheet) {
  auto& sheetList = aSheet.IsConstructed() ? mAdoptedStyleSheets : mStyleSheets;
  size_t index = sheetList.LastIndexOf(&aSheet);
  if (index == sheetList.NoIndex) {
    // NOTE(emilio): @import sheets are handled in the relevant RuleAdded
    // notification, which only notifies after the sheet is loaded.
    //
    // This setup causes weirdness in other places, we may want to fix this in
    // bug 1465031.
    MOZ_DIAGNOSTIC_ASSERT(aSheet.GetParentSheet(),
                          "It'd better be an @import sheet");
    return;
  }
  if (aSheet.IsApplicable()) {
    InsertSheetIntoAuthorData(index, aSheet, sheetList);
  } else {
    MOZ_ASSERT(mServoStyles);
    if (mStyleRuleMap) {
      mStyleRuleMap->SheetRemoved(aSheet);
    }
    Servo_AuthorStyles_RemoveStyleSheet(mServoStyles.get(), &aSheet);
    ApplicableRulesChanged();
  }
}

void ShadowRoot::AppendBuiltInStyleSheet(BuiltInStyleSheet aSheet) {
  auto* cache = GlobalStyleSheetCache::Singleton();
  // NOTE(emilio): It's important to Clone() the stylesheet to avoid leaking,
  // since the built-in sheet is kept alive forever, and AppendStyleSheet will
  // set the associated global of the stylesheet.
  RefPtr sheet = cache->BuiltInSheet(aSheet)->Clone(nullptr, nullptr);
  AppendStyleSheet(*sheet);
}

void ShadowRoot::RemoveSheetFromStyles(StyleSheet& aSheet) {
  MOZ_ASSERT(aSheet.IsApplicable());
  MOZ_ASSERT(mServoStyles);
  if (mStyleRuleMap) {
    mStyleRuleMap->SheetRemoved(aSheet);
  }
  Servo_AuthorStyles_RemoveStyleSheet(mServoStyles.get(), &aSheet);
  ApplicableRulesChanged();
}

void ShadowRoot::AddToIdTable(Element* aElement, nsAtom* aId) {
  IdentifierMapEntry* entry = mIdentifierMap.PutEntry(aId);
  if (entry) {
    entry->AddIdElement(aElement);
  }
}

void ShadowRoot::RemoveFromIdTable(Element* aElement, nsAtom* aId) {
  IdentifierMapEntry* entry = mIdentifierMap.GetEntry(aId);
  if (entry) {
    entry->RemoveIdElement(aElement);
    if (entry->IsEmpty()) {
      mIdentifierMap.RemoveEntry(entry);
    }
  }
}

void ShadowRoot::GetEventTargetParent(EventChainPreVisitor& aVisitor) {
  aVisitor.mCanHandle = true;
  aVisitor.mRootOfClosedTree = IsClosed();
  // Inform that we're about to exit the current scope.
  aVisitor.mRelatedTargetRetargetedInCurrentScope = false;

  // https://dom.spec.whatwg.org/#ref-for-get-the-parent%E2%91%A6
  if (!aVisitor.mEvent->mFlags.mComposed) {
    nsIContent* originalTarget =
        nsIContent::FromEventTargetOrNull(aVisitor.mEvent->mOriginalTarget);
    if (originalTarget && originalTarget->GetContainingShadow() == this) {
      // If we do stop propagation, we still want to propagate
      // the event to chrome (nsPIDOMWindow::GetParentTarget()).
      // The load event is special in that we don't ever propagate it
      // to chrome.
      nsPIDOMWindowOuter* win = OwnerDoc()->GetWindow();
      EventTarget* parentTarget = win && aVisitor.mEvent->mMessage != eLoad
                                      ? win->GetParentTarget()
                                      : nullptr;

      aVisitor.SetParentTarget(parentTarget, true);
      return;
    }
  }

  nsIContent* shadowHost = GetHost();
  aVisitor.SetParentTarget(shadowHost, false);

  nsIContent* content =
      nsIContent::FromEventTargetOrNull(aVisitor.mEvent->mTarget);
  if (content && content->GetContainingShadow() == this) {
    aVisitor.mEventTargetAtParent = shadowHost;
  }
}

void ShadowRoot::GetSlotNameFor(const nsIContent& aContent,
                                nsAString& aName) const {
  if (mIsDetailsShadowTree) {
    const auto* summary = HTMLSummaryElement::FromNode(aContent);
    if (summary && summary->IsMainSummary()) {
      aName.AssignLiteral("internal-main-summary");
    }
    // Otherwise use the default slot.
    return;
  }

  // Note that if slot attribute is missing, assign it to the first default
  // slot, if exists.
  if (const Element* element = Element::FromNode(aContent)) {
    element->GetAttr(nsGkAtoms::slot, aName);
  }
}

ShadowRoot::SlotInsertionPoint ShadowRoot::SlotInsertionPointFor(
    nsIContent& aContent) {
  HTMLSlotElement* slot = nullptr;

  if (SlotAssignment() == SlotAssignmentMode::Manual) {
    slot = aContent.GetManualSlotAssignment();
    if (!slot || slot->GetContainingShadow() != this) {
      return {};
    }
  } else {
    nsAutoString slotName;
    GetSlotNameFor(aContent, slotName);

    SlotArray* slots = mSlotMap.Get(slotName);
    if (!slots) {
      return {};
    }
    slot = (*slots)->ElementAt(0);
  }

  MOZ_ASSERT(slot);

  if (SlotAssignment() == SlotAssignmentMode::Named) {
    if (!aContent.GetNextSibling()) {
      // aContent is the last child, no need to loop through the assigned nodes,
      // we're necessarily the last one.
      //
      // This prevents multiple appends into the host from getting quadratic.
      return {slot, Nothing()};
    }
  } else {
    // For manual slots, if aContent is the last element, we return Nothing
    // because we just need to append the element to the assigned nodes. No need
    // to return an index.
    if (slot->ManuallyAssignedNodes().SafeLastElement(nullptr) == &aContent) {
      return {slot, Nothing()};
    }
  }

  // Find the appropriate position in the assigned node list for the newly
  // assigned content.
  if (SlotAssignment() == SlotAssignmentMode::Manual) {
    const nsTArray<nsINode*>& manuallyAssignedNodes =
        slot->ManuallyAssignedNodes();
    auto index = manuallyAssignedNodes.IndexOf(&aContent);
    if (index != manuallyAssignedNodes.NoIndex) {
      return {slot, Some(index)};
    }
  } else {
    const nsTArray<RefPtr<nsINode>>& assignedNodes = slot->AssignedNodes();
    nsIContent* currentContent = GetHost()->GetFirstChild();
    for (uint32_t i = 0; i < assignedNodes.Length(); i++) {
      // Seek through the host's explicit children until the
      // assigned content is found.
      while (currentContent && currentContent != assignedNodes[i]) {
        if (currentContent == &aContent) {
          return {slot, Some(i)};
        }
        currentContent = currentContent->GetNextSibling();
      }
    }
  }

  return {slot, Nothing()};
}

void ShadowRoot::MaybeReassignContent(nsIContent& aElementOrText) {
  MOZ_ASSERT(aElementOrText.GetParent() == GetHost());
  MOZ_ASSERT(aElementOrText.IsElement() || aElementOrText.IsText());
  HTMLSlotElement* oldSlot = aElementOrText.GetAssignedSlot();

  SlotInsertionPoint assignment = SlotInsertionPointFor(aElementOrText);

  if (assignment.mSlot == oldSlot) {
    // Nothing to do here.
    return;
  }

  // The layout invalidation piece for Manual slots is handled in
  // HTMLSlotElement::Assign
  if (aElementOrText.IsElement() &&
      SlotAssignment() == SlotAssignmentMode::Named) {
    if (Document* doc = GetComposedDoc()) {
      if (RefPtr<PresShell> presShell = doc->GetPresShell()) {
        presShell->SlotAssignmentWillChange(*aElementOrText.AsElement(),
                                            oldSlot, assignment.mSlot);
      }
    }
  }

  if (oldSlot) {
    if (SlotAssignment() == SlotAssignmentMode::Named) {
      oldSlot->RemoveAssignedNode(aElementOrText);
      // Don't need to EnqueueSlotChangeEvent for Manual slots because it
      // needs to be done in tree order, so
      // HTMLSlotElement::Assign will handle it explicitly.
      oldSlot->EnqueueSlotChangeEvent();
    } else {
      oldSlot->RemoveManuallyAssignedNode(aElementOrText);
    }
  }

  if (assignment.mSlot) {
    if (assignment.mIndex) {
      assignment.mSlot->InsertAssignedNode(*assignment.mIndex, aElementOrText);
    } else {
      assignment.mSlot->AppendAssignedNode(aElementOrText);
    }
    // Similar as above, HTMLSlotElement::Assign handles enqueuing
    // slotchange event.
    if (SlotAssignment() == SlotAssignmentMode::Named) {
      assignment.mSlot->EnqueueSlotChangeEvent();
    }
  }
}

void ShadowRoot::MaybeReassignMainSummary(SummaryChangeReason aReason) {
  MOZ_ASSERT(mIsDetailsShadowTree);
  if (aReason == SummaryChangeReason::Insertion) {
    // We've inserted a summary element, may need to remove the existing one.
    SlotArray* array = mSlotMap.Get(u"internal-main-summary"_ns);
    MOZ_RELEASE_ASSERT(array && (*array)->Length() == 1);
    HTMLSlotElement* slot = (*array)->ElementAt(0);
    auto* summary = HTMLSummaryElement::FromNodeOrNull(
        slot->AssignedNodes().SafeElementAt(0));
    if (summary) {
      MaybeReassignContent(*summary);
    }
  } else if (MOZ_LIKELY(GetHost())) {
    // We need to null-check GetHost() in case we're unlinking already.
    auto* details = HTMLDetailsElement::FromNode(Host());
    MOZ_DIAGNOSTIC_ASSERT(details);
    // We've removed a summary element, we may need to assign the new one.
    if (HTMLSummaryElement* newMainSummary = details->GetFirstSummary()) {
      MaybeReassignContent(*newMainSummary);
    }
  }
}

Element* ShadowRoot::GetActiveElement() {
  return GetRetargetedFocusedElement();
}

nsINode* ShadowRoot::ImportNodeAndAppendChildAt(nsINode& aParentNode,
                                                nsINode& aNode, bool aDeep,
                                                mozilla::ErrorResult& rv) {
  MOZ_ASSERT(IsUAWidget());

  if (aParentNode.SubtreeRoot() != this) {
    rv.Throw(NS_ERROR_INVALID_ARG);
    return nullptr;
  }

  RefPtr<nsINode> node = OwnerDoc()->ImportNode(aNode, aDeep, rv);
  if (rv.Failed()) {
    return nullptr;
  }

  return aParentNode.AppendChild(*node, rv);
}

nsINode* ShadowRoot::CreateElementAndAppendChildAt(nsINode& aParentNode,
                                                   const nsAString& aTagName,
                                                   mozilla::ErrorResult& rv) {
  MOZ_ASSERT(IsUAWidget());

  if (aParentNode.SubtreeRoot() != this) {
    rv.Throw(NS_ERROR_INVALID_ARG);
    return nullptr;
  }

  // This option is not exposed to UA Widgets
  ElementCreationOptionsOrString options;

  RefPtr<nsINode> node = OwnerDoc()->CreateElement(aTagName, options, rv);
  if (rv.Failed()) {
    return nullptr;
  }

  return aParentNode.AppendChild(*node, rv);
}

void ShadowRoot::MaybeUnslotHostChild(nsIContent& aChild) {
  // Need to null-check the host because we may be unlinked already.
  MOZ_ASSERT(!GetHost() || aChild.GetParent() == GetHost());

  HTMLSlotElement* slot = aChild.GetAssignedSlot();
  if (!slot) {
    return;
  }

  MOZ_DIAGNOSTIC_ASSERT(!aChild.IsRootOfNativeAnonymousSubtree(),
                        "How did aChild end up assigned to a slot?");
  // If the slot is going to start showing fallback content, we need to tell
  // layout about it.
  if (slot->AssignedNodes().Length() == 1 && slot->HasChildren()) {
    InvalidateStyleAndLayoutOnSubtree(slot);
  }

  slot->RemoveAssignedNode(aChild);
  slot->EnqueueSlotChangeEvent();

  if (mIsDetailsShadowTree && aChild.IsHTMLElement(nsGkAtoms::summary)) {
    MaybeReassignMainSummary(SummaryChangeReason::Deletion);
  }
}

void ShadowRoot::MaybeSlotHostChild(nsIContent& aChild) {
  MOZ_ASSERT(aChild.GetParent() == GetHost());
  // Check to ensure that the child not an anonymous subtree root because even
  // though its parent could be the host it may not be in the host's child
  // list.
  if (aChild.IsRootOfNativeAnonymousSubtree()) {
    return;
  }

  if (!aChild.IsSlotable()) {
    return;
  }

  if (mIsDetailsShadowTree && aChild.IsHTMLElement(nsGkAtoms::summary)) {
    MaybeReassignMainSummary(SummaryChangeReason::Insertion);
  }

  SlotInsertionPoint assignment = SlotInsertionPointFor(aChild);
  if (!assignment.mSlot) {
    return;
  }

  // Fallback content will go away, let layout know.
  if (assignment.mSlot->AssignedNodes().IsEmpty() &&
      assignment.mSlot->HasChildren()) {
    InvalidateStyleAndLayoutOnSubtree(assignment.mSlot);
  }

  if (assignment.mIndex) {
    assignment.mSlot->InsertAssignedNode(*assignment.mIndex, aChild);
  } else {
    assignment.mSlot->AppendAssignedNode(aChild);
  }
  assignment.mSlot->EnqueueSlotChangeEvent();
}

ServoStyleRuleMap& ShadowRoot::ServoStyleRuleMap() {
  if (!mStyleRuleMap) {
    mStyleRuleMap = MakeUnique<mozilla::ServoStyleRuleMap>();
  }
  mStyleRuleMap->EnsureTable(*this);
  return *mStyleRuleMap;
}

nsresult ShadowRoot::Clone(dom::NodeInfo* aNodeInfo, nsINode** aResult) const {
  *aResult = nullptr;
  return NS_ERROR_DOM_NOT_SUPPORTED_ERR;
}

void ShadowRoot::SetHTML(const nsAString& aHTML, const SetHTMLOptions& aOptions,
                         ErrorResult& aError) {
  RefPtr<Element> host = GetHost();
  nsContentUtils::SetHTML(this, host, aHTML, aOptions, aError);
}

void ShadowRoot::SetHTMLUnsafe(const TrustedHTMLOrString& aHTML,
                               const SetHTMLUnsafeOptions& aOptions,
                               nsIPrincipal* aSubjectPrincipal,
                               ErrorResult& aError) {
  RefPtr<Element> host = GetHost();
  nsContentUtils::SetHTMLUnsafe(this, host, aHTML, aOptions,
                                true /*aIsShadowRoot*/, aSubjectPrincipal,
                                aError);
}

void ShadowRoot::GetInnerHTML(
    OwningTrustedHTMLOrNullIsEmptyString& aInnerHTML) {
  DocumentFragment::GetInnerHTML(aInnerHTML.SetAsNullIsEmptyString());
}

MOZ_CAN_RUN_SCRIPT void ShadowRoot::SetInnerHTML(
    const TrustedHTMLOrNullIsEmptyString& aInnerHTML,
    nsIPrincipal* aSubjectPrincipal, ErrorResult& aError) {
  constexpr nsLiteralString sink = u"ShadowRoot innerHTML"_ns;

  Maybe<nsAutoString> compliantStringHolder;
  const nsAString* compliantString =
      TrustedTypeUtils::GetTrustedTypesCompliantString(
          aInnerHTML, sink, kTrustedTypesOnlySinkGroup, *this,
          aSubjectPrincipal, compliantStringHolder, aError);
  if (aError.Failed()) {
    return;
  }

  SetInnerHTMLInternal(*compliantString, aError);
}

void ShadowRoot::GetHTML(const GetHTMLOptions& aOptions, nsAString& aResult) {
  nsContentUtils::SerializeNodeToMarkup<SerializeShadowRoots::Yes>(
      this, true, aResult, aOptions.mSerializableShadowRoots,
      aOptions.mShadowRoots);
}
