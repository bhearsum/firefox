/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NSSUBDOCUMENTFRAME_H_
#define NSSUBDOCUMENTFRAME_H_

#include "Units.h"
#include "mozilla/Attributes.h"
#include "mozilla/gfx/Matrix.h"
#include "nsAtomicContainerFrame.h"
#include "nsDisplayList.h"
#include "nsFrameLoader.h"
#include "nsIReflowCallback.h"

namespace mozilla {
class PresShell;
}  // namespace mozilla

namespace mozilla::layers {
class Layer;
class RenderRootStateManager;
class WebRenderLayerScrollData;
class WebRenderScrollData;
}  // namespace mozilla::layers

/******************************************************************************
 * nsSubDocumentFrame
 *****************************************************************************/
class nsSubDocumentFrame final : public nsAtomicContainerFrame,
                                 public nsIReflowCallback {
 public:
  NS_DECL_FRAMEARENA_HELPERS(nsSubDocumentFrame)

  explicit nsSubDocumentFrame(ComputedStyle* aStyle,
                              nsPresContext* aPresContext);

#ifdef DEBUG_FRAME_DUMP
  void List(FILE* out = stderr, const char* aPrefix = "",
            ListFlags aFlags = ListFlags()) const override;
  nsresult GetFrameName(nsAString& aResult) const override;
#endif

  NS_DECL_QUERYFRAME

  void Init(nsIContent* aContent, nsContainerFrame* aParent,
            nsIFrame* aPrevInFlow) override;

  void Destroy(DestroyContext&) override;

  nscoord IntrinsicISize(const mozilla::IntrinsicSizeInput& aInput,
                         mozilla::IntrinsicISizeType aType) override;

  mozilla::IntrinsicSize GetIntrinsicSize() override;
  mozilla::AspectRatio GetIntrinsicRatio() const override;

  SizeComputationResult ComputeSize(
      gfxContext* aRenderingContext, mozilla::WritingMode aWM,
      const mozilla::LogicalSize& aCBSize, nscoord aAvailableISize,
      const mozilla::LogicalSize& aMargin,
      const mozilla::LogicalSize& aBorderPadding,
      const mozilla::StyleSizeOverrides& aSizeOverrides,
      mozilla::ComputeSizeFlags aFlags) override;

  void Reflow(nsPresContext* aPresContext, ReflowOutput& aDesiredSize,
              const ReflowInput& aReflowInput,
              nsReflowStatus& aStatus) override;

  void BuildDisplayList(nsDisplayListBuilder* aBuilder,
                        const nsDisplayListSet& aLists) override;

  nsresult AttributeChanged(int32_t aNameSpaceID, nsAtom* aAttribute,
                            int32_t aModType) override;

  void DidSetComputedStyle(ComputedStyle* aOldComputedStyle) override;

  // if the content is "visibility:hidden", then just hide the view
  // and all our contents. We don't extend "visibility:hidden" to
  // the child content ourselves, since it belongs to a different
  // document and CSS doesn't inherit in there.
  bool SupportsVisibilityHidden() override { return false; }

#ifdef ACCESSIBILITY
  mozilla::a11y::AccType AccessibleType() override;
#endif

  mozilla::IntrinsicSize ComputeIntrinsicSize(
      bool aIgnoreContainment = false) const;

  nsIDocShell* GetDocShell() const;
  nsresult BeginSwapDocShells(nsIFrame* aOther);
  void EndSwapDocShells(nsIFrame* aOther);

  static void InsertViewsInReverseOrder(nsView* aSibling, nsView* aParent);
  static void EndSwapDocShellsForViews(nsView* aView);

  nsView* EnsureInnerView();
  nsPoint GetExtraOffset() const;
  nsIFrame* GetSubdocumentRootFrame();
  enum { IGNORE_PAINT_SUPPRESSION = 0x1 };
  mozilla::PresShell* GetSubdocumentPresShellForPainting(uint32_t aFlags);
  nsRect GetDestRect() const;
  nsRect GetDestRect(const nsRect& aConstraintRect) const;

  mozilla::LayoutDeviceIntSize GetInitialSubdocumentSize() const;
  mozilla::LayoutDeviceIntSize GetSubdocumentSize() const;

  bool ContentReactsToPointerEvents() const;

  // nsIReflowCallback
  bool ReflowFinished() override;
  void ReflowCallbackCanceled() override;

  /**
   * Return true if pointer event hit-testing should be allowed to target
   * content in the subdocument.
   */
  bool PassPointerEventsToChildren();

  void MaybeShowViewer() {
    if (!mDidCreateDoc && !mCallingShow) {
      ShowViewer();
    }
  }

  nsFrameLoader* FrameLoader() const;

  enum class RetainPaintData : bool { No, Yes };
  void ResetFrameLoader(RetainPaintData);
  void ClearRetainedPaintData();

  void ClearDisplayItems();

  void SubdocumentIntrinsicSizeOrRatioChanged();

  struct RemoteFramePaintData {
    mozilla::layers::LayersId mLayersId;
    mozilla::dom::TabId mTabId{0};
  };

  RemoteFramePaintData GetRemotePaintData() const;
  bool HasRetainedPaintData() const { return mRetainedRemoteFrame.isSome(); }

  const mozilla::gfx::MatrixScales& GetRasterScale() const {
    return mRasterScale;
  }
  void SetRasterScale(const mozilla::gfx::MatrixScales& aScale) {
    mRasterScale = aScale;
  }
  const Maybe<nsRect>& GetVisibleRect() const { return mVisibleRect; }
  void SetVisibleRect(const Maybe<nsRect>& aRect) { mVisibleRect = aRect; }

 protected:
  friend class AsyncFrameInit;

  void MaybeUpdateEmbedderColorScheme();
  void MaybeUpdateEmbedderZoom();
  void MaybeUpdateRemoteStyle(ComputedStyle* aOldComputedStyle = nullptr);
  void PropagateIsUnderHiddenEmbedderElement(bool aValue);
  void UpdateEmbeddedBrowsingContextDependentData();

  bool IsInline() const { return mIsInline; }

  // Show our document viewer. The document viewer is hidden via a script
  // runner, so that we can save and restore the presentation if we're
  // being reframed.
  void ShowViewer();

  nsView* GetViewInternal() const override { return mOuterView; }
  void SetViewInternal(nsView* aView) override { mOuterView = aView; }
  void CreateView();

  mutable RefPtr<nsFrameLoader> mFrameLoader;

  nsView* mOuterView;
  nsView* mInnerView;

  // When process-switching a remote tab, we might temporarily paint the old
  // one.
  Maybe<RemoteFramePaintData> mRetainedRemoteFrame;

  // The raster scale from our last paint.
  mozilla::gfx::MatrixScales mRasterScale;
  // The visible rect from our last paint.
  Maybe<nsRect> mVisibleRect;

  bool mIsInline : 1;
  bool mPostedReflowCallback : 1;
  bool mDidCreateDoc : 1;
  bool mCallingShow : 1;
  bool mIsInObjectOrEmbed : 1;
};

namespace mozilla {

/**
 * A nsDisplayRemote will graft a remote frame's shadow layer tree (for a given
 * nsFrameLoader) into its parent frame's layer tree.
 */
class nsDisplayRemote final : public nsPaintedDisplayItem {
  typedef mozilla::dom::TabId TabId;
  typedef mozilla::gfx::Matrix4x4 Matrix4x4;
  typedef mozilla::layers::EventRegionsOverride EventRegionsOverride;
  typedef mozilla::layers::LayersId LayersId;
  typedef mozilla::layers::StackingContextHelper StackingContextHelper;
  typedef mozilla::LayoutDeviceRect LayoutDeviceRect;
  typedef mozilla::LayoutDevicePoint LayoutDevicePoint;

 public:
  nsDisplayRemote(nsDisplayListBuilder* aBuilder, nsSubDocumentFrame* aFrame);

  void Paint(nsDisplayListBuilder* aBuilder, gfxContext* aCtx) override;

  bool CreateWebRenderCommands(
      mozilla::wr::DisplayListBuilder& aBuilder,
      mozilla::wr::IpcResourceUpdateQueue& aResources,
      const StackingContextHelper& aSc,
      mozilla::layers::RenderRootStateManager* aManager,
      nsDisplayListBuilder* aDisplayListBuilder) override;
  bool UpdateScrollData(
      mozilla::layers::WebRenderScrollData* aData,
      mozilla::layers::WebRenderLayerScrollData* aLayerData) override;

  NS_DISPLAY_DECL_NAME("Remote", TYPE_REMOTE)

 private:
  friend class nsDisplayItem;
  using RemoteFramePaintData = nsSubDocumentFrame::RemoteFramePaintData;

  nsFrameLoader* GetFrameLoader() const;

  RemoteFramePaintData mPaintData;
  LayoutDevicePoint mOffset;
  EventRegionsOverride mEventRegionsOverride;
};

}  // namespace mozilla

#endif /* NSSUBDOCUMENTFRAME_H_ */
