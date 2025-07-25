/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * nsBaseContentList is a basic list of content nodes; nsContentList
 * is a commonly used NodeList implementation (used for
 * getElementsByTagName, some properties on HTMLDocument/Document, etc).
 */

#include "nsContentList.h"
#include "nsIContent.h"
#include "mozilla/dom/Document.h"
#include "mozilla/ContentIterator.h"
#include "mozilla/dom/Element.h"
#include "nsWrapperCacheInlines.h"
#include "nsContentUtils.h"
#include "nsCCUncollectableMarker.h"
#include "nsGkAtoms.h"
#include "mozilla/dom/HTMLCollectionBinding.h"
#include "mozilla/dom/NodeListBinding.h"
#include "mozilla/Likely.h"
#include "nsGenericHTMLElement.h"
#include "jsfriendapi.h"
#include <algorithm>
#include "mozilla/dom/NodeInfoInlines.h"
#include "mozilla/MruCache.h"
#include "mozilla/StaticPtr.h"

#include "PLDHashTable.h"
#include "nsTHashtable.h"

#ifdef DEBUG_CONTENT_LIST
#  define ASSERT_IN_SYNC AssertInSync()
#else
#  define ASSERT_IN_SYNC PR_BEGIN_MACRO PR_END_MACRO
#endif

using namespace mozilla;
using namespace mozilla::dom;

nsBaseContentList::~nsBaseContentList() = default;

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(nsBaseContentList)
NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(nsBaseContentList)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mElements)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER
  tmp->RemoveFromCaches();
NS_IMPL_CYCLE_COLLECTION_UNLINK_END
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(nsBaseContentList)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mElements)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_BEGIN(nsBaseContentList)
  if (nsCCUncollectableMarker::sGeneration && tmp->HasKnownLiveWrapper()) {
    for (uint32_t i = 0; i < tmp->mElements.Length(); ++i) {
      nsIContent* c = tmp->mElements[i];
      if (c->IsPurple()) {
        c->RemovePurple();
      }
      Element::MarkNodeChildren(c);
    }
    return true;
  }
NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_END

NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_IN_CC_BEGIN(nsBaseContentList)
  return nsCCUncollectableMarker::sGeneration && tmp->HasKnownLiveWrapper();
NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_IN_CC_END

NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_THIS_BEGIN(nsBaseContentList)
  return nsCCUncollectableMarker::sGeneration && tmp->HasKnownLiveWrapper();
NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_THIS_END

// QueryInterface implementation for nsBaseContentList
NS_INTERFACE_TABLE_HEAD(nsBaseContentList)
  NS_WRAPPERCACHE_INTERFACE_TABLE_ENTRY
  NS_INTERFACE_TABLE(nsBaseContentList, nsINodeList)
  NS_INTERFACE_TABLE_TO_MAP_SEGUE_CYCLE_COLLECTION(nsBaseContentList)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(nsBaseContentList)
NS_IMPL_CYCLE_COLLECTING_RELEASE_WITH_LAST_RELEASE(nsBaseContentList,
                                                   LastRelease())

nsIContent* nsBaseContentList::Item(uint32_t aIndex) {
  return mElements.SafeElementAt(aIndex);
}

int32_t nsBaseContentList::IndexOf(nsIContent* aContent, bool aDoFlush) {
  return mElements.IndexOf(aContent);
}

int32_t nsBaseContentList::IndexOf(nsIContent* aContent) {
  return IndexOf(aContent, true);
}

size_t nsBaseContentList::SizeOfIncludingThis(
    MallocSizeOf aMallocSizeOf) const {
  size_t n = aMallocSizeOf(this);
  n += mElements.ShallowSizeOfExcludingThis(aMallocSizeOf);
  return n;
}

NS_IMPL_CYCLE_COLLECTION_INHERITED(nsSimpleContentList, nsBaseContentList,
                                   mRoot)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(nsSimpleContentList)
NS_INTERFACE_MAP_END_INHERITING(nsBaseContentList)

NS_IMPL_ADDREF_INHERITED(nsSimpleContentList, nsBaseContentList)
NS_IMPL_RELEASE_INHERITED(nsSimpleContentList, nsBaseContentList)

JSObject* nsSimpleContentList::WrapObject(JSContext* cx,
                                          JS::Handle<JSObject*> aGivenProto) {
  return NodeList_Binding::Wrap(cx, this, aGivenProto);
}

NS_IMPL_CYCLE_COLLECTION_INHERITED(nsEmptyContentList, nsBaseContentList, mRoot)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(nsEmptyContentList)
  NS_INTERFACE_MAP_ENTRY(nsIHTMLCollection)
NS_INTERFACE_MAP_END_INHERITING(nsBaseContentList)

NS_IMPL_ADDREF_INHERITED(nsEmptyContentList, nsBaseContentList)
NS_IMPL_RELEASE_INHERITED(nsEmptyContentList, nsBaseContentList)

JSObject* nsEmptyContentList::WrapObject(JSContext* cx,
                                         JS::Handle<JSObject*> aGivenProto) {
  return HTMLCollection_Binding::Wrap(cx, this, aGivenProto);
}

mozilla::dom::Element* nsEmptyContentList::GetElementAt(uint32_t index) {
  return nullptr;
}

mozilla::dom::Element* nsEmptyContentList::GetFirstNamedElement(
    const nsAString& aName, bool& aFound) {
  aFound = false;
  return nullptr;
}

void nsEmptyContentList::GetSupportedNames(nsTArray<nsString>& aNames) {}

nsIContent* nsEmptyContentList::Item(uint32_t aIndex) { return nullptr; }

struct ContentListCache
    : public MruCache<nsContentListKey, nsContentList*, ContentListCache> {
  static HashNumber Hash(const nsContentListKey& aKey) {
    return aKey.GetHash();
  }
  static bool Match(const nsContentListKey& aKey, const nsContentList* aVal) {
    return aVal->MatchesKey(aKey);
  }
};

static ContentListCache sRecentlyUsedContentLists;

class nsContentList::HashEntry : public PLDHashEntryHdr {
 public:
  using KeyType = const nsContentListKey*;
  using KeyTypePointer = KeyType;

  // Note that this is creating a blank entry, so you'll have to manually
  // initialize it after it has been inserted into the hash table.
  explicit HashEntry(KeyTypePointer aKey) : mContentList(nullptr) {}

  HashEntry(HashEntry&& aEnt) : mContentList(std::move(aEnt.mContentList)) {}

  ~HashEntry() {
    if (mContentList) {
      MOZ_RELEASE_ASSERT(mContentList->mInHashtable);
      mContentList->mInHashtable = false;
    }
  }

  bool KeyEquals(KeyTypePointer aKey) const {
    return mContentList->MatchesKey(*aKey);
  }

  static KeyTypePointer KeyToPointer(KeyType aKey) { return aKey; }

  static PLDHashNumber HashKey(KeyTypePointer aKey) { return aKey->GetHash(); }

  nsContentList* GetContentList() const { return mContentList; }
  void SetContentList(nsContentList* aContentList) {
    MOZ_RELEASE_ASSERT(!mContentList);
    MOZ_ASSERT(aContentList);
    MOZ_RELEASE_ASSERT(!aContentList->mInHashtable);
    mContentList = aContentList;
    mContentList->mInHashtable = true;
  }

  enum { ALLOW_MEMMOVE = true };

 private:
  nsContentList* MOZ_UNSAFE_REF(
      "This entry will be removed in nsContentList::RemoveFromHashtable "
      "before mContentList is destroyed") mContentList;
};

// Hashtable for storing nsContentLists
static StaticAutoPtr<nsTHashtable<nsContentList::HashEntry>>
    gContentListHashTable;

already_AddRefed<nsContentList> NS_GetContentList(nsINode* aRootNode,
                                                  int32_t aMatchNameSpaceId,
                                                  const nsAString& aTagname) {
  NS_ASSERTION(aRootNode, "content list has to have a root");

  RefPtr<nsContentList> list;
  nsContentListKey hashKey(aRootNode, aMatchNameSpaceId, aTagname,
                           aRootNode->OwnerDoc()->IsHTMLDocument());
  auto p = sRecentlyUsedContentLists.Lookup(hashKey);
  if (p) {
    list = p.Data();
    return list.forget();
  }

  // Initialize the hashtable if needed.
  if (!gContentListHashTable) {
    gContentListHashTable = new nsTHashtable<nsContentList::HashEntry>();
  }

  // First we look in our hashtable.  Then we create a content list if needed
  auto entry = gContentListHashTable->PutEntry(&hashKey, fallible);
  if (entry) {
    list = entry->GetContentList();
  }

  if (!list) {
    // We need to create a ContentList and add it to our new entry, if
    // we have an entry
    RefPtr<nsAtom> xmlAtom = NS_Atomize(aTagname);
    RefPtr<nsAtom> htmlAtom;
    if (aMatchNameSpaceId == kNameSpaceID_Unknown) {
      nsAutoString lowercaseName;
      nsContentUtils::ASCIIToLower(aTagname, lowercaseName);
      htmlAtom = NS_Atomize(lowercaseName);
    } else {
      htmlAtom = xmlAtom;
    }
    list = new nsContentList(aRootNode, aMatchNameSpaceId, htmlAtom, xmlAtom);
    if (entry) {
      entry->SetContentList(list);
    }
  }

  p.Set(list);
  return list.forget();
}

#ifdef DEBUG
const nsCacheableFuncStringContentList::ContentListType
    nsCachableElementsByNameNodeList::sType =
        nsCacheableFuncStringContentList::eNodeList;
const nsCacheableFuncStringContentList::ContentListType
    nsCacheableFuncStringHTMLCollection::sType =
        nsCacheableFuncStringContentList::eHTMLCollection;
#endif

class nsCacheableFuncStringContentList::HashEntry : public PLDHashEntryHdr {
 public:
  using KeyType = const nsFuncStringCacheKey*;
  using KeyTypePointer = KeyType;

  // Note that this is creating a blank entry, so you'll have to manually
  // initialize it after it has been inserted into the hash table.
  explicit HashEntry(KeyTypePointer aKey) : mContentList(nullptr) {}

  HashEntry(HashEntry&& aEnt) : mContentList(std::move(aEnt.mContentList)) {}

  ~HashEntry() {
    if (mContentList) {
      MOZ_RELEASE_ASSERT(mContentList->mInHashtable);
      mContentList->mInHashtable = false;
    }
  }

  bool KeyEquals(KeyTypePointer aKey) const {
    return mContentList->Equals(aKey);
  }

  static KeyTypePointer KeyToPointer(KeyType aKey) { return aKey; }

  static PLDHashNumber HashKey(KeyTypePointer aKey) { return aKey->GetHash(); }

  nsCacheableFuncStringContentList* GetContentList() const {
    return mContentList;
  }
  void SetContentList(nsCacheableFuncStringContentList* aContentList) {
    MOZ_RELEASE_ASSERT(!mContentList);
    MOZ_ASSERT(aContentList);
    MOZ_RELEASE_ASSERT(!aContentList->mInHashtable);
    mContentList = aContentList;
    mContentList->mInHashtable = true;
  }

  enum { ALLOW_MEMMOVE = true };

 private:
  nsCacheableFuncStringContentList* MOZ_UNSAFE_REF(
      "This entry will be removed in "
      "nsCacheableFuncStringContentList::RemoveFromFuncStringHashtable "
      "before mContentList is destroyed") mContentList;
};

// Hashtable for storing nsCacheableFuncStringContentList
static StaticAutoPtr<nsTHashtable<nsCacheableFuncStringContentList::HashEntry>>
    gFuncStringContentListHashTable;

template <class ListType>
already_AddRefed<nsContentList> GetFuncStringContentList(
    nsINode* aRootNode, nsContentListMatchFunc aFunc,
    nsContentListDestroyFunc aDestroyFunc,
    nsFuncStringContentListDataAllocator aDataAllocator,
    const nsAString& aString) {
  NS_ASSERTION(aRootNode, "content list has to have a root");

  RefPtr<nsCacheableFuncStringContentList> list;

  // Initialize the hashtable if needed.
  if (!gFuncStringContentListHashTable) {
    gFuncStringContentListHashTable =
        new nsTHashtable<nsCacheableFuncStringContentList::HashEntry>();
  }

  nsCacheableFuncStringContentList::HashEntry* entry = nullptr;
  // First we look in our hashtable.  Then we create a content list if needed
  if (gFuncStringContentListHashTable) {
    nsFuncStringCacheKey hashKey(aRootNode, aFunc, aString);

    entry = gFuncStringContentListHashTable->PutEntry(&hashKey, fallible);
    if (entry) {
      list = entry->GetContentList();
#ifdef DEBUG
      MOZ_ASSERT_IF(list, list->mType == ListType::sType);
#endif
    }
  }

  if (!list) {
    // We need to create a ContentList and add it to our new entry, if
    // we have an entry
    list =
        new ListType(aRootNode, aFunc, aDestroyFunc, aDataAllocator, aString);
    if (entry) {
      entry->SetContentList(list);
    }
  }

  // Don't cache these lists globally

  return list.forget();
}

// Explicit instantiations to avoid link errors
template already_AddRefed<nsContentList>
GetFuncStringContentList<nsCachableElementsByNameNodeList>(
    nsINode* aRootNode, nsContentListMatchFunc aFunc,
    nsContentListDestroyFunc aDestroyFunc,
    nsFuncStringContentListDataAllocator aDataAllocator,
    const nsAString& aString);
template already_AddRefed<nsContentList>
GetFuncStringContentList<nsCacheableFuncStringHTMLCollection>(
    nsINode* aRootNode, nsContentListMatchFunc aFunc,
    nsContentListDestroyFunc aDestroyFunc,
    nsFuncStringContentListDataAllocator aDataAllocator,
    const nsAString& aString);

//-----------------------------------------------------
// nsContentList implementation

nsContentList::nsContentList(nsINode* aRootNode, int32_t aMatchNameSpaceId,
                             nsAtom* aHTMLMatchAtom, nsAtom* aXMLMatchAtom,
                             bool aDeep, bool aLiveList)
    : nsBaseContentList(),
      mRootNode(aRootNode),
      mMatchNameSpaceId(aMatchNameSpaceId),
      mHTMLMatchAtom(aHTMLMatchAtom),
      mXMLMatchAtom(aXMLMatchAtom),
      mState(State::Dirty),
      mDeep(aDeep),
      mFuncMayDependOnAttr(false),
      mIsHTMLDocument(aRootNode->OwnerDoc()->IsHTMLDocument()),
      mNamedItemsCacheValid(false),
      mIsLiveList(aLiveList),
      mInHashtable(false) {
  NS_ASSERTION(mRootNode, "Must have root");
  if (nsGkAtoms::_asterisk == mHTMLMatchAtom) {
    NS_ASSERTION(mXMLMatchAtom == nsGkAtoms::_asterisk,
                 "HTML atom and XML atom are not both asterisk?");
    mMatchAll = true;
  } else {
    mMatchAll = false;
  }
  // This is aLiveList instead of mIsLiveList to avoid Valgrind errors.
  if (aLiveList) {
    SetEnabledCallbacks(nsIMutationObserver::kNodeWillBeDestroyed);
    mRootNode->AddMutationObserver(this);
  }

  // We only need to flush if we're in an non-HTML document, since the
  // HTML5 parser doesn't need flushing.  Further, if we're not in a
  // document at all right now (in the GetUncomposedDoc() sense), we're
  // not parser-created and don't need to be flushing stuff under us
  // to get our kids right.
  Document* doc = mRootNode->GetUncomposedDoc();
  mFlushesNeeded = doc && !doc->IsHTMLDocument();
}

nsContentList::nsContentList(nsINode* aRootNode, nsContentListMatchFunc aFunc,
                             nsContentListDestroyFunc aDestroyFunc, void* aData,
                             bool aDeep, nsAtom* aMatchAtom,
                             int32_t aMatchNameSpaceId,
                             bool aFuncMayDependOnAttr, bool aLiveList)
    : nsBaseContentList(),
      mRootNode(aRootNode),
      mMatchNameSpaceId(aMatchNameSpaceId),
      mHTMLMatchAtom(aMatchAtom),
      mXMLMatchAtom(aMatchAtom),
      mFunc(aFunc),
      mDestroyFunc(aDestroyFunc),
      mData(aData),
      mState(State::Dirty),
      mMatchAll(false),
      mDeep(aDeep),
      mFuncMayDependOnAttr(aFuncMayDependOnAttr),
      mIsHTMLDocument(false),
      mNamedItemsCacheValid(false),
      mIsLiveList(aLiveList),
      mInHashtable(false) {
  NS_ASSERTION(mRootNode, "Must have root");
  // This is aLiveList instead of mIsLiveList to avoid Valgrind errors.
  if (aLiveList) {
    SetEnabledCallbacks(nsIMutationObserver::kNodeWillBeDestroyed);
    mRootNode->AddMutationObserver(this);
  }

  // We only need to flush if we're in an non-HTML document, since the
  // HTML5 parser doesn't need flushing.  Further, if we're not in a
  // document at all right now (in the GetUncomposedDoc() sense), we're
  // not parser-created and don't need to be flushing stuff under us
  // to get our kids right.
  Document* doc = mRootNode->GetUncomposedDoc();
  mFlushesNeeded = doc && !doc->IsHTMLDocument();
}

nsContentList::~nsContentList() {
  RemoveFromHashtable();
  if (mIsLiveList && mRootNode) {
    mRootNode->RemoveMutationObserver(this);
  }

  if (mDestroyFunc) {
    // Clean up mData
    (*mDestroyFunc)(mData);
  }
}

JSObject* nsContentList::WrapObject(JSContext* cx,
                                    JS::Handle<JSObject*> aGivenProto) {
  return HTMLCollection_Binding::Wrap(cx, this, aGivenProto);
}

NS_IMPL_ISUPPORTS_INHERITED(nsContentList, nsBaseContentList, nsIHTMLCollection,
                            nsIMutationObserver)

uint32_t nsContentList::Length(bool aDoFlush) {
  BringSelfUpToDate(aDoFlush);

  return mElements.Length();
}

nsIContent* nsContentList::Item(uint32_t aIndex, bool aDoFlush) {
  if (mRootNode && aDoFlush && mFlushesNeeded) {
    // XXX sXBL/XBL2 issue
    Document* doc = mRootNode->GetUncomposedDoc();
    if (doc) {
      // Flush pending content changes Bug 4891.
      doc->FlushPendingNotifications(FlushType::ContentAndNotify);
    }
  }

  if (mState != State::UpToDate) {
    PopulateSelf(std::min(aIndex, UINT32_MAX - 1) + 1);
  }

  ASSERT_IN_SYNC;
  NS_ASSERTION(!mRootNode || mState != State::Dirty,
               "PopulateSelf left the list in a dirty (useless) state!");

  return mElements.SafeElementAt(aIndex);
}

inline void nsContentList::InsertElementInNamedItemsCache(
    nsIContent& aContent) {
  const bool hasName = aContent.HasName();
  const bool hasId = aContent.HasID();
  if (!hasName && !hasId) {
    return;
  }

  Element* el = aContent.AsElement();
  MOZ_ASSERT_IF(hasName, el->IsHTMLElement());

  uint32_t i = 0;
  while (BorrowedAttrInfo info = el->GetAttrInfoAt(i++)) {
    const bool valid = (info.mName->Equals(nsGkAtoms::name) && hasName) ||
                       (info.mName->Equals(nsGkAtoms::id) && hasId);
    if (!valid) {
      continue;
    }

    if (!mNamedItemsCache) {
      mNamedItemsCache = MakeUnique<NamedItemsCache>();
    }

    nsAtom* name = info.mValue->GetAtomValue();
    // NOTE: LookupOrInsert makes sure we keep the first element we find for a
    // given name.
    mNamedItemsCache->LookupOrInsert(name, el);
  }
}

inline void nsContentList::InvalidateNamedItemsCacheForAttributeChange(
    int32_t aNamespaceID, nsAtom* aAttribute) {
  if (!mNamedItemsCacheValid) {
    return;
  }
  if ((aAttribute == nsGkAtoms::id || aAttribute == nsGkAtoms::name) &&
      aNamespaceID == kNameSpaceID_None) {
    InvalidateNamedItemsCache();
  }
}

inline void nsContentList::InvalidateNamedItemsCacheForInsertion(
    Element& aElement) {
  if (!mNamedItemsCacheValid) {
    return;
  }

  InsertElementInNamedItemsCache(aElement);
}

inline void nsContentList::InvalidateNamedItemsCacheForDeletion(
    Element& aElement) {
  if (!mNamedItemsCacheValid) {
    return;
  }
  if (aElement.HasName() || aElement.HasID()) {
    InvalidateNamedItemsCache();
  }
}

void nsContentList::EnsureNamedItemsCacheValid(bool aDoFlush) {
  BringSelfUpToDate(aDoFlush);

  if (mNamedItemsCacheValid) {
    return;
  }

  MOZ_ASSERT(!mNamedItemsCache);

  // https://dom.spec.whatwg.org/#dom-htmlcollection-nameditem-key
  // XXX: Blink/WebKit don't follow the spec here, and searches first-by-id,
  // then by name.
  for (const nsCOMPtr<nsIContent>& content : mElements) {
    InsertElementInNamedItemsCache(*content);
  }

  mNamedItemsCacheValid = true;
}

Element* nsContentList::NamedItem(const nsAString& aName, bool aDoFlush) {
  if (aName.IsEmpty()) {
    return nullptr;
  }

  EnsureNamedItemsCacheValid(aDoFlush);

  if (!mNamedItemsCache) {
    return nullptr;
  }

  // Typically IDs and names are atomized
  RefPtr<nsAtom> name = NS_Atomize(aName);
  NS_ENSURE_TRUE(name, nullptr);

  return mNamedItemsCache->Get(name);
}

void nsContentList::GetSupportedNames(nsTArray<nsString>& aNames) {
  BringSelfUpToDate(true);

  AutoTArray<nsAtom*, 8> atoms;
  for (uint32_t i = 0; i < mElements.Length(); ++i) {
    nsIContent* content = mElements.ElementAt(i);
    if (content->HasID()) {
      nsAtom* id = content->GetID();
      MOZ_ASSERT(id != nsGkAtoms::_empty, "Empty ids don't get atomized");
      if (!atoms.Contains(id)) {
        atoms.AppendElement(id);
      }
    }

    nsGenericHTMLElement* el = nsGenericHTMLElement::FromNode(content);
    if (el) {
      // XXXbz should we be checking for particular tags here?  How
      // stable is this part of the spec?
      // Note: nsINode::HasName means the name is exposed on the document,
      // which is false for options, so we don't check it here.
      const nsAttrValue* val = el->GetParsedAttr(nsGkAtoms::name);
      if (val && val->Type() == nsAttrValue::eAtom) {
        nsAtom* name = val->GetAtomValue();
        MOZ_ASSERT(name != nsGkAtoms::_empty, "Empty names don't get atomized");
        if (!atoms.Contains(name)) {
          atoms.AppendElement(name);
        }
      }
    }
  }

  uint32_t atomsLen = atoms.Length();
  nsString* names = aNames.AppendElements(atomsLen);
  for (uint32_t i = 0; i < atomsLen; ++i) {
    atoms[i]->ToString(names[i]);
  }
}

int32_t nsContentList::IndexOf(nsIContent* aContent, bool aDoFlush) {
  BringSelfUpToDate(aDoFlush);

  return mElements.IndexOf(aContent);
}

int32_t nsContentList::IndexOf(nsIContent* aContent) {
  return IndexOf(aContent, true);
}

void nsContentList::NodeWillBeDestroyed(nsINode* aNode) {
  // We shouldn't do anything useful from now on

  RemoveFromCaches();
  mRootNode = nullptr;

  // We will get no more updates, so we can never know we're up to
  // date
  SetDirty();
}

void nsContentList::LastRelease() {
  RemoveFromCaches();
  if (mIsLiveList && mRootNode) {
    mRootNode->RemoveMutationObserver(this);
    mRootNode = nullptr;
  }
  SetDirty();
}

Element* nsContentList::GetElementAt(uint32_t aIndex) {
  return static_cast<Element*>(Item(aIndex, true));
}

nsIContent* nsContentList::Item(uint32_t aIndex) {
  return GetElementAt(aIndex);
}

void nsContentList::AttributeChanged(Element* aElement, int32_t aNameSpaceID,
                                     nsAtom* aAttribute, int32_t aModType,
                                     const nsAttrValue* aOldValue) {
  MOZ_ASSERT(aElement, "Must have a content node to work with");

  if (mState == State::Dirty ||
      !MayContainRelevantNodes(aElement->GetParentNode()) ||
      !nsContentUtils::IsInSameAnonymousTree(mRootNode, aElement)) {
    // Either we're already dirty or aElement will never match us.
    return;
  }

  InvalidateNamedItemsCacheForAttributeChange(aNameSpaceID, aAttribute);

  if (!mFunc || !mFuncMayDependOnAttr) {
    // aElement might be relevant but the attribute change doesn't affect
    // whether we match it.
    return;
  }

  if (Match(aElement)) {
    if (mElements.IndexOf(aElement) == mElements.NoIndex) {
      // We match aElement now, and it's not in our list already.  Just dirty
      // ourselves; this is simpler than trying to figure out where to insert
      // aElement.
      SetDirty();
    }
  } else {
    // We no longer match aElement.  Remove it from our list.  If it's
    // already not there, this is a no-op (though a potentially
    // expensive one).  Either way, no change of mState is required
    // here.
    if (mElements.RemoveElement(aElement)) {
      InvalidateNamedItemsCacheForDeletion(*aElement);
    }
  }
}

void nsContentList::ContentAppended(nsIContent* aFirstNewContent,
                                    const ContentAppendInfo&) {
  nsIContent* container = aFirstNewContent->GetParent();
  MOZ_ASSERT(container, "Can't get at the new content if no container!");

  /*
   * If the state is State::Dirty then we have no useful information in our list
   * and we want to put off doing work as much as possible.
   *
   * Also, if container is anonymous from our point of view, we know that we
   * can't possibly be matching any of the kids.
   *
   * Optimize out also the common case when just one new node is appended and
   * it doesn't match us.
   */
  if (mState == State::Dirty ||
      !nsContentUtils::IsInSameAnonymousTree(mRootNode, container) ||
      !MayContainRelevantNodes(container) ||
      (!aFirstNewContent->HasChildren() &&
       !aFirstNewContent->GetNextSibling() && !MatchSelf(aFirstNewContent))) {
    MaybeMarkDirty();
    return;
  }

  /*
   * We want to handle the case of ContentAppended by sometimes
   * appending the content to our list, not just setting state to
   * State::Dirty, since most of our ContentAppended notifications
   * should come during pageload and be at the end of the document.
   * Do a bit of work to see whether we could just append to what we
   * already have.
   */

  uint32_t ourCount = mElements.Length();
  const bool appendingToList = [&] {
    if (ourCount == 0) {
      return true;
    }
    if (mRootNode == container) {
      return true;
    }
    return nsContentUtils::PositionIsBefore(mElements.LastElement(),
                                            aFirstNewContent);
  }();

  if (!appendingToList) {
    // The new stuff is somewhere in the middle of our list; check
    // whether we need to invalidate
    for (nsIContent* cur = aFirstNewContent; cur; cur = cur->GetNextSibling()) {
      if (MatchSelf(cur)) {
        // Uh-oh.  We're gonna have to add elements into the middle
        // of our list. That's not worth the effort.
        SetDirty();
        break;
      }
    }

    ASSERT_IN_SYNC;
    return;
  }

  /*
   * At this point we know we could append.  If we're not up to
   * date, however, that would be a bad idea -- it could miss some
   * content that we never picked up due to being lazy.  Further, we
   * may never get asked for this content... so don't grab it yet.
   */
  if (mState == State::Lazy) {
    return;
  }

  /*
   * We're up to date.  That means someone's actively using us; we
   * may as well grab this content....
   */
  if (mDeep) {
    for (nsIContent* cur = aFirstNewContent; cur;
         cur = cur->GetNextNode(container)) {
      if (cur->IsElement() && Match(cur->AsElement())) {
        mElements.AppendElement(cur);
        InvalidateNamedItemsCacheForInsertion(*cur->AsElement());
      }
    }
  } else {
    for (nsIContent* cur = aFirstNewContent; cur; cur = cur->GetNextSibling()) {
      if (cur->IsElement() && Match(cur->AsElement())) {
        mElements.AppendElement(cur);
        InvalidateNamedItemsCacheForInsertion(*cur->AsElement());
      }
    }
  }

  ASSERT_IN_SYNC;
}

void nsContentList::ContentInserted(nsIContent* aChild,
                                    const ContentInsertInfo&) {
  // Note that aChild->GetParentNode() can be null here if we are inserting into
  // the document itself; any attempted optimizations to this method should deal
  // with that.
  if (mState != State::Dirty &&
      MayContainRelevantNodes(aChild->GetParentNode()) &&
      nsContentUtils::IsInSameAnonymousTree(mRootNode, aChild) &&
      MatchSelf(aChild)) {
    SetDirty();
  }

  ASSERT_IN_SYNC;
}

void nsContentList::ContentWillBeRemoved(nsIContent* aChild,
                                         const ContentRemoveInfo&) {
  if (mState != State::Dirty &&
      MayContainRelevantNodes(aChild->GetParentNode()) &&
      nsContentUtils::IsInSameAnonymousTree(mRootNode, aChild) &&
      MatchSelf(aChild)) {
    SetDirty();
  }

  ASSERT_IN_SYNC;
}

bool nsContentList::Match(Element* aElement) {
  if (mFunc) {
    return (*mFunc)(aElement, mMatchNameSpaceId, mXMLMatchAtom, mData);
  }

  if (!mXMLMatchAtom) return false;

  NodeInfo* ni = aElement->NodeInfo();

  bool unknown = mMatchNameSpaceId == kNameSpaceID_Unknown;
  bool wildcard = mMatchNameSpaceId == kNameSpaceID_Wildcard;
  bool toReturn = mMatchAll;
  if (!unknown && !wildcard) toReturn &= ni->NamespaceEquals(mMatchNameSpaceId);

  if (toReturn) return toReturn;

  bool matchHTML =
      mIsHTMLDocument && aElement->GetNameSpaceID() == kNameSpaceID_XHTML;

  if (unknown) {
    return matchHTML ? ni->QualifiedNameEquals(mHTMLMatchAtom)
                     : ni->QualifiedNameEquals(mXMLMatchAtom);
  }

  if (wildcard) {
    return matchHTML ? ni->Equals(mHTMLMatchAtom) : ni->Equals(mXMLMatchAtom);
  }

  return matchHTML ? ni->Equals(mHTMLMatchAtom, mMatchNameSpaceId)
                   : ni->Equals(mXMLMatchAtom, mMatchNameSpaceId);
}

bool nsContentList::MatchSelf(nsIContent* aContent) {
  MOZ_ASSERT(aContent, "Can't match null stuff, you know");
  MOZ_ASSERT(mDeep || aContent->GetParentNode() == mRootNode,
             "MatchSelf called on a node that we can't possibly match");

  if (!aContent->IsElement()) {
    return false;
  }

  if (Match(aContent->AsElement())) return true;

  if (!mDeep) return false;

  for (nsIContent* cur = aContent->GetFirstChild(); cur;
       cur = cur->GetNextNode(aContent)) {
    if (cur->IsElement() && Match(cur->AsElement())) {
      return true;
    }
  }

  return false;
}

void nsContentList::PopulateSelf(uint32_t aNeededLength,
                                 uint32_t aExpectedElementsIfDirty) {
  if (!mRootNode) {
    return;
  }

  ASSERT_IN_SYNC;

  uint32_t count = mElements.Length();
  NS_ASSERTION(mState != State::Dirty || count == aExpectedElementsIfDirty,
               "Reset() not called when setting state to State::Dirty?");

  if (count >= aNeededLength)  // We're all set
    return;

  uint32_t elementsToAppend = aNeededLength - count;
#ifdef DEBUG
  uint32_t invariant = elementsToAppend + mElements.Length();
#endif

  if (mDeep) {
    // If we already have nodes start searching at the last one, otherwise
    // start searching at the root.
    nsINode* cur = count ? mElements[count - 1].get() : mRootNode;
    do {
      cur = cur->GetNextNode(mRootNode);
      if (!cur) {
        break;
      }
      if (cur->IsElement() && Match(cur->AsElement())) {
        // Append AsElement() to get nsIContent instead of nsINode
        mElements.AppendElement(cur->AsElement());
        --elementsToAppend;
      }
    } while (elementsToAppend);
  } else {
    nsIContent* cur = count ? mElements[count - 1]->GetNextSibling()
                            : mRootNode->GetFirstChild();
    for (; cur && elementsToAppend; cur = cur->GetNextSibling()) {
      if (cur->IsElement() && Match(cur->AsElement())) {
        mElements.AppendElement(cur);
        --elementsToAppend;
      }
    }
  }

  NS_ASSERTION(elementsToAppend + mElements.Length() == invariant,
               "Something is awry!");

  if (elementsToAppend != 0) {
    mState = State::UpToDate;
  } else {
    mState = State::Lazy;
  }

  SetEnabledCallbacks(nsIMutationObserver::kAll);

  ASSERT_IN_SYNC;
}

void nsContentList::RemoveFromHashtable() {
  if (mFunc) {
    // nsCacheableFuncStringContentList can be in a hash table without being
    // in gContentListHashTable, but it will have been removed from the hash
    // table in its dtor before it runs the nsContentList dtor.
    MOZ_RELEASE_ASSERT(!mInHashtable);

    // This can't be in gContentListHashTable.
    return;
  }

  nsDependentAtomString str(mXMLMatchAtom);
  nsContentListKey key(mRootNode, mMatchNameSpaceId, str, mIsHTMLDocument);
  sRecentlyUsedContentLists.Remove(key);

  if (gContentListHashTable) {
    gContentListHashTable->RemoveEntry(&key);

    if (gContentListHashTable->Count() == 0) {
      gContentListHashTable = nullptr;
    }
  }

  MOZ_RELEASE_ASSERT(!mInHashtable);
}

void nsContentList::BringSelfUpToDate(bool aDoFlush) {
  if (mFlushesNeeded && mRootNode && aDoFlush) {
    // XXX sXBL/XBL2 issue
    if (Document* doc = mRootNode->GetUncomposedDoc()) {
      // Flush pending content changes Bug 4891.
      doc->FlushPendingNotifications(FlushType::ContentAndNotify);
    }
  }

  if (mState != State::UpToDate) {
    PopulateSelf(uint32_t(-1));
  }

  mMissedUpdates = 0;

  ASSERT_IN_SYNC;
  NS_ASSERTION(!mRootNode || mState == State::UpToDate,
               "PopulateSelf dod not bring content list up to date!");
}

nsCacheableFuncStringContentList::~nsCacheableFuncStringContentList() {
  RemoveFromFuncStringHashtable();
}

void nsCacheableFuncStringContentList::RemoveFromFuncStringHashtable() {
  if (!gFuncStringContentListHashTable) {
    MOZ_RELEASE_ASSERT(!mInHashtable);
    return;
  }

  nsFuncStringCacheKey key(mRootNode, mFunc, mString);
  gFuncStringContentListHashTable->RemoveEntry(&key);

  if (gFuncStringContentListHashTable->Count() == 0) {
    gFuncStringContentListHashTable = nullptr;
  }

  MOZ_RELEASE_ASSERT(!mInHashtable);
}

#ifdef DEBUG_CONTENT_LIST
void nsContentList::AssertInSync() {
  if (mState == State::Dirty) {
    return;
  }

  if (!mRootNode) {
    NS_ASSERTION(mElements.Length() == 0 && mState == State::Dirty,
                 "Empty iterator isn't quite empty?");
    return;
  }

  // XXX This code will need to change if nsContentLists can ever match
  // elements that are outside of the document element.
  nsIContent* root = mRootNode->IsDocument()
                         ? mRootNode->AsDocument()->GetRootElement()
                         : mRootNode->AsContent();

  PreContentIterator preOrderIter;
  if (mDeep) {
    preOrderIter.Init(root);
    preOrderIter.First();
  }

  uint32_t cnt = 0, index = 0;
  while (true) {
    if (cnt == mElements.Length() && mState == State::Lazy) {
      break;
    }

    nsIContent* cur =
        mDeep ? preOrderIter.GetCurrentNode() : mRootNode->GetChildAt(index++);
    if (!cur) {
      break;
    }

    if (cur->IsElement() && Match(cur->AsElement())) {
      NS_ASSERTION(cnt < mElements.Length() && mElements[cnt] == cur,
                   "Elements is out of sync");
      ++cnt;
    }

    if (mDeep) {
      preOrderIter.Next();
    }
  }

  NS_ASSERTION(cnt == mElements.Length(), "Too few elements");
}
#endif

//-----------------------------------------------------
// nsCachableElementsByNameNodeList

JSObject* nsCachableElementsByNameNodeList::WrapObject(
    JSContext* cx, JS::Handle<JSObject*> aGivenProto) {
  return NodeList_Binding::Wrap(cx, this, aGivenProto);
}

void nsCachableElementsByNameNodeList::AttributeChanged(
    Element* aElement, int32_t aNameSpaceID, nsAtom* aAttribute,
    int32_t aModType, const nsAttrValue* aOldValue) {
  // No need to rebuild the list if the changed attribute is not the name
  // attribute.
  if (aAttribute != nsGkAtoms::name) {
    InvalidateNamedItemsCacheForAttributeChange(aNameSpaceID, aAttribute);
    return;
  }

  nsCacheableFuncStringContentList::AttributeChanged(
      aElement, aNameSpaceID, aAttribute, aModType, aOldValue);
}

//-----------------------------------------------------
// nsCacheableFuncStringHTMLCollection

JSObject* nsCacheableFuncStringHTMLCollection::WrapObject(
    JSContext* cx, JS::Handle<JSObject*> aGivenProto) {
  return HTMLCollection_Binding::Wrap(cx, this, aGivenProto);
}

//-----------------------------------------------------
// nsLabelsNodeList

JSObject* nsLabelsNodeList::WrapObject(JSContext* cx,
                                       JS::Handle<JSObject*> aGivenProto) {
  return NodeList_Binding::Wrap(cx, this, aGivenProto);
}

void nsLabelsNodeList::AttributeChanged(Element* aElement, int32_t aNameSpaceID,
                                        nsAtom* aAttribute, int32_t aModType,
                                        const nsAttrValue* aOldValue) {
  MOZ_ASSERT(aElement, "Must have a content node to work with");
  if (mState == State::Dirty ||
      !nsContentUtils::IsInSameAnonymousTree(mRootNode, aElement)) {
    return;
  }

  InvalidateNamedItemsCacheForAttributeChange(aNameSpaceID, aAttribute);

  // We need to handle input type changes to or from "hidden".
  if (aElement->IsHTMLElement(nsGkAtoms::input) &&
      aAttribute == nsGkAtoms::type && aNameSpaceID == kNameSpaceID_None) {
    SetDirty();
    return;
  }
}

void nsLabelsNodeList::ContentAppended(nsIContent* aFirstNewContent,
                                       const ContentAppendInfo&) {
  nsIContent* container = aFirstNewContent->GetParent();
  // If a labelable element is moved to outside or inside of
  // nested associated labels, we're gonna have to modify
  // the content list.
  if (mState != State::Dirty &&
      nsContentUtils::IsInSameAnonymousTree(mRootNode, container)) {
    SetDirty();
    return;
  }
}

void nsLabelsNodeList::ContentInserted(nsIContent* aChild,
                                       const ContentInsertInfo&) {
  // If a labelable element is moved to outside or inside of
  // nested associated labels, we're gonna have to modify
  // the content list.
  if (mState != State::Dirty &&
      nsContentUtils::IsInSameAnonymousTree(mRootNode, aChild)) {
    SetDirty();
    return;
  }
}

void nsLabelsNodeList::ContentWillBeRemoved(nsIContent* aChild,
                                            const ContentRemoveInfo&) {
  // If a labelable element is removed, we're gonna have to clean
  // the content list.
  if (mState != State::Dirty &&
      nsContentUtils::IsInSameAnonymousTree(mRootNode, aChild)) {
    SetDirty();
    return;
  }
}

void nsLabelsNodeList::MaybeResetRoot(nsINode* aRootNode) {
  MOZ_ASSERT(aRootNode, "Must have root");
  if (mRootNode == aRootNode) {
    return;
  }

  MOZ_ASSERT(mIsLiveList, "nsLabelsNodeList is always a live list");
  if (mRootNode) {
    mRootNode->RemoveMutationObserver(this);
  }
  mRootNode = aRootNode;
  mRootNode->AddMutationObserver(this);
  SetDirty();
}

void nsLabelsNodeList::PopulateSelf(uint32_t aNeededLength,
                                    uint32_t aExpectedElementsIfDirty) {
  if (!mRootNode) {
    return;
  }

  // Start searching at the root.
  nsINode* cur = mRootNode;
  if (mElements.IsEmpty() && cur->IsElement() && Match(cur->AsElement())) {
    mElements.AppendElement(cur->AsElement());
    ++aExpectedElementsIfDirty;
  }

  nsContentList::PopulateSelf(aNeededLength, aExpectedElementsIfDirty);
}
