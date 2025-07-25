/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/SandboxSettings.h"
#include "mozISandboxSettings.h"
#include "nsServiceManagerUtils.h"
#include "nsAppRunner.h"

#include "mozilla/Components.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/StaticPrefs_network.h"
#include "mozilla/StaticPrefs_security.h"
#include "mozilla/StaticPrefs_webgl.h"

#include "prenv.h"

#ifdef XP_WIN
#  include "mozilla/gfx/gfxVars.h"
#  include "mozilla/WindowsVersion.h"
#  include "nsExceptionHandler.h"
#  include "PDMFactory.h"
#endif  // XP_WIN

using namespace mozilla;

namespace mozilla {

const char* ContentWin32kLockdownStateToString(
    nsIXULRuntime::ContentWin32kLockdownState aValue) {
  switch (aValue) {
    case nsIXULRuntime::ContentWin32kLockdownState::LockdownEnabled:
      return "Win32k Lockdown enabled";

    case nsIXULRuntime::ContentWin32kLockdownState::MissingWebRender:
      return "Win32k Lockdown disabled -- Missing WebRender";

    case nsIXULRuntime::ContentWin32kLockdownState::OperatingSystemNotSupported:
      return "Win32k Lockdown disabled -- Operating system not supported";

    case nsIXULRuntime::ContentWin32kLockdownState::PrefNotSet:
      return "Win32k Lockdown disabled -- Preference not set";

    case nsIXULRuntime::ContentWin32kLockdownState::MissingRemoteWebGL:
      return "Win32k Lockdown disabled -- Missing Remote WebGL";

    case nsIXULRuntime::ContentWin32kLockdownState::MissingNonNativeTheming:
      return "Win32k Lockdown disabled -- Missing Non-Native Theming";

    case nsIXULRuntime::ContentWin32kLockdownState::DecodersArentRemote:
      return "Win32k Lockdown disabled -- Not all media decoders are remoted "
             "to Utility Process";

    case nsIXULRuntime::ContentWin32kLockdownState::DisabledByEnvVar:
      return "Win32k Lockdown disabled -- MOZ_ENABLE_WIN32K is set";

    case nsIXULRuntime::ContentWin32kLockdownState::DisabledByE10S:
      return "Win32k Lockdown disabled -- E10S is disabled";

    case nsIXULRuntime::ContentWin32kLockdownState::DisabledByUserPref:
      return "Win32k Lockdown disabled -- manually set "
             "security.sandbox.content.win32k-disable to false";

    case nsIXULRuntime::ContentWin32kLockdownState::EnabledByUserPref:
      return "Win32k Lockdown enabled -- manually set "
             "security.sandbox.content.win32k-disable to true";

    case nsIXULRuntime::ContentWin32kLockdownState::DisabledByControlGroup:
      return "Win32k Lockdown disabled -- user in Control Group";

    case nsIXULRuntime::ContentWin32kLockdownState::EnabledByTreatmentGroup:
      return "Win32k Lockdown enabled -- user in Treatment Group";

    case nsIXULRuntime::ContentWin32kLockdownState::DisabledByDefault:
      return "Win32k Lockdown disabled -- default value is false";

    case nsIXULRuntime::ContentWin32kLockdownState::EnabledByDefault:
      return "Win32k Lockdown enabled -- default value is true";

    case nsIXULRuntime::ContentWin32kLockdownState::
        IncompatibleMitigationPolicy:
      return "Win32k Lockdown disabled -- Incompatible Windows Exploit "
             "Protection policies enabled";
  }

  MOZ_CRASH("Should never reach here");
}

bool GetContentWin32kLockdownEnabled() {
  auto state = GetContentWin32kLockdownState();
  return state ==
             nsIXULRuntime::ContentWin32kLockdownState::EnabledByUserPref ||
         state == nsIXULRuntime::ContentWin32kLockdownState::
                      EnabledByTreatmentGroup ||
         state == nsIXULRuntime::ContentWin32kLockdownState::EnabledByDefault;
}

nsIXULRuntime::ContentWin32kLockdownState GetContentWin32kLockdownState() {
#ifdef XP_WIN

  static auto getLockdownState = [] {
    auto state = GetWin32kLockdownState();

    CrashReporter::RecordAnnotationCString(
        CrashReporter::Annotation::ContentSandboxWin32kState,
        ContentWin32kLockdownStateToString(state));

    return state;
  };

  static nsIXULRuntime::ContentWin32kLockdownState result = getLockdownState();
  return result;

#else  // XP_WIN

  return nsIXULRuntime::ContentWin32kLockdownState::OperatingSystemNotSupported;

#endif  // XP_WIN
}

#if defined(XP_WIN)
static bool IsWebglOutOfProcessEnabled() {
  if (StaticPrefs::webgl_out_of_process_force()) {
    return true;
  }

  // We have to check initialization state for gfxVars, because of early use in
  // child processes. In rare cases this could lead to the incorrect sandbox
  // level being reported, but not the incorrect one being set.
  if (gfx::gfxVars::IsInitialized() && !gfx::gfxVars::AllowWebglOop()) {
    return false;
  }

  return StaticPrefs::webgl_out_of_process();
}
#endif

int GetEffectiveContentSandboxLevel() {
  if (PR_GetEnv("MOZ_DISABLE_CONTENT_SANDBOX")) {
    return 0;
  }
  int level = StaticPrefs::security_sandbox_content_level_DoNotUseDirectly();
#if !defined(NIGHTLY_BUILD) && defined(XP_MACOSX)
  // On non-Nightly macOS, enforce a minimum sandbox level of 1.
  static const int minimumLevel = 1;
#elif defined(XP_WIN)
  // On Windows, enforce a minimum sandbox level of 6.
  static const int minimumLevel = 6;
#else
  static const int minimumLevel = 0;
#endif
  if (level < minimumLevel) {
    level = minimumLevel;
  }
#ifdef XP_LINUX
  // Level 1 was a configuration with default-deny seccomp-bpf but
  // which allowed direct filesystem access; that required additional
  // code for the syscall filter which was untested and tended to
  // bit-rot.  It was trivially escapable and was no longer being used
  // even for debugging, so it has been removed.
  //
  // If the content sandbox is enabled, enforce a minimum level of 2.
  static constexpr int kMinSupportedLevel = 2;

  if (level > 0 && level <= kMinSupportedLevel) {
    level = kMinSupportedLevel;
  }
  // Level 4 and up will break direct access to audio.
  if (level > 3 && !StaticPrefs::media_cubeb_sandbox()) {
    level = 3;
  }
  // Turn off ioctl lockdown in safe mode, until it's gotten more testing.
  if (level > 5 && gSafeMode) {
    level = 5;
  }
#endif
#if defined(XP_WIN)
  // Sandbox level 8, which uses a USER_RESTRICTED access token level, breaks if
  // prefs moving processing out of the content process are not the default.
  if (level >= 8 &&
      (!IsWebglOutOfProcessEnabled() ||
       !PDMFactory::AllDecodersAreRemote()
#  if defined(MOZ_WEBRTC) && !defined(MOZ_THUNDERBIRD)
       // These are only relevant if webrtc is present. Thunderbird currently
       // compiles with webrtc, but doesn't use it.
       || !StaticPrefs::network_process_enabled() ||
       !Preferences::GetBool("media.peerconnection.mtransport_process")
#  endif
           )) {
    level = 7;
  }
#endif

  return level;
}

bool IsContentSandboxEnabled() { return GetEffectiveContentSandboxLevel() > 0; }

int GetEffectiveSocketProcessSandboxLevel() {
  if (PR_GetEnv("MOZ_DISABLE_SOCKET_PROCESS_SANDBOX")) {
    return 0;
  }

  int level =
      StaticPrefs::security_sandbox_socket_process_level_DoNotUseDirectly();

#ifdef XP_LINUX
  // Turn off ioctl lockdown in safe mode, until it's gotten more testing.
  if (level > 1 && gSafeMode) {
    level = 1;
  }
#endif

  return level;
}

int GetEffectiveGpuSandboxLevel() {
  return StaticPrefs::security_sandbox_gpu_level();
}

#if defined(MOZ_PROFILE_GENERATE)
// It should only be allowed on instrumented builds, never on production
// builds.
#  if defined(XP_WIN)
bool GetLlvmProfileDir(std::wstring& parentPath) {
  bool rv = false;
  if (const wchar_t* llvmProfileFileEnv = _wgetenv(L"LLVM_PROFILE_FILE")) {
    std::wstring llvmProfileDir(llvmProfileFileEnv);
    const size_t found = llvmProfileDir.find_last_of(L"/\\");
    if (found != std::string::npos) {
      parentPath = llvmProfileDir.substr(0, found);
      parentPath.append(L"\\*");
      rv = true;
    }
  }
  return rv;
}
#  else   // defined(XP_WIN)
bool GetLlvmProfileDir(std::string& parentPath) {
  bool rv = false;
  if (const char* llvmProfileFileEnv = getenv("LLVM_PROFILE_FILE")) {
    std::string llvmProfileDir(llvmProfileFileEnv);
    const size_t found = llvmProfileDir.find_last_of("/\\");
    if (found != std::string::npos) {
      parentPath = llvmProfileDir.substr(0, found);
      rv = true;
    }
  }
  return rv;
}
#  endif  // defined(XP_WIN)
#endif    // defined(MOZ_PROFILE_GENERATE

#if defined(XP_MACOSX)
int ClampFlashSandboxLevel(const int aLevel) {
  const int minLevel = 0;
  const int maxLevel = 3;

  if (aLevel < minLevel) {
    return minLevel;
  }

  if (aLevel > maxLevel) {
    return maxLevel;
  }
  return aLevel;
}
#endif

class SandboxSettings final : public mozISandboxSettings {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_MOZISANDBOXSETTINGS

  SandboxSettings() = default;

 private:
  ~SandboxSettings() = default;
};

NS_IMPL_ISUPPORTS(SandboxSettings, mozISandboxSettings)

NS_IMETHODIMP SandboxSettings::GetEffectiveContentSandboxLevel(
    int32_t* aRetVal) {
  *aRetVal = mozilla::GetEffectiveContentSandboxLevel();
  return NS_OK;
}

NS_IMETHODIMP SandboxSettings::GetContentWin32kLockdownState(int32_t* aRetVal) {
  *aRetVal = static_cast<int32_t>(mozilla::GetContentWin32kLockdownState());
  return NS_OK;
}

NS_IMETHODIMP
SandboxSettings::GetContentWin32kLockdownStateString(nsAString& aString) {
  nsIXULRuntime::ContentWin32kLockdownState lockdownState =
      mozilla::GetContentWin32kLockdownState();
  aString = NS_ConvertASCIItoUTF16(
      mozilla::ContentWin32kLockdownStateToString(lockdownState));
  return NS_OK;
}

}  // namespace mozilla

NS_IMPL_COMPONENT_FACTORY(mozISandboxSettings) {
  return MakeAndAddRef<SandboxSettings>().downcast<nsISupports>();
}
