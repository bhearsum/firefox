/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GtkCompositorWidget.h"

#include "mozilla/gfx/gfxVars.h"
#include "mozilla/WidgetUtilsGtk.h"
#include "mozilla/widget/PlatformWidgetTypes.h"
#include "nsWindow.h"

#ifdef MOZ_X11
#  include "mozilla/X11Util.h"
#endif

#ifdef MOZ_WAYLAND
#  include "mozilla/layers/NativeLayerWayland.h"
#endif

#ifdef MOZ_LOGGING
#  undef LOG
#  define LOG(str, ...)                               \
    MOZ_LOG(IsPopup() ? gWidgetPopupLog : gWidgetLog, \
            mozilla::LogLevel::Debug,                 \
            ("[%p]: " str, mWidget.get(), ##__VA_ARGS__))
#endif /* MOZ_LOGGING */

namespace mozilla {
namespace widget {

GtkCompositorWidget::GtkCompositorWidget(
    const GtkCompositorWidgetInitData& aInitData,
    const layers::CompositorOptions& aOptions, RefPtr<nsWindow> aWindow)
    : CompositorWidget(aOptions),
      mWidget(std::move(aWindow)),
      mClientSize(LayoutDeviceIntSize(aInitData.InitialClientSize()),
                  "GtkCompositorWidget::mClientSize") {
#if defined(MOZ_X11)
  if (GdkIsX11Display()) {
    ConfigureX11Backend((Window)aInitData.XWindow());
    LOG("GtkCompositorWidget::GtkCompositorWidget() [%p] mXWindow %p\n",
        (void*)mWidget.get(), (void*)aInitData.XWindow());
  }
#endif
#if defined(MOZ_WAYLAND)
  if (GdkIsWaylandDisplay()) {
    ConfigureWaylandBackend();
    LOG("GtkCompositorWidget::GtkCompositorWidget() [%p] mWidget %p\n",
        (void*)mWidget.get(), (void*)mWidget);
  }
#endif
}

GtkCompositorWidget::~GtkCompositorWidget() {
  LOG("GtkCompositorWidget::~GtkCompositorWidget [%p]\n", (void*)mWidget.get());
  CleanupResources();
#ifdef MOZ_WAYLAND
  if (mNativeLayerRoot) {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "~GtkCompositorWidget::NativeLayerRootWayland::Shutdown()",
        [root = RefPtr{mNativeLayerRoot}]() -> void { root->Shutdown(); }));
  }
#endif
  RefPtr<nsIWidget> widget = mWidget.forget();
  NS_ReleaseOnMainThread("GtkCompositorWidget::mWidget", widget.forget());
}

already_AddRefed<gfx::DrawTarget> GtkCompositorWidget::StartRemoteDrawing() {
  return nullptr;
}
void GtkCompositorWidget::EndRemoteDrawing() {}

already_AddRefed<gfx::DrawTarget>
GtkCompositorWidget::StartRemoteDrawingInRegion(
    const LayoutDeviceIntRegion& aInvalidRegion) {
  return mProvider.StartRemoteDrawingInRegion(aInvalidRegion);
}

void GtkCompositorWidget::EndRemoteDrawingInRegion(
    gfx::DrawTarget* aDrawTarget, const LayoutDeviceIntRegion& aInvalidRegion) {
  mProvider.EndRemoteDrawingInRegion(aDrawTarget, aInvalidRegion);
}

nsIWidget* GtkCompositorWidget::RealWidget() { return mWidget; }

void GtkCompositorWidget::NotifyClientSizeChanged(
    const LayoutDeviceIntSize& aClientSize) {
  LOG("GtkCompositorWidget::NotifyClientSizeChanged() to %d x %d",
      aClientSize.width, aClientSize.height);

  auto size = mClientSize.Lock();
  *size = aClientSize;
}

void GtkCompositorWidget::NotifyFullscreenChanged(bool aIsFullscreen) {
#ifdef MOZ_WAYLAND
  if (mNativeLayerRoot) {
    LOG("GtkCompositorWidget::NotifyFullscreenChanged() [%d]", aIsFullscreen);
    mNativeLayerRoot->NotifyFullscreenChanged(aIsFullscreen);
  }
#endif
}

LayoutDeviceIntSize GtkCompositorWidget::GetClientSize() {
  auto size = mClientSize.Lock();
  return *size;
}

EGLNativeWindowType GtkCompositorWidget::GetEGLNativeWindow() {
  EGLNativeWindowType window = nullptr;
  if (mWidget) {
    window = (EGLNativeWindowType)mWidget->GetNativeData(NS_NATIVE_EGL_WINDOW);
  }
#if defined(MOZ_X11)
  else {
    window = (EGLNativeWindowType)mProvider.GetXWindow();
  }
#endif
  LOG("GtkCompositorWidget::GetEGLNativeWindow [%p] window %p\n", mWidget.get(),
      window);
  return window;
}

bool GtkCompositorWidget::SetEGLNativeWindowSize(
    const LayoutDeviceIntSize& aEGLWindowSize) {
#if defined(MOZ_WAYLAND)
  // We explicitly need to set EGL window size on Wayland only.
  if (GdkIsWaylandDisplay() && mWidget) {
    return mWidget->SetEGLNativeWindowSize(aEGLWindowSize);
  }
#endif
  return true;
}

LayoutDeviceIntRegion GtkCompositorWidget::GetTransparentRegion() {
  LayoutDeviceIntRegion fullRegion(
      LayoutDeviceIntRect(LayoutDeviceIntPoint(), GetClientSize()));
  if (mWidget) {
    fullRegion.SubOut(mWidget->GetOpaqueRegion());
  }
  return fullRegion;
}

#ifdef MOZ_WAYLAND
RefPtr<mozilla::layers::NativeLayerRoot>
GtkCompositorWidget::GetNativeLayerRoot() {
  if (gfx::gfxVars::UseWebRenderCompositor()) {
    if (!mNativeLayerRoot) {
      LOG("GtkCompositorWidget::GetNativeLayerRoot [%p] create",
          (void*)mWidget.get());
      MOZ_ASSERT(mWidget && mWidget->GetMozContainer());
      mNativeLayerRoot = layers::NativeLayerRootWayland::Create(
          MOZ_WL_SURFACE(mWidget->GetMozContainer()));
      mNativeLayerRoot->Init();
    }
    return mNativeLayerRoot;
  }
  return nullptr;
}
#endif

void GtkCompositorWidget::CleanupResources() {
  LOG("GtkCompositorWidget::CleanupResources [%p]\n", (void*)mWidget.get());
  mProvider.CleanupResources();
}

#if defined(MOZ_WAYLAND)
void GtkCompositorWidget::ConfigureWaylandBackend() {
  mProvider.Initialize(this);
}
#endif

#if defined(MOZ_X11)
void GtkCompositorWidget::ConfigureX11Backend(Window aXWindow) {
  // We don't have X window yet.
  if (!aXWindow) {
    mProvider.CleanupResources();
    return;
  }
  // Initialize the window surface provider
  mProvider.Initialize(aXWindow);
}
#endif

void GtkCompositorWidget::SetRenderingSurface(const uintptr_t aXWindow) {
  LOG("GtkCompositorWidget::SetRenderingSurface() [%p]\n", mWidget.get());

#if defined(MOZ_WAYLAND)
  if (GdkIsWaylandDisplay()) {
    LOG("  configure widget %p\n", mWidget.get());
    ConfigureWaylandBackend();
  }
#endif
#if defined(MOZ_X11)
  if (GdkIsX11Display()) {
    LOG("  configure XWindow %p\n", (void*)aXWindow);
    ConfigureX11Backend((Window)aXWindow);
  }
#endif
}

#ifdef MOZ_LOGGING
bool GtkCompositorWidget::IsPopup() {
  return mWidget ? mWidget->IsPopup() : false;
}
#endif

UniquePtr<WaylandSurfaceLock> GtkCompositorWidget::LockSurface() {
  return mWidget ? mWidget->LockSurface() : nullptr;
}

}  // namespace widget
}  // namespace mozilla
