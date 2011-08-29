/*
 * Copyright 2010, The Android Open Source Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "BaseTile.h"

#if USE(ACCELERATED_COMPOSITING)

#include "GLUtils.h"
#include "RasterRenderer.h"
#include "TextureInfo.h"
#include "TilesManager.h"

#include <cutils/atomic.h>

#ifdef DEBUG

#include <cutils/log.h>
#include <wtf/CurrentTime.h>
#include <wtf/text/CString.h>

#undef XLOG
#define XLOG(...) android_printLog(ANDROID_LOG_DEBUG, "BaseTile", __VA_ARGS__)

#else

#undef XLOG
#define XLOG(...)

#endif // DEBUG

namespace WebCore {

BaseTile::BaseTile(bool isLayerTile)
    : m_glWebViewState(0)
    , m_painter(0)
    , m_x(-1)
    , m_y(-1)
    , m_page(0)
    , m_frontTexture(0)
    , m_backTexture(0)
    , m_scale(1)
    , m_dirty(true)
    , m_repaintPending(false)
    , m_lastDirtyPicture(0)
    , m_isTexturePainted(false)
    , m_isLayerTile(isLayerTile)
    , m_isSwapNeeded(false)
    , m_drawCount(0)
{
#ifdef DEBUG_COUNT
    ClassTracker::instance()->increment("BaseTile");
#endif
    m_currentDirtyAreaIndex = 0;

    // For EglImage Mode, the internal buffer should be 2.
    // For Surface Texture mode, we only need one.
    if (TilesManager::instance()->getSharedTextureMode() == EglImageMode)
        m_maxBufferNumber = 2;
    else
        m_maxBufferNumber = 1;

    m_dirtyArea = new SkRegion[m_maxBufferNumber];
    m_fullRepaint = new bool[m_maxBufferNumber];
    for (int i = 0; i < m_maxBufferNumber; i++)
        m_fullRepaint[i] = true;

    m_renderer = BaseRenderer::createRenderer();
}

BaseTile::~BaseTile()
{
    if (m_backTexture)
        m_backTexture->release(this);
    if (m_frontTexture)
        m_frontTexture->release(this);

    delete m_renderer;
    delete[] m_dirtyArea;
    delete[] m_fullRepaint;

#ifdef DEBUG_COUNT
    ClassTracker::instance()->decrement("BaseTile");
#endif
}

// All the following functions must be called from the main GL thread.

void BaseTile::setContents(TilePainter* painter, int x, int y, float scale)
{
    android::AutoMutex lock(m_atomicSync);
    if ((m_painter != painter)
        || (m_x != x)
        || (m_y != y)
        || (m_scale != scale))
        fullInval();

    m_painter = painter;
    m_x = x;
    m_y = y;
    m_scale = scale;
    m_drawCount = TilesManager::instance()->getDrawGLCount();
}

void BaseTile::reserveTexture()
{
    BaseTileTexture* texture = TilesManager::instance()->getAvailableTexture(this);

    android::AutoMutex lock(m_atomicSync);
    if (texture && m_backTexture != texture) {
        m_isSwapNeeded = false; // no longer ready to swap
        m_backTexture = texture;

        // this is to catch when the front texture is stolen from beneath us. We
        // should refine the stealing method to be simpler, and not require last
        // moment checks like this
        if (!m_frontTexture)
            m_dirty = true;
    }
}

bool BaseTile::removeTexture(BaseTileTexture* texture)
{
    XLOG("%x removeTexture back %x front %x... page %x",
         this, m_backTexture, m_frontTexture, m_page);
    // We update atomically, so paintBitmap() can see the correct value
    android::AutoMutex lock(m_atomicSync);
    if (m_frontTexture == texture) {
        m_frontTexture = 0;
        m_dirty = true;
    }
    if (m_backTexture == texture)
        m_backTexture = 0;
    return true;
}

void BaseTile::fullInval()
{
    for (int i = 0; i < m_maxBufferNumber; i++) {
        m_dirtyArea[i].setEmpty();
        m_fullRepaint[i] = true;
    }
    m_dirty = true;
}

void BaseTile::markAsDirty(int unsigned pictureCount,
                           const SkRegion& dirtyArea)
{
    if (dirtyArea.isEmpty())
        return;
    android::AutoMutex lock(m_atomicSync);
    m_lastDirtyPicture = pictureCount;
    for (int i = 0; i < m_maxBufferNumber; i++)
        m_dirtyArea[i].op(dirtyArea, SkRegion::kUnion_Op);
    m_dirty = true;
}

bool BaseTile::isDirty()
{
    android::AutoMutex lock(m_atomicSync);
    return m_dirty;
}

bool BaseTile::isRepaintPending()
{
    android::AutoMutex lock(m_atomicSync);
    return m_repaintPending;
}

void BaseTile::setRepaintPending(bool pending)
{
    android::AutoMutex lock(m_atomicSync);
    m_repaintPending = pending;
}

void BaseTile::draw(float transparency, SkRect& rect, float scale)
{
    if (m_x < 0 || m_y < 0 || m_scale != scale)
        return;

    // No need to mutex protect reads of m_backTexture as it is only written to by
    // the consumer thread.
    if (!m_frontTexture)
        return;

    // Early return if set to un-usable in purpose!
    m_atomicSync.lock();
    bool isTexturePainted = m_isTexturePainted;
    m_atomicSync.unlock();

    if (!isTexturePainted)
        return;

    TextureInfo* textureInfo = m_frontTexture->consumerLock();
    if (!textureInfo) {
        m_frontTexture->consumerRelease();
        return;
    }

    if (m_frontTexture->readyFor(this)) {
        if (isLayerTile())
            TilesManager::instance()->shader()->drawLayerQuad(*m_painter->transform(),
                                                              rect, m_frontTexture->m_ownTextureId,
                                                              transparency, true);
        else
            TilesManager::instance()->shader()->drawQuad(rect, m_frontTexture->m_ownTextureId,
                                                         transparency);
    } else
        m_dirty = true;

    m_frontTexture->consumerRelease();
}

bool BaseTile::isTileReady()
{
    // Return true if the tile's most recently drawn texture is up to date
    android::AutoMutex lock(m_atomicSync);
    BaseTileTexture * texture = m_isSwapNeeded ? m_backTexture : m_frontTexture;

    if (!texture)
        return false;

    if (texture->owner() != this)
        return false;

    if (m_dirty)
        return false;

    texture->consumerLock();
    bool ready = texture->readyFor(this);
    texture->consumerRelease();

    if (ready)
        return true;

    m_dirty = true;
    return false;
}

bool BaseTile::intersectWithRect(int x, int y, int tileWidth, int tileHeight,
                                 float scale, const SkRect& dirtyRect,
                                 SkRect& realTileRect)
{
    // compute the rect to corresponds to pixels
    realTileRect.fLeft = x * tileWidth;
    realTileRect.fTop = y * tileHeight;
    realTileRect.fRight = realTileRect.fLeft + tileWidth;
    realTileRect.fBottom = realTileRect.fTop + tileHeight;

    // scale the dirtyRect for intersect computation.
    SkRect realDirtyRect = SkRect::MakeWH(dirtyRect.width() * scale,
                                          dirtyRect.height() * scale);
    realDirtyRect.offset(dirtyRect.fLeft * scale, dirtyRect.fTop * scale);

    if (!realTileRect.intersect(realDirtyRect))
        return false;
    return true;
}

// This is called from the texture generation thread
void BaseTile::paintBitmap()
{
    // We acquire the values below atomically. This ensures that we are reading
    // values correctly across cores. Further, once we have these values they
    // can be updated by other threads without consequence.
    m_atomicSync.lock();
    bool dirty = m_dirty;
    BaseTileTexture* texture = m_backTexture;
    SkRegion dirtyArea = m_dirtyArea[m_currentDirtyAreaIndex];
    float scale = m_scale;
    const int x = m_x;
    const int y = m_y;
    TilePainter* painter = m_painter;

    if (!dirty || !texture) {
        m_atomicSync.unlock();
        return;
    }

    texture->producerAcquireContext();
    TextureInfo* textureInfo = texture->producerLock();
    m_atomicSync.unlock();

    // at this point we can safely check the ownership (if the texture got
    // transferred to another BaseTile under us)
    if (texture->owner() != this) {
        texture->producerRelease();
        return;
    }

    unsigned int pictureCount = 0;

    // swap out the renderer if necessary
    BaseRenderer::swapRendererIfNeeded(m_renderer);

    // setup the common renderInfo fields;
    TileRenderInfo renderInfo;
    renderInfo.x = x;
    renderInfo.y = y;
    renderInfo.scale = scale;
    renderInfo.tileSize = texture->getSize();
    renderInfo.tilePainter = painter;
    renderInfo.baseTile = this;
    renderInfo.textureInfo = textureInfo;

    const float tileWidth = renderInfo.tileSize.width();
    const float tileHeight = renderInfo.tileSize.height();

    SkRegion::Iterator cliperator(dirtyArea);

    bool fullRepaint = false;

    if (m_fullRepaint[m_currentDirtyAreaIndex]
        || textureInfo->m_width != tileWidth
        || textureInfo->m_height != tileHeight) {
        fullRepaint = true;
    }

    bool surfaceTextureMode = textureInfo->getSharedTextureMode() == SurfaceTextureMode;

    if (surfaceTextureMode)
        fullRepaint = true;

    while (!fullRepaint && !cliperator.done()) {
        SkRect realTileRect;
        SkRect dirtyRect;
        dirtyRect.set(cliperator.rect());
        bool intersect = intersectWithRect(x, y, tileWidth, tileHeight,
                                           scale, dirtyRect, realTileRect);

        // With SurfaceTexture, just repaint the entire tile if we intersect
        // TODO: Implement the partial invalidate in Surface Texture Mode
        if (intersect && surfaceTextureMode) {
            fullRepaint = true;
            break;
        }

        if (intersect && !surfaceTextureMode) {
            // initialize finalRealRect to the rounded values of realTileRect
            SkIRect finalRealRect;
            realTileRect.roundOut(&finalRealRect);

            // stash the int values of the current width and height
            const int iWidth = finalRealRect.width();
            const int iHeight = finalRealRect.height();

            if (iWidth == tileWidth || iHeight == tileHeight) {
                fullRepaint = true;
                break;
            }

            // translate the rect into tile space coordinates
            finalRealRect.fLeft = finalRealRect.fLeft % static_cast<int>(tileWidth);
            finalRealRect.fTop = finalRealRect.fTop % static_cast<int>(tileHeight);
            finalRealRect.fRight = finalRealRect.fLeft + iWidth;
            finalRealRect.fBottom = finalRealRect.fTop + iHeight;

            renderInfo.invalRect = &finalRealRect;
            renderInfo.measurePerf = false;

            pictureCount = m_renderer->renderTiledContent(renderInfo);
        }

        cliperator.next();
    }

    // Do a full repaint if needed
    if (fullRepaint) {
        SkIRect rect;
        rect.set(0, 0, tileWidth, tileHeight);

        renderInfo.invalRect = &rect;
        renderInfo.measurePerf = TilesManager::instance()->getShowVisualIndicator();

        pictureCount = m_renderer->renderTiledContent(renderInfo);
    }

    m_atomicSync.lock();

#if DEPRECATED_SURFACE_TEXTURE_MODE
    texture->setTile(textureInfo, x, y, scale, painter, pictureCount);
#endif
    texture->producerReleaseAndSwap();
    if (texture == m_backTexture) {
        m_isTexturePainted = true;

        // set the fullrepaint flags
        m_fullRepaint[m_currentDirtyAreaIndex] = false;

        // The various checks to see if we are still dirty...

        m_dirty = false;

        if (m_scale != scale)
            m_dirty = true;

        if (fullRepaint)
            m_dirtyArea[m_currentDirtyAreaIndex].setEmpty();
        else
            m_dirtyArea[m_currentDirtyAreaIndex].op(dirtyArea, SkRegion::kDifference_Op);

        if (!m_dirtyArea[m_currentDirtyAreaIndex].isEmpty())
            m_dirty = true;

        // Now we can swap the dirty areas
        // TODO: For surface texture in Async mode, the index will be updated
        // according to the current buffer just dequeued.
        m_currentDirtyAreaIndex = (m_currentDirtyAreaIndex+1) % m_maxBufferNumber;

        if (!m_dirtyArea[m_currentDirtyAreaIndex].isEmpty())
            m_dirty = true;

        if (!m_dirty)
            m_isSwapNeeded = true;
    }

    m_atomicSync.unlock();
}

void BaseTile::discardTextures() {
    android::AutoMutex lock(m_atomicSync);
    if (m_frontTexture) {
        m_frontTexture->release(this);
        m_frontTexture = 0;
    }
    if (m_backTexture) {
        m_backTexture->release(this);
        m_backTexture = 0;
    }
    m_dirty = true;
}

bool BaseTile::swapTexturesIfNeeded() {
    android::AutoMutex lock(m_atomicSync);
    if (m_isSwapNeeded) {
        // discard old texture and swap the new one in its place
        if (m_frontTexture)
            m_frontTexture->release(this);

        XLOG("%p's frontTexture was %p, now becoming %p", this, m_frontTexture, m_backTexture);
        m_frontTexture = m_backTexture;
        m_backTexture = 0;
        m_isSwapNeeded = false;
        XLOG("display texture for %d, %d front is now %p, texture is %p",
             m_x, m_y, m_frontTexture, m_backTexture);
        return true;
    }
    return false;
}

} // namespace WebCore

#endif // USE(ACCELERATED_COMPOSITING)
