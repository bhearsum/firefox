/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GlobalStyleSheetCache.h"

#include "MainThreadUtils.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/NeverDestroyed.h"
#include "mozilla/Preferences.h"
#include "mozilla/ServoBindings.h"
#include "mozilla/StaticPrefs_layout.h"
#include "mozilla/StyleSheet.h"
#include "mozilla/StyleSheetInlines.h"
#include "mozilla/css/Loader.h"
#include "mozilla/dom/ReferrerInfo.h"
#include "mozilla/dom/SRIMetadata.h"
#include "mozilla/ipc/SharedMemoryHandle.h"
#include "mozilla/ipc/SharedMemoryMapping.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsContentUtils.h"
#include "nsExceptionHandler.h"
#include "nsIConsoleService.h"
#include "nsIFile.h"
#include "nsIObserverService.h"
#include "nsIXULRuntime.h"
#include "nsNetUtil.h"
#include "nsPrintfCString.h"
#include "nsServiceManagerUtils.h"
#include "nsXULAppAPI.h"

namespace mozilla {

// The GlobalStyleSheetCache is responsible for sharing user agent style sheet
// contents across processes using shared memory.  Here is a high level view of
// how that works:
//
// * In the parent process, in the GlobalStyleSheetCache constructor (which is
//   called early on in a process' lifetime), we parse all UA style sheets into
//   Gecko StyleSheet objects.
//
// * The constructor calls InitSharedSheetsInParent, which creates a shared
//   memory segment that we know ahead of time will be big enough to store the
//   UA sheets.
//
// * It then creates a Rust SharedMemoryBuilder object and passes it a pointer
//   to the start of the shared memory.
//
// * For each UA sheet, we call Servo_SharedMemoryBuilder_AddStylesheet, which
//   takes the StylesheetContents::rules (an Arc<Locked<CssRules>>), produces a
//   deep clone of it, and writes that clone into the shared memory:
//
//   * The deep clone isn't a clone() call, but a call to ToShmem::to_shmem. The
//     ToShmem trait must be implemented on every type that is reachable under
//     the Arc<Locked<CssRules>>. The to_shmem call for each type will clone the
//     value, but any heap allocation will be cloned and placed into the shared
//     memory buffer, rather than heap allocated.
//
//   * For most types, the ToShmem implementation is simple, and we just
//     #[derive(ToShmem)] it. For the types that need special handling due to
//     having heap allocations (Vec<T>, Box<T>, Arc<T>, etc.) we have impls that
//     call to_shmem on the heap allocated data, and then create a new container
//     (e.g. using Box::from_raw) that points into the shared memory.
//
//   * Arc<T> and Locked<T> want to perform atomic writes on data that needs to
//     be in the shared memory buffer (the reference count for Arc<T>, and the
//     SharedRwLock's AtomicRefCell for Locked<T>), so we add special modes to
//     those objects that skip the writes.  For Arc<T>, that means never
//     dropping the object since we don't track the reference count.  That's
//     fine, since we want to just drop the entire shared memory buffer at
//     shutdown.  For Locked<T>, we just panic on attempting to take the lock
//     for writing.  That's also fine, since we don't want devtools being able
//     to modify UA sheets.
//
//   * For Atoms in Rust, static atoms are represented by an index into the
//     static atom table.  Then if we need to Deref the Atom we look up the
//     table.  We panic if any Atom we encounter in the UA style sheets is
//     not a static atom.
//
// * For each UA sheet, we create a new C++ StyleSheet object using the shared
//   memory clone of the sheet contents, and throw away the original heap
//   allocated one.  (We could avoid creating a new C++ StyleSheet object
//   wrapping the shared contents, and update the original StyleSheet object's
//   contents, but it's doubtful that matters.)
//
// * When we initially map the shared memory in the parent process in
//   InitSharedSheetsInParent, we choose an address which is far away from the
//   current extent of the heap.  Although not too far, since we don't want to
//   unnecessarily fragment the virtual address space.
//
// * In the child process, as early as possible (in
//   ContentChild::InitSharedUASheets), we try to map the shared memory at that
//   same address, then pass the shared memory buffer to
//   GlobalStyleSheetCache::SetSharedMemory.  Since we map at the same
//   address, this means any internal pointers in the UA sheets back into the
//   shared memory buffer that were written by the parent process are valid in
//   the child process too.
//
// * In practice, mapping at the address we need in the child process this works
//   nearly all the time.  If we fail to map at the address we need, the child
//   process falls back to parsing and allocating its own copy of the UA sheets.

using namespace mozilla;
using namespace css;

mozilla::ipc::ReadOnlySharedMemoryHandle& sSharedMemoryHandle() {
  static NeverDestroyed<mozilla::ipc::ReadOnlySharedMemoryHandle> handle;
  return *handle;
}

#define PREF_LEGACY_STYLESHEET_CUSTOMIZATION \
  "toolkit.legacyUserProfileCustomizations.stylesheets"

NS_IMPL_ISUPPORTS(GlobalStyleSheetCache, nsIObserver, nsIMemoryReporter)

nsresult GlobalStyleSheetCache::Observe(nsISupports* aSubject,
                                        const char* aTopic,
                                        const char16_t* aData) {
  if (!strcmp(aTopic, "profile-before-change")) {
    mUserContentSheet = nullptr;
    mUserChromeSheet = nullptr;
  } else if (!strcmp(aTopic, "profile-do-change")) {
    InitFromProfile();
  } else {
    MOZ_ASSERT_UNREACHABLE("Unexpected observer topic.");
  }
  return NS_OK;
}

static constexpr struct {
  nsLiteralCString mURL;
  BuiltInStyleSheetFlags mFlags;
} kBuiltInSheetInfo[] = {
#define STYLE_SHEET(identifier_, url_, flags_) \
  {nsLiteralCString(url_), BuiltInStyleSheetFlags::flags_},
#include "mozilla/BuiltInStyleSheetList.h"
#undef STYLE_SHEET
};

NotNull<StyleSheet*> GlobalStyleSheetCache::BuiltInSheet(
    BuiltInStyleSheet aSheet) {
  auto& slot = mBuiltIns[aSheet];
  if (!slot) {
    const auto& info = kBuiltInSheetInfo[size_t(aSheet)];
    const auto parsingMode = (info.mFlags & BuiltInStyleSheetFlags::UA)
                                 ? eAgentSheetFeatures
                                 : eAuthorSheetFeatures;
    MOZ_ASSERT(info.mFlags & BuiltInStyleSheetFlags::UA ||
               info.mFlags & BuiltInStyleSheetFlags::Author);
    slot = LoadSheetURL(info.mURL, parsingMode, eCrash);
  }
  return WrapNotNull(slot);
}

StyleSheet* GlobalStyleSheetCache::GetUserContentSheet() {
  return mUserContentSheet;
}

StyleSheet* GlobalStyleSheetCache::GetUserChromeSheet() {
  return mUserChromeSheet;
}

void GlobalStyleSheetCache::Shutdown() {
  gCSSLoader = nullptr;
  NS_WARNING_ASSERTION(!gStyleCache || !gUserContentSheetURL,
                       "Got the URL but never used?");
  gStyleCache = nullptr;
  gUserContentSheetURL = nullptr;
  for (auto& r : URLExtraData::sShared) {
    r = nullptr;
  }
  // We want to leak the shared memory forever, rather than cleaning up all
  // potential DOM references and such that chrome code may have created.
}

void GlobalStyleSheetCache::SetUserContentCSSURL(nsIURI* aURI) {
  MOZ_ASSERT(XRE_IsContentProcess(), "Only used in content processes.");
  gUserContentSheetURL = aURI;
}

MOZ_DEFINE_MALLOC_SIZE_OF(LayoutStylesheetCacheMallocSizeOf)

NS_IMETHODIMP
GlobalStyleSheetCache::CollectReports(nsIHandleReportCallback* aHandleReport,
                                      nsISupports* aData, bool aAnonymize) {
  MOZ_COLLECT_REPORT("explicit/layout/style-sheet-cache/unshared", KIND_HEAP,
                     UNITS_BYTES,
                     SizeOfIncludingThis(LayoutStylesheetCacheMallocSizeOf),
                     "Memory used for built-in style sheets that are not "
                     "shared between processes.");

  if (XRE_IsParentProcess()) {
    MOZ_COLLECT_REPORT(
        "explicit/layout/style-sheet-cache/shared", KIND_NONHEAP, UNITS_BYTES,
        sSharedMemory.IsEmpty() ? 0 : sUsedSharedMemory,
        "Memory used for built-in style sheets that are shared to "
        "child processes.");
  }

  return NS_OK;
}

size_t GlobalStyleSheetCache::SizeOfIncludingThis(
    MallocSizeOf aMallocSizeOf) const {
  size_t n = aMallocSizeOf(this);

#define MEASURE(s) n += s ? s->SizeOfIncludingThis(aMallocSizeOf) : 0;

  for (const auto& sheet : mBuiltIns) {
    MEASURE(sheet);
  }

  MEASURE(mUserChromeSheet);
  MEASURE(mUserContentSheet);

  // Measurement of the following members may be added later if DMD finds it is
  // worthwhile:
  // - gCSSLoader

  return n;
}

GlobalStyleSheetCache::GlobalStyleSheetCache() {
  nsCOMPtr<nsIObserverService> obsSvc = services::GetObserverService();
  NS_ASSERTION(obsSvc, "No global observer service?");

  if (obsSvc) {
    obsSvc->AddObserver(this, "profile-before-change", false);
    obsSvc->AddObserver(this, "profile-do-change", false);
  }

  // Load user style sheets.
  InitFromProfile();

  if (XRE_IsParentProcess()) {
    // We know we need xul.css for the UI, so load that now too:
    XULSheet();
  }

  if (gUserContentSheetURL) {
    MOZ_ASSERT(XRE_IsContentProcess(), "Only used in content processes.");
    mUserContentSheet =
        LoadSheet(gUserContentSheetURL, eUserSheetFeatures, eLogToConsole);
    gUserContentSheetURL = nullptr;
  }

  // If we are the in the parent process, then we load all of the UA sheets that
  // are shareable and store them into shared memory.  In both the parent and
  // the content process, we load these sheets out of shared memory.
  //
  // The shared memory buffer's format is a Header object, which contains
  // internal pointers to each of the shared style sheets, followed by the style
  // sheets themselves.
  if (StaticPrefs::layout_css_shared_memory_ua_sheets_enabled()) {
    if (XRE_IsParentProcess()) {
      // Load the style sheets and store them in a new shared memory buffer.
      InitSharedSheetsInParent();
    } else if (!sSharedMemory.IsEmpty()) {
      // Use the shared memory that was given to us by a SetSharedMemory call
      // under ContentChild::InitXPCOM.
      MOZ_ASSERT(sSharedMemory.data(),
                 "GlobalStyleSheetCache::SetSharedMemory should have mapped "
                 "the shared memory");
    }
  }

  // If we get here and we don't have a shared memory handle, then it means
  // either we failed to create the shared memory buffer in the parent process
  // (unexpected), or we failed to map the shared memory buffer at the address
  // we needed in the content process (might happen).
  //
  // If sSharedMemory is non-null, but it is not currently mapped, then it means
  // we are in the parent process, and we failed to re-map the memory after
  // freezing it.  (We keep sSharedMemory around so that we can still share it
  // to content processes.)
  //
  // In the parent process, this means we'll just leave our eagerly loaded
  // non-shared sheets in the mFooSheet fields.  In a content process, we'll
  // lazily load our own copies of the sheets later.
  if (!sSharedMemory.IsEmpty()) {
    if (const auto* header =
            reinterpret_cast<const Header*>(sSharedMemory.data())) {
      MOZ_RELEASE_ASSERT(header->mMagic == Header::kMagic);

      for (auto kind : MakeEnumeratedRange(BuiltInStyleSheet::Count)) {
        const auto& info = kBuiltInSheetInfo[size_t(kind)];
        if (info.mFlags & BuiltInStyleSheetFlags::NotShared) {
          continue;
        }
        const auto parsingMode = (info.mFlags & BuiltInStyleSheetFlags::UA)
                                     ? eAgentSheetFeatures
                                     : eAuthorSheetFeatures;
        LoadSheetFromSharedMemory(info.mURL, &mBuiltIns[kind], parsingMode,
                                  header, kind);
      }
    }
  }
}

void GlobalStyleSheetCache::LoadSheetFromSharedMemory(
    const nsACString& aURL, RefPtr<StyleSheet>* aSheet,
    SheetParsingMode aParsingMode, const Header* aHeader,
    BuiltInStyleSheet aSheetID) {
  auto i = size_t(aSheetID);

  auto sheet =
      MakeRefPtr<StyleSheet>(aParsingMode, CORS_NONE, dom::SRIMetadata());

  nsCOMPtr<nsIURI> uri;
  MOZ_ALWAYS_SUCCEEDS(NS_NewURI(getter_AddRefs(uri), aURL));

  sheet->SetPrincipal(nsContentUtils::GetSystemPrincipal());
  sheet->SetURIs(uri, uri, uri);
  nsCOMPtr<nsIReferrerInfo> referrerInfo =
      dom::ReferrerInfo::CreateForExternalCSSResources(sheet);
  sheet->SetReferrerInfo(referrerInfo);
  sheet->SetSharedContents(aHeader->mSheets[i]);
  sheet->SetComplete();
  URLExtraData::sShared[i] = sheet->URLData();

  *aSheet = std::move(sheet);
}

void GlobalStyleSheetCache::InitSharedSheetsInParent() {
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_RELEASE_ASSERT(sSharedMemory.IsEmpty());

  auto handle = ipc::shared_memory::CreateFreezable(kSharedMemorySize);
  if (NS_WARN_IF(!handle)) {
    return;
  }

  // We need to choose an address to map the shared memory in the parent process
  // that we'll also be able to use in content processes.  There's no way to
  // pick an address that is guaranteed to be free in future content processes,
  // so instead we pick an address that is some distance away from current heap
  // allocations and hope that by the time the content process maps the shared
  // memory, that address will be free.
  //
  // On 64 bit, we have a large amount of address space, so we pick an address
  // half way through the next 8 GiB of free space, and this has a very good
  // chance of succeeding.  On 32 bit, address space is more constrained.  We
  // only have 3 GiB of space to work with, and we don't want to pick a location
  // right in the middle, since that could cause future large allocations to
  // fail.  So we pick an address half way through the next 512 MiB of free
  // space.  Experimentally this seems to work 9 times out of 10; this is good
  // enough, as it means only 1 in 10 content processes will have its own unique
  // copies of the UA style sheets, and we're still getting a significant
  // overall memory saving.
  //
  // In theory ASLR could reduce the likelihood of the mapping succeeding in
  // content processes, due to our expectations of where the heap is being
  // wrong, but in practice this isn't an issue.
#ifdef HAVE_64BIT_BUILD
  constexpr size_t kOffset = 0x200000000ULL;  // 8 GiB
#else
  constexpr size_t kOffset = 0x20000000;  // 512 MiB
#endif

  void* address = nullptr;
  if (void* p = ipc::shared_memory::FindFreeAddressSpace(2 * kOffset)) {
    address = reinterpret_cast<void*>(uintptr_t(p) + kOffset);
  }

  auto mapping = std::move(handle).Map(address);
  if (!mapping) {
    // Failed to map at the address we computed for some reason.  Fall back
    // to just allocating at a location of the OS's choosing, and hope that
    // it works in the content process.
    auto handle = std::move(mapping).Unmap();
    mapping = std::move(handle).Map();
    if (NS_WARN_IF(!mapping)) {
      return;
    }
  }
  address = mapping.Address();

  auto* header = static_cast<Header*>(address);
  header->mMagic = Header::kMagic;
#ifdef DEBUG
  for (const auto* ptr : header->mSheets) {
    MOZ_RELEASE_ASSERT(!ptr, "expected shared memory to have been zeroed");
  }
#endif

  UniquePtr<StyleSharedMemoryBuilder> builder(Servo_SharedMemoryBuilder_Create(
      header->mBuffer, kSharedMemorySize - offsetof(Header, mBuffer)));

  nsCString message;

  // Copy each one into the shared memory, and record its pointer.
  //
  // Normally calling ToShared on UA sheets should not fail.  It happens
  // in practice in odd cases that seem like corrupted installations; see bug
  // 1621773.  On failure, return early and fall back to non-shared sheets.
  for (auto kind : MakeEnumeratedRange(BuiltInStyleSheet::Count)) {
    auto i = size_t(kind);
    const auto& info = kBuiltInSheetInfo[i];
    if (info.mFlags & BuiltInStyleSheetFlags::NotShared) {
      continue;
    }
    StyleSheet* sheet = BuiltInSheet(kind);
    URLExtraData::sShared[i] = sheet->URLData();
    header->mSheets[i] = sheet->ToShared(builder.get(), message);
    if (!header->mSheets[i]) {
      CrashReporter::AppendAppNotesToCrashReport("\n"_ns + message);
      return;
    }
  }

  // Finished writing into the shared memory.  Freeze it, so that a process
  // can't confuse other processes by changing the UA style sheet contents.
  auto readOnlyHandle = std::move(mapping).Freeze();
  if (NS_WARN_IF(!readOnlyHandle)) {
    return;
  }

  auto roMapping = readOnlyHandle.Map(address);

  // Record how must of the shared memory we have used, for memory reporting
  // later.  We round up to the nearest page since the free space at the end
  // of the page isn't really usable for anything else.
  //
  // TODO(heycam): This won't be true on Windows unless we allow creating the
  // shared memory with SEC_RESERVE so that the pages are reserved but not
  // committed.
  size_t pageSize = ipc::shared_memory::SystemPageSize();
  sUsedSharedMemory =
      (Servo_SharedMemoryBuilder_GetLength(builder.get()) + pageSize - 1) &
      ~(pageSize - 1);

  sSharedMemory = std::move(roMapping).Release();
  sSharedMemoryHandle() = std::move(readOnlyHandle);
}

GlobalStyleSheetCache::~GlobalStyleSheetCache() {
  UnregisterWeakMemoryReporter(this);
}

void GlobalStyleSheetCache::InitMemoryReporter() {
  RegisterWeakMemoryReporter(this);
}

/* static */
GlobalStyleSheetCache* GlobalStyleSheetCache::Singleton() {
  MOZ_ASSERT(NS_IsMainThread());

  if (!gStyleCache) {
    gStyleCache = new GlobalStyleSheetCache;
    gStyleCache->InitMemoryReporter();

    // For each pref that controls a CSS feature that a UA style sheet depends
    // on (such as a pref that enables a property that a UA style sheet uses),
    // register DependentPrefChanged as a callback to ensure that the relevant
    // style sheets will be re-parsed.
    // Preferences::RegisterCallback(&DependentPrefChanged,
    //                               "layout.css.example-pref.enabled");
  }

  return gStyleCache;
}

void GlobalStyleSheetCache::InitFromProfile() {
  if (!Preferences::GetBool(PREF_LEGACY_STYLESHEET_CUSTOMIZATION)) {
    return;
  }

  nsCOMPtr<nsIXULRuntime> appInfo =
      do_GetService("@mozilla.org/xre/app-info;1");
  if (appInfo) {
    bool inSafeMode = false;
    appInfo->GetInSafeMode(&inSafeMode);
    if (inSafeMode) {
      return;
    }
  }
  nsCOMPtr<nsIFile> contentFile;
  nsCOMPtr<nsIFile> chromeFile;

  NS_GetSpecialDirectory(NS_APP_USER_CHROME_DIR, getter_AddRefs(contentFile));
  if (!contentFile) {
    // if we don't have a profile yet, that's OK!
    return;
  }

  contentFile->Clone(getter_AddRefs(chromeFile));
  if (!chromeFile) {
    return;
  }

  contentFile->Append(u"userContent.css"_ns);
  chromeFile->Append(u"userChrome.css"_ns);

  mUserContentSheet = LoadSheetFile(contentFile, eUserSheetFeatures);
  mUserChromeSheet = LoadSheetFile(chromeFile, eUserSheetFeatures);
}

RefPtr<StyleSheet> GlobalStyleSheetCache::LoadSheetURL(
    const nsACString& aURL, SheetParsingMode aParsingMode,
    FailureAction aFailureAction) {
  nsCOMPtr<nsIURI> uri;
  NS_NewURI(getter_AddRefs(uri), aURL);
  return LoadSheet(uri, aParsingMode, aFailureAction);
}

RefPtr<StyleSheet> GlobalStyleSheetCache::LoadSheetFile(
    nsIFile* aFile, SheetParsingMode aParsingMode) {
  bool exists = false;
  aFile->Exists(&exists);
  if (!exists) {
    return nullptr;
  }

  nsCOMPtr<nsIURI> uri;
  NS_NewFileURI(getter_AddRefs(uri), aFile);
  return LoadSheet(uri, aParsingMode, eLogToConsole);
}

static void ErrorLoadingSheet(nsIURI* aURI, const char* aMsg,
                              FailureAction aFailureAction) {
  nsPrintfCString errorMessage("%s loading built-in stylesheet '%s'", aMsg,
                               aURI ? aURI->GetSpecOrDefault().get() : "");
  if (aFailureAction == eLogToConsole) {
    nsCOMPtr<nsIConsoleService> cs =
        do_GetService(NS_CONSOLESERVICE_CONTRACTID);
    if (cs) {
      cs->LogStringMessage(NS_ConvertUTF8toUTF16(errorMessage).get());
      return;
    }
  }

  MOZ_CRASH_UNSAFE(errorMessage.get());
}

RefPtr<StyleSheet> GlobalStyleSheetCache::LoadSheet(
    nsIURI* aURI, SheetParsingMode aParsingMode, FailureAction aFailureAction) {
  if (!aURI) {
    ErrorLoadingSheet(aURI, "null URI", eCrash);
    return nullptr;
  }

  if (!gCSSLoader) {
    gCSSLoader = new Loader;
  }

  auto result = gCSSLoader->LoadSheetSync(aURI, aParsingMode,
                                          css::Loader::UseSystemPrincipal::Yes);
  if (MOZ_UNLIKELY(result.isErr())) {
    ErrorLoadingSheet(
        aURI,
        nsPrintfCString("LoadSheetSync failed with error %" PRIx32,
                        static_cast<uint32_t>(result.unwrapErr()))
            .get(),
        aFailureAction);
  }
  return result.unwrapOr(nullptr);
}

/* static */ void GlobalStyleSheetCache::SetSharedMemory(
    ipc::ReadOnlySharedMemoryHandle aHandle, uintptr_t aAddress) {
  MOZ_ASSERT(!XRE_IsParentProcess());
  MOZ_ASSERT(!gStyleCache, "Too late, GlobalStyleSheetCache already created!");
  MOZ_ASSERT(sSharedMemory.IsEmpty(), "Shouldn't call this more than once");

  auto mapping = aHandle.Map(reinterpret_cast<void*>(aAddress));
  if (!mapping) {
    return;
  }

  sSharedMemory = std::move(mapping).Release();
  sSharedMemoryHandle() = std::move(aHandle);
}

ipc::ReadOnlySharedMemoryHandle GlobalStyleSheetCache::CloneHandle() {
  MOZ_ASSERT(XRE_IsParentProcess());
  if (sSharedMemoryHandle().IsValid()) {
    return sSharedMemoryHandle().Clone();
  }
  return nullptr;
}

StaticRefPtr<GlobalStyleSheetCache> GlobalStyleSheetCache::gStyleCache;
StaticRefPtr<css::Loader> GlobalStyleSheetCache::gCSSLoader;
StaticRefPtr<nsIURI> GlobalStyleSheetCache::gUserContentSheetURL;

ipc::shared_memory::LeakedReadOnlyMapping GlobalStyleSheetCache::sSharedMemory;
size_t GlobalStyleSheetCache::sUsedSharedMemory;

}  // namespace mozilla
