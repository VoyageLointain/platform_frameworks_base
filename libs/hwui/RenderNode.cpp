/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "RenderNode.h"

#include "DamageAccumulator.h"
#include "Debug.h"
#if HWUI_NEW_OPS
#include "BakedOpRenderer.h"
#include "RecordedOp.h"
#include "OpDumper.h"
#endif
#include "DisplayListOp.h"
#include "LayerRenderer.h"
#include "OpenGLRenderer.h"
#include "TreeInfo.h"
#include "utils/MathUtils.h"
#include "utils/TraceUtils.h"
#include "renderthread/CanvasContext.h"

#include "protos/hwui.pb.h"
#include "protos/ProtoHelpers.h"

#include <algorithm>
#include <sstream>
#include <string>

namespace android {
namespace uirenderer {

void RenderNode::debugDumpLayers(const char* prefix) {
#if HWUI_NEW_OPS
    LOG_ALWAYS_FATAL("TODO: dump layer");
#else
    if (mLayer) {
        ALOGD("%sNode %p (%s) has layer %p (fbo = %u, wasBuildLayered = %s)",
                prefix, this, getName(), mLayer, mLayer->getFbo(),
                mLayer->wasBuildLayered ? "true" : "false");
    }
#endif
    if (mDisplayList) {
        for (auto&& child : mDisplayList->getChildren()) {
            child->renderNode->debugDumpLayers(prefix);
        }
    }
}

RenderNode::RenderNode()
        : mDirtyPropertyFields(0)
        , mNeedsDisplayListSync(false)
        , mDisplayList(nullptr)
        , mStagingDisplayList(nullptr)
        , mAnimatorManager(*this)
        , mParentCount(0) {
}

RenderNode::~RenderNode() {
    deleteDisplayList(nullptr);
    delete mStagingDisplayList;
#if HWUI_NEW_OPS
    LOG_ALWAYS_FATAL_IF(mLayer, "layer missed detachment!");
#else
    if (mLayer) {
        ALOGW("Memory Warning: Layer %p missed its detachment, held on to for far too long!", mLayer);
        mLayer->postDecStrong();
        mLayer = nullptr;
    }
#endif
}

void RenderNode::setStagingDisplayList(DisplayList* displayList, TreeObserver* observer) {
    mNeedsDisplayListSync = true;
    delete mStagingDisplayList;
    mStagingDisplayList = displayList;
    // If mParentCount == 0 we are the sole reference to this RenderNode,
    // so immediately free the old display list
    if (!mParentCount && !mStagingDisplayList) {
        deleteDisplayList(observer);
    }
}

/**
 * This function is a simplified version of replay(), where we simply retrieve and log the
 * display list. This function should remain in sync with the replay() function.
 */
#if HWUI_NEW_OPS
void RenderNode::output(uint32_t level, const char* label) {
    ALOGD("%s (%s %p%s%s%s%s%s)",
            label,
            getName(),
            this,
            (MathUtils::isZero(properties().getAlpha()) ? ", zero alpha" : ""),
            (properties().hasShadow() ? ", casting shadow" : ""),
            (isRenderable() ? "" : ", empty"),
            (properties().getProjectBackwards() ? ", projected" : ""),
            (mLayer != nullptr ? ", on HW Layer" : ""));
    properties().debugOutputProperties(level + 1);

    if (mDisplayList) {
        for (auto&& op : mDisplayList->getOps()) {
            std::stringstream strout;
            OpDumper::dump(*op, strout, level + 1);
            if (op->opId == RecordedOpId::RenderNodeOp) {
                auto rnOp = reinterpret_cast<const RenderNodeOp*>(op);
                rnOp->renderNode->output(level + 1, strout.str().c_str());
            } else {
                ALOGD("%s", strout.str().c_str());
            }
        }
    }
    ALOGD("%*s/RenderNode(%s %p)", level * 2, "", getName(), this);
}
#else
void RenderNode::output(uint32_t level) {
    ALOGD("%*sStart display list (%p, %s%s%s%s%s%s)", (level - 1) * 2, "", this,
            getName(),
            (MathUtils::isZero(properties().getAlpha()) ? ", zero alpha" : ""),
            (properties().hasShadow() ? ", casting shadow" : ""),
            (isRenderable() ? "" : ", empty"),
            (properties().getProjectBackwards() ? ", projected" : ""),
            (mLayer != nullptr ? ", on HW Layer" : ""));
    ALOGD("%*s%s %d", level * 2, "", "Save", SaveFlags::MatrixClip);
    properties().debugOutputProperties(level);
    if (mDisplayList) {
        // TODO: consider printing the chunk boundaries here
        for (auto&& op : mDisplayList->getOps()) {
            op->output(level, DisplayListOp::kOpLogFlag_Recurse);
        }
    }
    ALOGD("%*sDone (%p, %s)", (level - 1) * 2, "", this, getName());
    }
#endif

void RenderNode::copyTo(proto::RenderNode *pnode) {
    pnode->set_id(static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(this)));
    pnode->set_name(mName.string(), mName.length());

    proto::RenderProperties* pprops = pnode->mutable_properties();
    pprops->set_left(properties().getLeft());
    pprops->set_top(properties().getTop());
    pprops->set_right(properties().getRight());
    pprops->set_bottom(properties().getBottom());
    pprops->set_clip_flags(properties().getClippingFlags());
    pprops->set_alpha(properties().getAlpha());
    pprops->set_translation_x(properties().getTranslationX());
    pprops->set_translation_y(properties().getTranslationY());
    pprops->set_translation_z(properties().getTranslationZ());
    pprops->set_elevation(properties().getElevation());
    pprops->set_rotation(properties().getRotation());
    pprops->set_rotation_x(properties().getRotationX());
    pprops->set_rotation_y(properties().getRotationY());
    pprops->set_scale_x(properties().getScaleX());
    pprops->set_scale_y(properties().getScaleY());
    pprops->set_pivot_x(properties().getPivotX());
    pprops->set_pivot_y(properties().getPivotY());
    pprops->set_has_overlapping_rendering(properties().getHasOverlappingRendering());
    pprops->set_pivot_explicitly_set(properties().isPivotExplicitlySet());
    pprops->set_project_backwards(properties().getProjectBackwards());
    pprops->set_projection_receiver(properties().isProjectionReceiver());
    set(pprops->mutable_clip_bounds(), properties().getClipBounds());

    const Outline& outline = properties().getOutline();
    if (outline.getType() != Outline::Type::None) {
        proto::Outline* poutline = pprops->mutable_outline();
        poutline->clear_path();
        if (outline.getType() == Outline::Type::Empty) {
            poutline->set_type(proto::Outline_Type_Empty);
        } else if (outline.getType() == Outline::Type::ConvexPath) {
            poutline->set_type(proto::Outline_Type_ConvexPath);
            if (const SkPath* path = outline.getPath()) {
                set(poutline->mutable_path(), *path);
            }
        } else if (outline.getType() == Outline::Type::RoundRect) {
            poutline->set_type(proto::Outline_Type_RoundRect);
        } else {
            ALOGW("Uknown outline type! %d", static_cast<int>(outline.getType()));
            poutline->set_type(proto::Outline_Type_None);
        }
        poutline->set_should_clip(outline.getShouldClip());
        poutline->set_alpha(outline.getAlpha());
        poutline->set_radius(outline.getRadius());
        set(poutline->mutable_bounds(), outline.getBounds());
    } else {
        pprops->clear_outline();
    }

    const RevealClip& revealClip = properties().getRevealClip();
    if (revealClip.willClip()) {
        proto::RevealClip* prevealClip = pprops->mutable_reveal_clip();
        prevealClip->set_x(revealClip.getX());
        prevealClip->set_y(revealClip.getY());
        prevealClip->set_radius(revealClip.getRadius());
    } else {
        pprops->clear_reveal_clip();
    }

    pnode->clear_children();
    if (mDisplayList) {
        for (auto&& child : mDisplayList->getChildren()) {
            child->renderNode->copyTo(pnode->add_children());
        }
    }
}

int RenderNode::getDebugSize() {
    int size = sizeof(RenderNode);
    if (mStagingDisplayList) {
        size += mStagingDisplayList->getUsedSize();
    }
    if (mDisplayList && mDisplayList != mStagingDisplayList) {
        size += mDisplayList->getUsedSize();
    }
    return size;
}

void RenderNode::prepareTree(TreeInfo& info) {
    ATRACE_CALL();
    LOG_ALWAYS_FATAL_IF(!info.damageAccumulator, "DamageAccumulator missing");

    // Functors don't correctly handle stencil usage of overdraw debugging - shove 'em in a layer.
    bool functorsNeedLayer = Properties::debugOverdraw;

    prepareTreeImpl(info, functorsNeedLayer);
}

void RenderNode::addAnimator(const sp<BaseRenderNodeAnimator>& animator) {
    mAnimatorManager.addAnimator(animator);
}

void RenderNode::removeAnimator(const sp<BaseRenderNodeAnimator>& animator) {
    mAnimatorManager.removeAnimator(animator);
}

void RenderNode::damageSelf(TreeInfo& info) {
    if (isRenderable()) {
        if (properties().getClipDamageToBounds()) {
            info.damageAccumulator->dirty(0, 0, properties().getWidth(), properties().getHeight());
        } else {
            // Hope this is big enough?
            // TODO: Get this from the display list ops or something
            info.damageAccumulator->dirty(DIRTY_MIN, DIRTY_MIN, DIRTY_MAX, DIRTY_MAX);
        }
    }
}

void RenderNode::prepareLayer(TreeInfo& info, uint32_t dirtyMask) {
    LayerType layerType = properties().effectiveLayerType();
    if (CC_UNLIKELY(layerType == LayerType::RenderLayer)) {
        // Damage applied so far needs to affect our parent, but does not require
        // the layer to be updated. So we pop/push here to clear out the current
        // damage and get a clean state for display list or children updates to
        // affect, which will require the layer to be updated
        info.damageAccumulator->popTransform();
        info.damageAccumulator->pushTransform(this);
        if (dirtyMask & DISPLAY_LIST) {
            damageSelf(info);
        }
    }
}

static layer_t* createLayer(RenderState& renderState, uint32_t width, uint32_t height) {
#if HWUI_NEW_OPS
    return renderState.layerPool().get(renderState, width, height);
#else
    return LayerRenderer::createRenderLayer(renderState, width, height);
#endif
}

static void destroyLayer(layer_t* layer) {
#if HWUI_NEW_OPS
    RenderState& renderState = layer->renderState;
    renderState.layerPool().putOrDelete(layer);
#else
    LayerRenderer::destroyLayer(layer);
#endif
}

static bool layerMatchesWidthAndHeight(layer_t* layer, int width, int height) {
#if HWUI_NEW_OPS
    return layer->viewportWidth == (uint32_t) width && layer->viewportHeight == (uint32_t)height;
#else
    return layer->layer.getWidth() == width && layer->layer.getHeight() == height;
#endif
}

void RenderNode::pushLayerUpdate(TreeInfo& info) {
    LayerType layerType = properties().effectiveLayerType();
    // If we are not a layer OR we cannot be rendered (eg, view was detached)
    // we need to destroy any Layers we may have had previously
    if (CC_LIKELY(layerType != LayerType::RenderLayer) || CC_UNLIKELY(!isRenderable())) {
        if (CC_UNLIKELY(mLayer)) {
            destroyLayer(mLayer);
            mLayer = nullptr;
        }
        return;
    }

    bool transformUpdateNeeded = false;
    if (!mLayer) {
        mLayer = createLayer(info.canvasContext.getRenderState(), getWidth(), getHeight());
#if !HWUI_NEW_OPS
        applyLayerPropertiesToLayer(info);
#endif
        damageSelf(info);
        transformUpdateNeeded = true;
    } else if (!layerMatchesWidthAndHeight(mLayer, getWidth(), getHeight())) {
#if HWUI_NEW_OPS
        // TODO: remove now irrelevant, currently enqueued damage (respecting damage ordering)
        // Or, ideally, maintain damage between frames on node/layer so ordering is always correct
        RenderState& renderState = mLayer->renderState;
        if (properties().fitsOnLayer()) {
            mLayer = renderState.layerPool().resize(mLayer, getWidth(), getHeight());
        } else {
#else
        if (!LayerRenderer::resizeLayer(mLayer, getWidth(), getHeight())) {
#endif
            destroyLayer(mLayer);
            mLayer = nullptr;
        }
        damageSelf(info);
        transformUpdateNeeded = true;
    }

    SkRect dirty;
    info.damageAccumulator->peekAtDirty(&dirty);

    if (!mLayer) {
        Caches::getInstance().dumpMemoryUsage();
        if (info.errorHandler) {
            std::ostringstream err;
            err << "Unable to create layer for " << getName();
            const int maxTextureSize = Caches::getInstance().maxTextureSize;
            if (getWidth() > maxTextureSize || getHeight() > maxTextureSize) {
                err << ", size " << getWidth() << "x" << getHeight()
                        << " exceeds max size " << maxTextureSize;
            } else {
                err << ", see logcat for more info";
            }
            info.errorHandler->onError(err.str());
        }
        return;
    }

    if (transformUpdateNeeded && mLayer) {
        // update the transform in window of the layer to reset its origin wrt light source position
        Matrix4 windowTransform;
        info.damageAccumulator->computeCurrentTransform(&windowTransform);
        mLayer->setWindowTransform(windowTransform);
    }

#if HWUI_NEW_OPS
    info.layerUpdateQueue->enqueueLayerWithDamage(this, dirty);
#else
    if (dirty.intersect(0, 0, getWidth(), getHeight())) {
        dirty.roundOut(&dirty);
        mLayer->updateDeferred(this, dirty.fLeft, dirty.fTop, dirty.fRight, dirty.fBottom);
    }
    // This is not inside the above if because we may have called
    // updateDeferred on a previous prepare pass that didn't have a renderer
    if (info.renderer && mLayer->deferredUpdateScheduled) {
        info.renderer->pushLayerUpdate(mLayer);
    }
#endif

    // There might be prefetched layers that need to be accounted for.
    // That might be us, so tell CanvasContext that this layer is in the
    // tree and should not be destroyed.
    info.canvasContext.markLayerInUse(this);
}

/**
 * Traverse down the the draw tree to prepare for a frame.
 *
 * MODE_FULL = UI Thread-driven (thus properties must be synced), otherwise RT driven
 *
 * While traversing down the tree, functorsNeedLayer flag is set to true if anything that uses the
 * stencil buffer may be needed. Views that use a functor to draw will be forced onto a layer.
 */
void RenderNode::prepareTreeImpl(TreeInfo& info, bool functorsNeedLayer) {
    info.damageAccumulator->pushTransform(this);

    if (info.mode == TreeInfo::MODE_FULL) {
        pushStagingPropertiesChanges(info);
    }
    uint32_t animatorDirtyMask = 0;
    if (CC_LIKELY(info.runAnimations)) {
        animatorDirtyMask = mAnimatorManager.animate(info);
    }

    bool willHaveFunctor = false;
    if (info.mode == TreeInfo::MODE_FULL && mStagingDisplayList) {
        willHaveFunctor = !mStagingDisplayList->getFunctors().empty();
    } else if (mDisplayList) {
        willHaveFunctor = !mDisplayList->getFunctors().empty();
    }
    bool childFunctorsNeedLayer = mProperties.prepareForFunctorPresence(
            willHaveFunctor, functorsNeedLayer);

    if (CC_UNLIKELY(mPositionListener.get())) {
        mPositionListener->onPositionUpdated(*this, info);
    }

    prepareLayer(info, animatorDirtyMask);
    if (info.mode == TreeInfo::MODE_FULL) {
        pushStagingDisplayListChanges(info);
    }
    prepareSubTree(info, childFunctorsNeedLayer, mDisplayList);
    pushLayerUpdate(info);

    for (auto& vectorDrawable : mDisplayList->getVectorDrawables()) {
        // If any vector drawable in the display list needs update, damage the node.
        if (vectorDrawable->isDirty()) {
            damageSelf(info);
        }
        vectorDrawable->setPropertyChangeWillBeConsumed(true);
    }

    info.damageAccumulator->popTransform();
}

void RenderNode::syncProperties() {
    mProperties = mStagingProperties;
}

void RenderNode::pushStagingPropertiesChanges(TreeInfo& info) {
    // Push the animators first so that setupStartValueIfNecessary() is called
    // before properties() is trampled by stagingProperties(), as they are
    // required by some animators.
    if (CC_LIKELY(info.runAnimations)) {
        mAnimatorManager.pushStaging();
    }
    if (mDirtyPropertyFields) {
        mDirtyPropertyFields = 0;
        damageSelf(info);
        info.damageAccumulator->popTransform();
        syncProperties();
#if !HWUI_NEW_OPS
        applyLayerPropertiesToLayer(info);
#endif
        // We could try to be clever and only re-damage if the matrix changed.
        // However, we don't need to worry about that. The cost of over-damaging
        // here is only going to be a single additional map rect of this node
        // plus a rect join(). The parent's transform (and up) will only be
        // performed once.
        info.damageAccumulator->pushTransform(this);
        damageSelf(info);
    }
}

#if !HWUI_NEW_OPS
void RenderNode::applyLayerPropertiesToLayer(TreeInfo& info) {
    if (CC_LIKELY(!mLayer)) return;

    const LayerProperties& props = properties().layerProperties();
    mLayer->setAlpha(props.alpha(), props.xferMode());
    mLayer->setColorFilter(props.colorFilter());
    mLayer->setBlend(props.needsBlending());
}
#endif

void RenderNode::syncDisplayList(TreeObserver* observer) {
    // Make sure we inc first so that we don't fluctuate between 0 and 1,
    // which would thrash the layer cache
    if (mStagingDisplayList) {
        for (auto&& child : mStagingDisplayList->getChildren()) {
            child->renderNode->incParentRefCount();
        }
    }
    deleteDisplayList(observer);
    mDisplayList = mStagingDisplayList;
    mStagingDisplayList = nullptr;
    if (mDisplayList) {
        for (auto& iter : mDisplayList->getFunctors()) {
            (*iter.functor)(DrawGlInfo::kModeSync, nullptr);
        }
        for (auto& vectorDrawable : mDisplayList->getVectorDrawables()) {
            vectorDrawable->syncProperties();
        }
    }
}

void RenderNode::pushStagingDisplayListChanges(TreeInfo& info) {
    if (mNeedsDisplayListSync) {
        mNeedsDisplayListSync = false;
        // Damage with the old display list first then the new one to catch any
        // changes in isRenderable or, in the future, bounds
        damageSelf(info);
        syncDisplayList(info.observer);
        damageSelf(info);
    }
}

void RenderNode::deleteDisplayList(TreeObserver* observer) {
    if (mDisplayList) {
        for (auto&& child : mDisplayList->getChildren()) {
            child->renderNode->decParentRefCount(observer);
        }
    }
    delete mDisplayList;
    mDisplayList = nullptr;
}

void RenderNode::prepareSubTree(TreeInfo& info, bool functorsNeedLayer, DisplayList* subtree) {
    if (subtree) {
        TextureCache& cache = Caches::getInstance().textureCache;
        info.out.hasFunctors |= subtree->getFunctors().size();
        for (auto&& bitmapResource : subtree->getBitmapResources()) {
            void* ownerToken = &info.canvasContext;
            info.prepareTextures = cache.prefetchAndMarkInUse(ownerToken, bitmapResource);
        }
        for (auto&& op : subtree->getChildren()) {
            RenderNode* childNode = op->renderNode;
#if HWUI_NEW_OPS
            info.damageAccumulator->pushTransform(&op->localMatrix);
            bool childFunctorsNeedLayer = functorsNeedLayer; // TODO! || op->mRecordedWithPotentialStencilClip;
#else
            info.damageAccumulator->pushTransform(&op->localMatrix);
            bool childFunctorsNeedLayer = functorsNeedLayer
                    // Recorded with non-rect clip, or canvas-rotated by parent
                    || op->mRecordedWithPotentialStencilClip;
#endif
            childNode->prepareTreeImpl(info, childFunctorsNeedLayer);
            info.damageAccumulator->popTransform();
        }
    }
}

void RenderNode::destroyHardwareResources(TreeObserver* observer) {
    if (mLayer) {
        destroyLayer(mLayer);
        mLayer = nullptr;
    }
    if (mDisplayList) {
        for (auto&& child : mDisplayList->getChildren()) {
            child->renderNode->destroyHardwareResources(observer);
        }
        if (mNeedsDisplayListSync) {
            // Next prepare tree we are going to push a new display list, so we can
            // drop our current one now
            deleteDisplayList(observer);
        }
    }
}

void RenderNode::decParentRefCount(TreeObserver* observer) {
    LOG_ALWAYS_FATAL_IF(!mParentCount, "already 0!");
    mParentCount--;
    if (!mParentCount) {
        if (observer) {
            observer->onMaybeRemovedFromTree(this);
        }
        // If a child of ours is being attached to our parent then this will incorrectly
        // destroy its hardware resources. However, this situation is highly unlikely
        // and the failure is "just" that the layer is re-created, so this should
        // be safe enough
        destroyHardwareResources(observer);
    }
}

/*
 * For property operations, we pass a savecount of 0, since the operations aren't part of the
 * displaylist, and thus don't have to compensate for the record-time/playback-time discrepancy in
 * base saveCount (i.e., how RestoreToCount uses saveCount + properties().getCount())
 */
#define PROPERTY_SAVECOUNT 0

template <class T>
void RenderNode::setViewProperties(OpenGLRenderer& renderer, T& handler) {
#if DEBUG_DISPLAY_LIST
    properties().debugOutputProperties(handler.level() + 1);
#endif
    if (properties().getLeft() != 0 || properties().getTop() != 0) {
        renderer.translate(properties().getLeft(), properties().getTop());
    }
    if (properties().getStaticMatrix()) {
        renderer.concatMatrix(*properties().getStaticMatrix());
    } else if (properties().getAnimationMatrix()) {
        renderer.concatMatrix(*properties().getAnimationMatrix());
    }
    if (properties().hasTransformMatrix()) {
        if (properties().isTransformTranslateOnly()) {
            renderer.translate(properties().getTranslationX(), properties().getTranslationY());
        } else {
            renderer.concatMatrix(*properties().getTransformMatrix());
        }
    }
    const bool isLayer = properties().effectiveLayerType() != LayerType::None;
    int clipFlags = properties().getClippingFlags();
    if (properties().getAlpha() < 1) {
        if (isLayer) {
            clipFlags &= ~CLIP_TO_BOUNDS; // bounds clipping done by layer
        }
        if (CC_LIKELY(isLayer || !properties().getHasOverlappingRendering())) {
            // simply scale rendering content's alpha
            renderer.scaleAlpha(properties().getAlpha());
        } else {
            // savelayer needed to create an offscreen buffer
            Rect layerBounds(0, 0, getWidth(), getHeight());
            if (clipFlags) {
                properties().getClippingRectForFlags(clipFlags, &layerBounds);
                clipFlags = 0; // all clipping done by savelayer
            }
            SaveLayerOp* op = new (handler.allocator()) SaveLayerOp(
                    layerBounds.left, layerBounds.top,
                    layerBounds.right, layerBounds.bottom,
                    (int) (properties().getAlpha() * 255),
                    SaveFlags::HasAlphaLayer | SaveFlags::ClipToLayer);
            handler(op, PROPERTY_SAVECOUNT, properties().getClipToBounds());
        }

        if (CC_UNLIKELY(ATRACE_ENABLED() && properties().promotedToLayer())) {
            // pretend alpha always causes savelayer to warn about
            // performance problem affecting old versions
            ATRACE_FORMAT("%s alpha caused saveLayer %dx%d", getName(),
                    static_cast<int>(getWidth()),
                    static_cast<int>(getHeight()));
        }
    }
    if (clipFlags) {
        Rect clipRect;
        properties().getClippingRectForFlags(clipFlags, &clipRect);
        ClipRectOp* op = new (handler.allocator()) ClipRectOp(
                clipRect.left, clipRect.top, clipRect.right, clipRect.bottom,
                SkRegion::kIntersect_Op);
        handler(op, PROPERTY_SAVECOUNT, properties().getClipToBounds());
    }

    // TODO: support nesting round rect clips
    if (mProperties.getRevealClip().willClip()) {
        Rect bounds;
        mProperties.getRevealClip().getBounds(&bounds);
        renderer.setClippingRoundRect(handler.allocator(), bounds, mProperties.getRevealClip().getRadius());
    } else if (mProperties.getOutline().willClip()) {
        renderer.setClippingOutline(handler.allocator(), &(mProperties.getOutline()));
    }
}

/**
 * Apply property-based transformations to input matrix
 *
 * If true3dTransform is set to true, the transform applied to the input matrix will use true 4x4
 * matrix computation instead of the Skia 3x3 matrix + camera hackery.
 */
void RenderNode::applyViewPropertyTransforms(mat4& matrix, bool true3dTransform) const {
    if (properties().getLeft() != 0 || properties().getTop() != 0) {
        matrix.translate(properties().getLeft(), properties().getTop());
    }
    if (properties().getStaticMatrix()) {
        mat4 stat(*properties().getStaticMatrix());
        matrix.multiply(stat);
    } else if (properties().getAnimationMatrix()) {
        mat4 anim(*properties().getAnimationMatrix());
        matrix.multiply(anim);
    }

    bool applyTranslationZ = true3dTransform && !MathUtils::isZero(properties().getZ());
    if (properties().hasTransformMatrix() || applyTranslationZ) {
        if (properties().isTransformTranslateOnly()) {
            matrix.translate(properties().getTranslationX(), properties().getTranslationY(),
                    true3dTransform ? properties().getZ() : 0.0f);
        } else {
            if (!true3dTransform) {
                matrix.multiply(*properties().getTransformMatrix());
            } else {
                mat4 true3dMat;
                true3dMat.loadTranslate(
                        properties().getPivotX() + properties().getTranslationX(),
                        properties().getPivotY() + properties().getTranslationY(),
                        properties().getZ());
                true3dMat.rotate(properties().getRotationX(), 1, 0, 0);
                true3dMat.rotate(properties().getRotationY(), 0, 1, 0);
                true3dMat.rotate(properties().getRotation(), 0, 0, 1);
                true3dMat.scale(properties().getScaleX(), properties().getScaleY(), 1);
                true3dMat.translate(-properties().getPivotX(), -properties().getPivotY());

                matrix.multiply(true3dMat);
            }
        }
    }
}

/**
 * Organizes the DisplayList hierarchy to prepare for background projection reordering.
 *
 * This should be called before a call to defer() or drawDisplayList()
 *
 * Each DisplayList that serves as a 3d root builds its list of composited children,
 * which are flagged to not draw in the standard draw loop.
 */
void RenderNode::computeOrdering() {
    ATRACE_CALL();
    mProjectedNodes.clear();

    // TODO: create temporary DDLOp and call computeOrderingImpl on top DisplayList so that
    // transform properties are applied correctly to top level children
    if (mDisplayList == nullptr) return;
    for (unsigned int i = 0; i < mDisplayList->getChildren().size(); i++) {
        renderNodeOp_t* childOp = mDisplayList->getChildren()[i];
        childOp->renderNode->computeOrderingImpl(childOp, &mProjectedNodes, &mat4::identity());
    }
}

void RenderNode::computeOrderingImpl(
        renderNodeOp_t* opState,
        std::vector<renderNodeOp_t*>* compositedChildrenOfProjectionSurface,
        const mat4* transformFromProjectionSurface) {
    mProjectedNodes.clear();
    if (mDisplayList == nullptr || mDisplayList->isEmpty()) return;

    // TODO: should avoid this calculation in most cases
    // TODO: just calculate single matrix, down to all leaf composited elements
    Matrix4 localTransformFromProjectionSurface(*transformFromProjectionSurface);
    localTransformFromProjectionSurface.multiply(opState->localMatrix);

    if (properties().getProjectBackwards()) {
        // composited projectee, flag for out of order draw, save matrix, and store in proj surface
        opState->skipInOrderDraw = true;
        opState->transformFromCompositingAncestor = localTransformFromProjectionSurface;
        compositedChildrenOfProjectionSurface->push_back(opState);
    } else {
        // standard in order draw
        opState->skipInOrderDraw = false;
    }

    if (mDisplayList->getChildren().size() > 0) {
        const bool isProjectionReceiver = mDisplayList->projectionReceiveIndex >= 0;
        bool haveAppliedPropertiesToProjection = false;
        for (unsigned int i = 0; i < mDisplayList->getChildren().size(); i++) {
            renderNodeOp_t* childOp = mDisplayList->getChildren()[i];
            RenderNode* child = childOp->renderNode;

            std::vector<renderNodeOp_t*>* projectionChildren = nullptr;
            const mat4* projectionTransform = nullptr;
            if (isProjectionReceiver && !child->properties().getProjectBackwards()) {
                // if receiving projections, collect projecting descendant

                // Note that if a direct descendant is projecting backwards, we pass its
                // grandparent projection collection, since it shouldn't project onto its
                // parent, where it will already be drawing.
                projectionChildren = &mProjectedNodes;
                projectionTransform = &mat4::identity();
            } else {
                if (!haveAppliedPropertiesToProjection) {
                    applyViewPropertyTransforms(localTransformFromProjectionSurface);
                    haveAppliedPropertiesToProjection = true;
                }
                projectionChildren = compositedChildrenOfProjectionSurface;
                projectionTransform = &localTransformFromProjectionSurface;
            }
            child->computeOrderingImpl(childOp, projectionChildren, projectionTransform);
        }
    }
}

class DeferOperationHandler {
public:
    DeferOperationHandler(DeferStateStruct& deferStruct, int level)
        : mDeferStruct(deferStruct), mLevel(level) {}
    inline void operator()(DisplayListOp* operation, int saveCount, bool clipToBounds) {
        operation->defer(mDeferStruct, saveCount, mLevel, clipToBounds);
    }
    inline LinearAllocator& allocator() { return *(mDeferStruct.mAllocator); }
    inline void startMark(const char* name) {} // do nothing
    inline void endMark() {}
    inline int level() { return mLevel; }
    inline int replayFlags() { return mDeferStruct.mReplayFlags; }
    inline SkPath* allocPathForFrame() { return mDeferStruct.allocPathForFrame(); }

private:
    DeferStateStruct& mDeferStruct;
    const int mLevel;
};

void RenderNode::defer(DeferStateStruct& deferStruct, const int level) {
    DeferOperationHandler handler(deferStruct, level);
    issueOperations<DeferOperationHandler>(deferStruct.mRenderer, handler);
}

class ReplayOperationHandler {
public:
    ReplayOperationHandler(ReplayStateStruct& replayStruct, int level)
        : mReplayStruct(replayStruct), mLevel(level) {}
    inline void operator()(DisplayListOp* operation, int saveCount, bool clipToBounds) {
#if DEBUG_DISPLAY_LIST_OPS_AS_EVENTS
        mReplayStruct.mRenderer.eventMark(operation->name());
#endif
        operation->replay(mReplayStruct, saveCount, mLevel, clipToBounds);
    }
    inline LinearAllocator& allocator() { return *(mReplayStruct.mAllocator); }
    inline void startMark(const char* name) {
        mReplayStruct.mRenderer.startMark(name);
    }
    inline void endMark() {
        mReplayStruct.mRenderer.endMark();
    }
    inline int level() { return mLevel; }
    inline int replayFlags() { return mReplayStruct.mReplayFlags; }
    inline SkPath* allocPathForFrame() { return mReplayStruct.allocPathForFrame(); }

private:
    ReplayStateStruct& mReplayStruct;
    const int mLevel;
};

void RenderNode::replay(ReplayStateStruct& replayStruct, const int level) {
    ReplayOperationHandler handler(replayStruct, level);
    issueOperations<ReplayOperationHandler>(replayStruct.mRenderer, handler);
}

void RenderNode::buildZSortedChildList(const DisplayList::Chunk& chunk,
        std::vector<ZDrawRenderNodeOpPair>& zTranslatedNodes) {
#if !HWUI_NEW_OPS
    if (chunk.beginChildIndex == chunk.endChildIndex) return;

    for (unsigned int i = chunk.beginChildIndex; i < chunk.endChildIndex; i++) {
        DrawRenderNodeOp* childOp = mDisplayList->getChildren()[i];
        RenderNode* child = childOp->renderNode;
        float childZ = child->properties().getZ();

        if (!MathUtils::isZero(childZ) && chunk.reorderChildren) {
            zTranslatedNodes.push_back(ZDrawRenderNodeOpPair(childZ, childOp));
            childOp->skipInOrderDraw = true;
        } else if (!child->properties().getProjectBackwards()) {
            // regular, in order drawing DisplayList
            childOp->skipInOrderDraw = false;
        }
    }

    // Z sort any 3d children (stable-ness makes z compare fall back to standard drawing order)
    std::stable_sort(zTranslatedNodes.begin(), zTranslatedNodes.end());
#endif
}

template <class T>
void RenderNode::issueDrawShadowOperation(const Matrix4& transformFromParent, T& handler) {
    if (properties().getAlpha() <= 0.0f
            || properties().getOutline().getAlpha() <= 0.0f
            || !properties().getOutline().getPath()
            || properties().getScaleX() == 0
            || properties().getScaleY() == 0) {
        // no shadow to draw
        return;
    }

    mat4 shadowMatrixXY(transformFromParent);
    applyViewPropertyTransforms(shadowMatrixXY);

    // Z matrix needs actual 3d transformation, so mapped z values will be correct
    mat4 shadowMatrixZ(transformFromParent);
    applyViewPropertyTransforms(shadowMatrixZ, true);

    const SkPath* casterOutlinePath = properties().getOutline().getPath();
    const SkPath* revealClipPath = properties().getRevealClip().getPath();
    if (revealClipPath && revealClipPath->isEmpty()) return;

    float casterAlpha = properties().getAlpha() * properties().getOutline().getAlpha();


    // holds temporary SkPath to store the result of intersections
    SkPath* frameAllocatedPath = nullptr;
    const SkPath* outlinePath = casterOutlinePath;

    // intersect the outline with the reveal clip, if present
    if (revealClipPath) {
        frameAllocatedPath = handler.allocPathForFrame();

        Op(*outlinePath, *revealClipPath, kIntersect_SkPathOp, frameAllocatedPath);
        outlinePath = frameAllocatedPath;
    }

    // intersect the outline with the clipBounds, if present
    if (properties().getClippingFlags() & CLIP_TO_CLIP_BOUNDS) {
        if (!frameAllocatedPath) {
            frameAllocatedPath = handler.allocPathForFrame();
        }

        Rect clipBounds;
        properties().getClippingRectForFlags(CLIP_TO_CLIP_BOUNDS, &clipBounds);
        SkPath clipBoundsPath;
        clipBoundsPath.addRect(clipBounds.left, clipBounds.top,
                clipBounds.right, clipBounds.bottom);

        Op(*outlinePath, clipBoundsPath, kIntersect_SkPathOp, frameAllocatedPath);
        outlinePath = frameAllocatedPath;
    }

    DisplayListOp* shadowOp  = new (handler.allocator()) DrawShadowOp(
            shadowMatrixXY, shadowMatrixZ, casterAlpha, outlinePath);
    handler(shadowOp, PROPERTY_SAVECOUNT, properties().getClipToBounds());
}

#define SHADOW_DELTA 0.1f

template <class T>
void RenderNode::issueOperationsOf3dChildren(ChildrenSelectMode mode,
        const Matrix4& initialTransform, const std::vector<ZDrawRenderNodeOpPair>& zTranslatedNodes,
        OpenGLRenderer& renderer, T& handler) {
    const int size = zTranslatedNodes.size();
    if (size == 0
            || (mode == ChildrenSelectMode::NegativeZChildren && zTranslatedNodes[0].key > 0.0f)
            || (mode == ChildrenSelectMode::PositiveZChildren && zTranslatedNodes[size - 1].key < 0.0f)) {
        // no 3d children to draw
        return;
    }

    // Apply the base transform of the parent of the 3d children. This isolates
    // 3d children of the current chunk from transformations made in previous chunks.
    int rootRestoreTo = renderer.save(SaveFlags::Matrix);
    renderer.setGlobalMatrix(initialTransform);

    /**
     * Draw shadows and (potential) casters mostly in order, but allow the shadows of casters
     * with very similar Z heights to draw together.
     *
     * This way, if Views A & B have the same Z height and are both casting shadows, the shadows are
     * underneath both, and neither's shadow is drawn on top of the other.
     */
    const size_t nonNegativeIndex = findNonNegativeIndex(zTranslatedNodes);
    size_t drawIndex, shadowIndex, endIndex;
    if (mode == ChildrenSelectMode::NegativeZChildren) {
        drawIndex = 0;
        endIndex = nonNegativeIndex;
        shadowIndex = endIndex; // draw no shadows
    } else {
        drawIndex = nonNegativeIndex;
        endIndex = size;
        shadowIndex = drawIndex; // potentially draw shadow for each pos Z child
    }

    DISPLAY_LIST_LOGD("%*s%d %s 3d children:", (handler.level() + 1) * 2, "",
            endIndex - drawIndex, mode == kNegativeZChildren ? "negative" : "positive");

    float lastCasterZ = 0.0f;
    while (shadowIndex < endIndex || drawIndex < endIndex) {
        if (shadowIndex < endIndex) {
            DrawRenderNodeOp* casterOp = zTranslatedNodes[shadowIndex].value;
            RenderNode* caster = casterOp->renderNode;
            const float casterZ = zTranslatedNodes[shadowIndex].key;
            // attempt to render the shadow if the caster about to be drawn is its caster,
            // OR if its caster's Z value is similar to the previous potential caster
            if (shadowIndex == drawIndex || casterZ - lastCasterZ < SHADOW_DELTA) {
                caster->issueDrawShadowOperation(casterOp->localMatrix, handler);

                lastCasterZ = casterZ; // must do this even if current caster not casting a shadow
                shadowIndex++;
                continue;
            }
        }

        // only the actual child DL draw needs to be in save/restore,
        // since it modifies the renderer's matrix
        int restoreTo = renderer.save(SaveFlags::Matrix);

        DrawRenderNodeOp* childOp = zTranslatedNodes[drawIndex].value;

        renderer.concatMatrix(childOp->localMatrix);
        childOp->skipInOrderDraw = false; // this is horrible, I'm so sorry everyone
        handler(childOp, renderer.getSaveCount() - 1, properties().getClipToBounds());
        childOp->skipInOrderDraw = true;

        renderer.restoreToCount(restoreTo);
        drawIndex++;
    }
    renderer.restoreToCount(rootRestoreTo);
}

template <class T>
void RenderNode::issueOperationsOfProjectedChildren(OpenGLRenderer& renderer, T& handler) {
    DISPLAY_LIST_LOGD("%*s%d projected children:", (handler.level() + 1) * 2, "", mProjectedNodes.size());
    const SkPath* projectionReceiverOutline = properties().getOutline().getPath();
    int restoreTo = renderer.getSaveCount();

    LinearAllocator& alloc = handler.allocator();
    handler(new (alloc) SaveOp(SaveFlags::MatrixClip),
            PROPERTY_SAVECOUNT, properties().getClipToBounds());

    // Transform renderer to match background we're projecting onto
    // (by offsetting canvas by translationX/Y of background rendernode, since only those are set)
    const DisplayListOp* op =
#if HWUI_NEW_OPS
            nullptr;
    LOG_ALWAYS_FATAL("unsupported");
#else
            (mDisplayList->getOps()[mDisplayList->projectionReceiveIndex]);
#endif
    const DrawRenderNodeOp* backgroundOp = reinterpret_cast<const DrawRenderNodeOp*>(op);
    const RenderProperties& backgroundProps = backgroundOp->renderNode->properties();
    renderer.translate(backgroundProps.getTranslationX(), backgroundProps.getTranslationY());

    // If the projection receiver has an outline, we mask projected content to it
    // (which we know, apriori, are all tessellated paths)
    renderer.setProjectionPathMask(alloc, projectionReceiverOutline);

    // draw projected nodes
    for (size_t i = 0; i < mProjectedNodes.size(); i++) {
        renderNodeOp_t* childOp = mProjectedNodes[i];

        // matrix save, concat, and restore can be done safely without allocating operations
        int restoreTo = renderer.save(SaveFlags::Matrix);
        renderer.concatMatrix(childOp->transformFromCompositingAncestor);
        childOp->skipInOrderDraw = false; // this is horrible, I'm so sorry everyone
        handler(childOp, renderer.getSaveCount() - 1, properties().getClipToBounds());
        childOp->skipInOrderDraw = true;
        renderer.restoreToCount(restoreTo);
    }

    handler(new (alloc) RestoreToCountOp(restoreTo),
            PROPERTY_SAVECOUNT, properties().getClipToBounds());
}

/**
 * This function serves both defer and replay modes, and will organize the displayList's component
 * operations for a single frame:
 *
 * Every 'simple' state operation that affects just the matrix and alpha (or other factors of
 * DeferredDisplayState) may be issued directly to the renderer, but complex operations (with custom
 * defer logic) and operations in displayListOps are issued through the 'handler' which handles the
 * defer vs replay logic, per operation
 */
template <class T>
void RenderNode::issueOperations(OpenGLRenderer& renderer, T& handler) {
    if (mDisplayList->isEmpty()) {
        DISPLAY_LIST_LOGD("%*sEmpty display list (%p, %s)", handler.level() * 2, "",
                this, getName());
        return;
    }

#if HWUI_NEW_OPS
    const bool drawLayer = false;
#else
    const bool drawLayer = (mLayer && (&renderer != mLayer->renderer.get()));
#endif
    // If we are updating the contents of mLayer, we don't want to apply any of
    // the RenderNode's properties to this issueOperations pass. Those will all
    // be applied when the layer is drawn, aka when this is true.
    const bool useViewProperties = (!mLayer || drawLayer);
    if (useViewProperties) {
        const Outline& outline = properties().getOutline();
        if (properties().getAlpha() <= 0
                || (outline.getShouldClip() && outline.isEmpty())
                || properties().getScaleX() == 0
                || properties().getScaleY() == 0) {
            DISPLAY_LIST_LOGD("%*sRejected display list (%p, %s)", handler.level() * 2, "",
                    this, getName());
            return;
        }
    }

    handler.startMark(getName());

#if DEBUG_DISPLAY_LIST
    const Rect& clipRect = renderer.getLocalClipBounds();
    DISPLAY_LIST_LOGD("%*sStart display list (%p, %s), localClipBounds: %.0f, %.0f, %.0f, %.0f",
            handler.level() * 2, "", this, getName(),
            clipRect.left, clipRect.top, clipRect.right, clipRect.bottom);
#endif

    LinearAllocator& alloc = handler.allocator();
    int restoreTo = renderer.getSaveCount();
    handler(new (alloc) SaveOp(SaveFlags::MatrixClip),
            PROPERTY_SAVECOUNT, properties().getClipToBounds());

    DISPLAY_LIST_LOGD("%*sSave %d %d", (handler.level() + 1) * 2, "",
            SaveFlags::MatrixClip, restoreTo);

    if (useViewProperties) {
        setViewProperties<T>(renderer, handler);
    }

#if HWUI_NEW_OPS
    LOG_ALWAYS_FATAL("legacy op traversal not supported");
#else
    bool quickRejected = properties().getClipToBounds()
            && renderer.quickRejectConservative(0, 0, properties().getWidth(), properties().getHeight());
    if (!quickRejected) {
        Matrix4 initialTransform(*(renderer.currentTransform()));
        renderer.setBaseTransform(initialTransform);

        if (drawLayer) {
            handler(new (alloc) DrawLayerOp(mLayer),
                    renderer.getSaveCount() - 1, properties().getClipToBounds());
        } else {
            const int saveCountOffset = renderer.getSaveCount() - 1;
            const int projectionReceiveIndex = mDisplayList->projectionReceiveIndex;
            for (size_t chunkIndex = 0; chunkIndex < mDisplayList->getChunks().size(); chunkIndex++) {
                const DisplayList::Chunk& chunk = mDisplayList->getChunks()[chunkIndex];

                std::vector<ZDrawRenderNodeOpPair> zTranslatedNodes;
                buildZSortedChildList(chunk, zTranslatedNodes);

                issueOperationsOf3dChildren(ChildrenSelectMode::NegativeZChildren,
                        initialTransform, zTranslatedNodes, renderer, handler);

                for (size_t opIndex = chunk.beginOpIndex; opIndex < chunk.endOpIndex; opIndex++) {
                    DisplayListOp *op = mDisplayList->getOps()[opIndex];
#if DEBUG_DISPLAY_LIST
                    op->output(handler.level() + 1);
#endif
                    handler(op, saveCountOffset, properties().getClipToBounds());

                    if (CC_UNLIKELY(!mProjectedNodes.empty() && projectionReceiveIndex >= 0 &&
                        opIndex == static_cast<size_t>(projectionReceiveIndex))) {
                        issueOperationsOfProjectedChildren(renderer, handler);
                    }
                }

                issueOperationsOf3dChildren(ChildrenSelectMode::PositiveZChildren,
                        initialTransform, zTranslatedNodes, renderer, handler);
            }
        }
    }
#endif

    DISPLAY_LIST_LOGD("%*sRestoreToCount %d", (handler.level() + 1) * 2, "", restoreTo);
    handler(new (alloc) RestoreToCountOp(restoreTo),
            PROPERTY_SAVECOUNT, properties().getClipToBounds());

    DISPLAY_LIST_LOGD("%*sDone (%p, %s)", handler.level() * 2, "", this, getName());
    handler.endMark();
}

} /* namespace uirenderer */
} /* namespace android */
