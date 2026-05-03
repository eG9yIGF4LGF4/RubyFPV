/*
    Ruby Licence
    Copyright (c) 2020-2025 Petru Soroaga petrusoroaga@yahoo.com
    All rights reserved.

    Redistribution and/or use in source and/or binary forms, with or without
    modification, are permitted provided that the following conditions are met:
        * Redistributions and/or use of the source code (partially or complete) must retain
        the above copyright notice, this list of conditions and the following disclaimer
        in the documentation and/or other materials provided with the distribution.
        * Redistributions in binary form (partially or complete) must reproduce
        the above copyright notice, this list of conditions and the following disclaimer
        in the documentation and/or other materials provided with the distribution.
        * Copyright info and developer info must be preserved as is in the user
        interface, additions could be made to that info.
        * Neither the name of the organization nor the
        names of its contributors may be used to endorse or promote products
        derived from this software without specific prior written permission.
        * Military use is not permitted.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE AUTHOR (PETRU SOROAGA) BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "../base/base.h"
#include "../base/config.h"
#include "render_engine_cairo_gtk.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Module-level font state (mirrors the DRM backend)
// ---------------------------------------------------------------------------
static int  s_iLastCairoFontFamilyId  = -1;
static bool s_bLastCairoFontStyleBold = false;


// ===========================================================================
// Construction / destruction
// ===========================================================================

RenderEngineCairoGtk::RenderEngineCairoGtk()
: RenderEngine()
, m_pBackSurface(NULL)
, m_pCairoCtx(NULL)
, m_pCairoTempCtx(NULL)
, m_pGtkWidget(NULL)
, m_uDrawSignalId(0)
, m_bMustTestFontAccess(true)
, m_bHasNewFont(false)
, m_CurrentImageId(0)
, m_iCountImages(0)
, m_CurrentIconId(0)
, m_iCountIcons(0)
{
   log_line("[RenderEngineCairoGtk] Init started.");

   // gtk_init() must be called before any GDK/GTK function.
   // If the caller already called it this is a safe no-op (GTK tracks init state).
   if ( !gtk_init_check(NULL, NULL) )
      log_softerror_and_alarm("[RenderEngineCairoGtk] gtk_init_check failed – no display?");

   // Query the primary monitor geometry via GDK so we can match the DRM
   // backend's convention of filling the whole display.
   m_iRenderWidth  = 1280; // safe fallback
   m_iRenderHeight = 720;

   GdkDisplay* pDisplay = gdk_display_get_default();
   if ( NULL != pDisplay )
   {
      GdkScreen* pScreen = gdk_display_get_default_screen(pDisplay);
      if ( NULL != pScreen )
      {
         m_iRenderWidth  = gdk_screen_get_width(pScreen);
         m_iRenderHeight = gdk_screen_get_height(pScreen);
      }
      else
         log_softerror_and_alarm("[RenderEngineCairoGtk] Could not get default GdkScreen, using fallback size.");
   }
   else
      log_softerror_and_alarm("[RenderEngineCairoGtk] Could not get default GdkDisplay, using fallback size.");

   m_fPixelWidth  = 1.0f / (float)m_iRenderWidth;
   m_fPixelHeight = 1.0f / (float)m_iRenderHeight;

   log_line("[RenderEngineCairoGtk] Screen size: %d x %d, pixel: %.4f x %.4f",
      m_iRenderWidth, m_iRenderHeight, m_fPixelWidth, m_fPixelHeight);

   // Create the single off-screen ARGB32 back-buffer.
   m_pBackSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                               m_iRenderWidth,
                                               m_iRenderHeight);
   if ( NULL == m_pBackSurface || cairo_surface_status(m_pBackSurface) != CAIRO_STATUS_SUCCESS )
      log_softerror_and_alarm("[RenderEngineCairoGtk] Failed to create back-buffer surface.");
   else
      log_line("[RenderEngineCairoGtk] Back-buffer surface created (%d x %d).",
               m_iRenderWidth, m_iRenderHeight);

   m_fStrokeSizePx = 1.0;

   memset(m_pImages,   0, sizeof(m_pImages));
   memset(m_ImageIds,  0, sizeof(m_ImageIds));
   memset(m_pIcons,    0, sizeof(m_pIcons));
   memset(m_pIconsMip, 0, sizeof(m_pIconsMip));
   memset(m_IconIds,   0, sizeof(m_IconIds));

   log_line("[RenderEngineCairoGtk] Init done.");
}

RenderEngineCairoGtk::~RenderEngineCairoGtk()
{
   // Disconnect the GTK draw signal before we destroy our surfaces.
   if ( m_pGtkWidget != NULL && m_uDrawSignalId != 0 )
   {
      g_signal_handler_disconnect(m_pGtkWidget, m_uDrawSignalId);
      m_uDrawSignalId = 0;
   }

   if ( NULL != m_pCairoCtx )
   {
      cairo_destroy(m_pCairoCtx);
      m_pCairoCtx = NULL;
   }
   if ( NULL != m_pCairoTempCtx )
   {
      cairo_destroy(m_pCairoTempCtx);
      m_pCairoTempCtx = NULL;
   }
   if ( NULL != m_pBackSurface )
   {
      cairo_surface_destroy(m_pBackSurface);
      m_pBackSurface = NULL;
   }
}


// ===========================================================================
// GTK plumbing
// ===========================================================================

void RenderEngineCairoGtk::connectDrawSignal(GtkWidget* pWidget)
{
   if ( NULL == pWidget )
   {
      log_softerror_and_alarm("[RenderEngineCairoGtk] connectDrawSignal: NULL widget.");
      return;
   }
   m_pGtkWidget   = pWidget;
   m_uDrawSignalId = g_signal_connect(pWidget, "draw",
                                       G_CALLBACK(_onDraw), this);
   log_line("[RenderEngineCairoGtk] Connected draw signal (id=%lu).", m_uDrawSignalId);
}

void RenderEngineCairoGtk::requestRedraw()
{
   if ( NULL != m_pGtkWidget )
      gtk_widget_queue_draw(m_pGtkWidget);
}

// static
gboolean RenderEngineCairoGtk::_onDraw(GtkWidget* /*pWidget*/, cairo_t* pCr, gpointer pUserData)
{
   RenderEngineCairoGtk* pSelf = reinterpret_cast<RenderEngineCairoGtk*>(pUserData);
   if ( NULL == pSelf || NULL == pSelf->m_pBackSurface )
      return FALSE;

   // Paint the off-screen back-buffer into the widget's cairo context.
   cairo_set_source_surface(pCr, pSelf->m_pBackSurface, 0, 0);
   cairo_paint(pCr);
   return FALSE;
}


// ===========================================================================
// Internal helpers
// ===========================================================================

cairo_t* RenderEngineCairoGtk::_createTempDrawContext()
{
   if ( NULL != m_pCairoTempCtx )
      return m_pCairoTempCtx;

   s_iLastCairoFontFamilyId  = -1;
   s_bLastCairoFontStyleBold = false;

   if ( NULL != m_pBackSurface )
      m_pCairoTempCtx = cairo_create(m_pBackSurface);

   return m_pCairoTempCtx;
}

cairo_t* RenderEngineCairoGtk::_getActiveCairoContext()
{
   if ( NULL != m_pCairoCtx )
      return m_pCairoCtx;
   return m_pCairoTempCtx;
}

void* RenderEngineCairoGtk::getDrawContext()
{
   return m_pCairoCtx;
}


// ===========================================================================
// Frame lifecycle
// ===========================================================================

void RenderEngineCairoGtk::startFrame()
{
   if ( m_bStartedFrame )
   {
      log_softerror_and_alarm("[RenderEngineCairoGtk] Tried to double-start a render frame.");
      return;
   }

   RenderEngine::startFrame();

   // Discard any lingering temporary context.
   if ( NULL != m_pCairoTempCtx )
   {
      cairo_destroy(m_pCairoTempCtx);
      m_pCairoTempCtx = NULL;
   }

   s_iLastCairoFontFamilyId  = -1;
   s_bLastCairoFontStyleBold = false;

   // Clear the back-buffer to the configured clear colour.
   if ( NULL != m_pBackSurface )
   {
      u8* pData   = cairo_image_surface_get_data(m_pBackSurface);
      int iStride = cairo_image_surface_get_stride(m_pBackSurface);
      cairo_surface_flush(m_pBackSurface);
      memset(pData, m_uClearBufferByte, (size_t)iStride * m_iRenderHeight);
      cairo_surface_mark_dirty(m_pBackSurface);
   }

   // Create the frame drawing context.
   if ( NULL != m_pCairoCtx )
   {
      cairo_destroy(m_pCairoCtx);
      m_pCairoCtx = NULL;
   }
   if ( NULL != m_pBackSurface )
      m_pCairoCtx = cairo_create(m_pBackSurface);

   if ( NULL == m_pCairoCtx )
      log_softerror_and_alarm("[RenderEngineCairoGtk] Failed to create frame cairo context.");
}

void RenderEngineCairoGtk::endFrame()
{
   if ( !m_bStartedFrame )
   {
      log_softerror_and_alarm("[RenderEngineCairoGtk] Tried to double-end a render frame.");
      return;
   }

   if ( NULL != m_pCairoCtx )
   {
      cairo_destroy(m_pCairoCtx);
      m_pCairoCtx = NULL;
   }
   if ( NULL != m_pCairoTempCtx )
   {
      cairo_destroy(m_pCairoTempCtx);
      m_pCairoTempCtx = NULL;
   }

   RenderEngine::endFrame();

   // Ask GTK to repaint the widget from our now-complete back-buffer.
   requestRedraw();
}


// ===========================================================================
// Stroke / style
// ===========================================================================

void RenderEngineCairoGtk::setStroke(const double* color, float fStrokeSize)
{
   RenderEngine::setStroke(color, fStrokeSize);
   if ( NULL != m_pCairoCtx )
      cairo_set_line_width(m_pCairoCtx, 1.1);
}

void RenderEngineCairoGtk::setStrokeSize(float fStrokeSize)
{
   RenderEngine::setStrokeSize(fStrokeSize);
   if ( NULL != m_pCairoCtx )
      cairo_set_line_width(m_pCairoCtx, 1.1);
}


// ===========================================================================
// Font image objects (used by the base-class font system)
// ===========================================================================

void* RenderEngineCairoGtk::_loadRawFontImageObject(const char* szFileName)
{
   if ( (NULL == szFileName) || (0 == szFileName[0]) )
   {
      log_softerror_and_alarm("[RenderEngineCairoGtk] _loadRawFontImageObject: invalid filename.");
      return NULL;
   }
   cairo_surface_t* pImage = cairo_image_surface_create_from_png(szFileName);
   if ( NULL == pImage || cairo_image_surface_get_stride(pImage) <= 0 )
      log_softerror_and_alarm("[RenderEngineCairoGtk] Failed to load font image (%s)", szFileName);
   else
      log_line("[RenderEngineCairoGtk] Loaded font image (%s), stride:%d, w:%d, h:%d",
               szFileName,
               cairo_image_surface_get_stride(pImage),
               cairo_image_surface_get_width(pImage),
               cairo_image_surface_get_height(pImage));
   return (void*)pImage;
}

void RenderEngineCairoGtk::_freeRawFontImageObject(void* pImageObject)
{
   if ( NULL == pImageObject )
      return;
   cairo_surface_destroy((cairo_surface_t*)pImageObject);
   log_line("[RenderEngineCairoGtk] Freed font image object %p", pImageObject);
}

void RenderEngineCairoGtk::setFontOutlineColor(u32 /*idFont*/, u8 /*r*/, u8 /*g*/, u8 /*b*/, u8 /*a*/)
{
   // Not implemented – mirrors DRM backend behaviour.
}


// ===========================================================================
// Image management
// ===========================================================================

u32 RenderEngineCairoGtk::loadImage(const char* szFile)
{
   if ( m_iCountImages >= MAX_RAW_IMAGES )
      return 0;
   if ( access(szFile, R_OK) == -1 )
      return 0;

   cairo_surface_t* pSurf = NULL;
   if ( NULL != strstr(szFile, ".png") )
      pSurf = cairo_image_surface_create_from_png(szFile);
   else
      return 0;

   if ( NULL != pSurf )
      log_line("[RenderEngineCairoGtk] Loaded image %s, id: %u", szFile, m_CurrentImageId + 1);
   else
      log_softerror_and_alarm("[RenderEngineCairoGtk] Failed to load image %s", szFile);

   m_CurrentImageId++;
   m_pImages[m_iCountImages]  = pSurf;
   m_ImageIds[m_iCountImages] = m_CurrentImageId;
   m_iCountImages++;
   return m_CurrentImageId;
}

void RenderEngineCairoGtk::freeImage(u32 idImage)
{
   int idx = -1;
   for ( int i = 0; i < m_iCountImages; i++ )
      if ( m_ImageIds[i] == idImage ) { idx = i; break; }
   if ( idx == -1 )
      return;

   cairo_surface_destroy(m_pImages[idx]);

   for ( int i = idx; i < m_iCountImages - 1; i++ )
   {
      m_pImages[i]  = m_pImages[i + 1];
      m_ImageIds[i] = m_ImageIds[i + 1];
   }
   m_iCountImages--;
}

int RenderEngineCairoGtk::getImageWidth(u32 uImageId)
{
   if ( uImageId < 1 ) return 0;
   for ( int i = 0; i < m_iCountImages; i++ )
      if ( m_ImageIds[i] == uImageId && m_pImages[i] != NULL )
         return cairo_image_surface_get_width(m_pImages[i]);
   return 0;
}

int RenderEngineCairoGtk::getImageHeight(u32 uImageId)
{
   if ( uImageId < 1 ) return 0;
   for ( int i = 0; i < m_iCountImages; i++ )
      if ( m_ImageIds[i] == uImageId && m_pImages[i] != NULL )
         return cairo_image_surface_get_height(m_pImages[i]);
   return 0;
}

void RenderEngineCairoGtk::changeImageHue(u32 uImageId, u8 r, u8 g, u8 b)
{
   if ( uImageId < 1 ) return;
   int idx = -1;
   for ( int i = 0; i < m_iCountImages; i++ )
      if ( m_ImageIds[i] == uImageId ) { idx = i; break; }
   if ( idx == -1 || NULL == m_pImages[idx] )
      return;

   int  iW      = cairo_image_surface_get_width(m_pImages[idx]);
   int  iH      = cairo_image_surface_get_height(m_pImages[idx]);
   int  iStride = cairo_image_surface_get_stride(m_pImages[idx]);
   u8*  pData   = cairo_image_surface_get_data(m_pImages[idx]);

   cairo_surface_flush(m_pImages[idx]);
   for ( int y = 0; y < iH; y++ )
   {
      u8* pLine = pData + y * iStride;
      for ( int x = 0; x < iW; x++ )
      {
         // BGRA order in ARGB32 on little-endian
         if ( pLine[0] > 220 && pLine[1] > 220 && pLine[2] > 220 )
         {
            pLine[0] = ((unsigned int)pLine[0] * (unsigned int)b) >> 8;
            pLine[1] = ((unsigned int)pLine[1] * (unsigned int)g) >> 8;
            pLine[2] = ((unsigned int)pLine[2] * (unsigned int)r) >> 8;
         }
         pLine += 4;
      }
   }
   cairo_surface_mark_dirty(m_pImages[idx]);
}


// ===========================================================================
// Icon management
// ===========================================================================

u32 RenderEngineCairoGtk::loadIcon(const char* szFile)
{
   if ( m_iCountIcons >= MAX_RAW_ICONS )
      return 0;
   if ( access(szFile, R_OK) == -1 )
      return 0;

   cairo_surface_t* pSurf = NULL;
   if ( NULL != strstr(szFile, ".png") )
      pSurf = cairo_image_surface_create_from_png(szFile);
   else
      return 0;

   if ( NULL != pSurf )
      log_line("[RenderEngineCairoGtk] Loaded icon %s, id: %u", szFile, m_CurrentIconId + 1);
   else
      log_softerror_and_alarm("[RenderEngineCairoGtk] Failed to load icon %s", szFile);

   m_pIcons[m_iCountIcons]       = pSurf;
   m_pIconsMip[m_iCountIcons][0] = NULL;
   m_pIconsMip[m_iCountIcons][1] = NULL;

   m_CurrentIconId++;
   m_IconIds[m_iCountIcons] = m_CurrentIconId;
   m_iCountIcons++;
   return m_CurrentIconId;
}

void RenderEngineCairoGtk::freeIcon(u32 idIcon)
{
   int idx = -1;
   for ( int i = 0; i < m_iCountIcons; i++ )
      if ( m_IconIds[i] == idIcon ) { idx = i; break; }
   if ( idx == -1 )
      return;

   cairo_surface_destroy(m_pIcons[idx]);
   if ( NULL != m_pIconsMip[idx][0] ) cairo_surface_destroy(m_pIconsMip[idx][0]);
   if ( NULL != m_pIconsMip[idx][1] ) cairo_surface_destroy(m_pIconsMip[idx][1]);

   for ( int i = idx; i < m_iCountIcons - 1; i++ )
   {
      m_pIcons[i]       = m_pIcons[i + 1];
      m_pIconsMip[i][0] = m_pIconsMip[i + 1][0];
      m_pIconsMip[i][1] = m_pIconsMip[i + 1][1];
      m_IconIds[i]      = m_IconIds[i + 1];
   }
   m_iCountIcons--;
}


// ===========================================================================
// Image / sprite drawing
// ===========================================================================

void RenderEngineCairoGtk::drawImage(float xPos, float yPos, float fWidth, float fHeight, u32 uImageId)
{
   if ( uImageId < 1 || NULL == m_pCairoCtx ) return;

   int idx = -1;
   for ( int i = 0; i < m_iCountImages; i++ )
      if ( m_ImageIds[i] == uImageId ) { idx = i; break; }
   if ( idx == -1 || NULL == m_pImages[idx] )
      return;

   int iImgW = cairo_image_surface_get_width(m_pImages[idx]);
   int iImgH = cairo_image_surface_get_height(m_pImages[idx]);
   int iDestW = (int)(fWidth  * m_iRenderWidth);
   int iDestH = (int)(fHeight * m_iRenderHeight);

   if ( iDestW <= 0 || iDestH <= 0 ) return;

   double scaleX = (double)iImgW / (double)iDestW;
   double scaleY = (double)iImgH / (double)iDestH;

   cairo_save(m_pCairoCtx);
   cairo_translate(m_pCairoCtx, xPos * m_iRenderWidth, yPos * m_iRenderHeight);
   cairo_scale(m_pCairoCtx, 1.0 / scaleX, 1.0 / scaleY);
   cairo_set_source_surface(m_pCairoCtx, m_pImages[idx], 0, 0);
   cairo_pattern_set_filter(cairo_get_source(m_pCairoCtx), CAIRO_FILTER_NEAREST);
   cairo_paint(m_pCairoCtx);
   cairo_restore(m_pCairoCtx);
}

void RenderEngineCairoGtk::drawImageAlpha(float xPos, float yPos, float fWidth, float fHeight, u32 uImageId, u8 uAlpha)
{
   // Same simplification as the DRM backend: delegate to drawImage.
   (void)uAlpha;
   drawImage(xPos, yPos, fWidth, fHeight, uImageId);
}

void RenderEngineCairoGtk::bltImage(float xPosDest, float yPosDest, float fWidthDest, float fHeightDest,
                                     int iSrcX, int iSrcY, int iSrcWidth, int iSrcHeight, u32 uImageId)
{
   if ( uImageId < 1 ) return;

   int idx = -1;
   for ( int i = 0; i < m_iCountImages; i++ )
      if ( m_ImageIds[i] == uImageId ) { idx = i; break; }
   if ( idx == -1 || NULL == m_pImages[idx] )
      return;

   int xDest = (int)(xPosDest  * m_iRenderWidth);
   int yDest = (int)(yPosDest  * m_iRenderHeight);
   int wDest = (int)(fWidthDest  * m_iRenderWidth);
   int hDest = (int)(fHeightDest * m_iRenderHeight);

   if ( xDest < 0 || yDest < 0 || xDest + wDest > m_iRenderWidth || yDest + hDest > m_iRenderHeight )
      return;

   // Direct pixel blt with colour-fill tinting onto the back-buffer.
   cairo_surface_flush(m_pBackSurface);
   u8* pBackData   = cairo_image_surface_get_data(m_pBackSurface);
   int iBackStride = cairo_image_surface_get_stride(m_pBackSurface);
   u8* pSrcData    = cairo_image_surface_get_data(m_pImages[idx]);
   int iSrcStride  = cairo_image_surface_get_stride(m_pImages[idx]);

   float dxSrc = (float)iSrcWidth  / (float)wDest;
   float dySrc = (float)iSrcHeight / (float)hDest;
   float fSrcY = (float)iSrcY;

   for ( int sy = 0; sy < hDest; sy++ )
   {
      float fSrcX  = (float)iSrcX;
      u8*   pDest  = pBackData + (yDest + sy) * iBackStride + xDest * 4;
      for ( int sx = 0; sx < wDest; sx++ )
      {
         u8* pSrc = pSrcData + (int)fSrcY * iSrcStride + (int)fSrcX * 4;
         u8 b = pSrc[0], g = pSrc[1], r = pSrc[2], a = pSrc[3];
         if ( a > 4 )
         {
            pDest[0] = (u8)((((b * m_ColorFill[2]) >> 8) * (255 - m_ColorFill[3]) + pDest[0] * m_ColorFill[3]) >> 8);
            pDest[1] = (u8)((((g * m_ColorFill[1]) >> 8) * (255 - m_ColorFill[3]) + pDest[1] * m_ColorFill[3]) >> 8);
            pDest[2] = (u8)((((r * m_ColorFill[0]) >> 8) * (255 - m_ColorFill[3]) + pDest[2] * m_ColorFill[3]) >> 8);
            pDest[3] = (u8)((((a * m_ColorFill[3]) >> 8) * (255 - m_ColorFill[3]) + pDest[3] * m_ColorFill[3]) >> 8);
         }
         pDest  += 4;
         fSrcX  += dxSrc;
      }
      fSrcY += dySrc;
   }
   cairo_surface_mark_dirty(m_pBackSurface);
}

void RenderEngineCairoGtk::bltSprite(float xPosDest, float yPosDest,
                                      int iSrcX, int iSrcY, int iSrcWidth, int iSrcHeight,
                                      u32 uImageId)
{
   if ( uImageId < 1 ) return;

   int idx = -1;
   for ( int i = 0; i < m_iCountImages; i++ )
      if ( m_ImageIds[i] == uImageId ) { idx = i; break; }
   if ( idx == -1 || NULL == m_pImages[idx] )
      return;

   int xDest = (int)(xPosDest * m_iRenderWidth);
   int yDest = (int)(yPosDest * m_iRenderHeight);

   if ( xDest < 0 || yDest < 0 || xDest + iSrcWidth >= m_iRenderWidth || yDest + iSrcHeight >= m_iRenderHeight )
      return;

   cairo_surface_flush(m_pBackSurface);
   u8* pBackData   = cairo_image_surface_get_data(m_pBackSurface);
   int iBackStride = cairo_image_surface_get_stride(m_pBackSurface);
   u8* pSrcData    = cairo_image_surface_get_data(m_pImages[idx]);
   int iSrcStride  = cairo_image_surface_get_stride(m_pImages[idx]);

   for ( int sy = 0; sy < iSrcHeight; sy++ )
   {
      u8* pDest = pBackData + (yDest + sy) * iBackStride + xDest * 4;
      u8* pSrc  = pSrcData  + (iSrcY + sy) * iSrcStride  + iSrcX * 4;
      for ( int sx = 0; sx < iSrcWidth; sx++ )
      {
         u8 b = *pSrc++, g = *pSrc++, r = *pSrc++, a = *pSrc++;
         if ( a > 4 )
         {
            *pDest++ = (u8)((((b * m_ColorFill[2]) >> 8) * a + (*pDest) * (255 - a)) >> 8);
            *pDest++ = (u8)((((g * m_ColorFill[1]) >> 8) * a + (*pDest) * (255 - a)) >> 8);
            *pDest++ = (u8)((((r * m_ColorFill[0]) >> 8) * a + (*pDest) * (255 - a)) >> 8);
            *pDest++ = (u8)((((a * m_ColorFill[3]) >> 8) * a + (*pDest) * (255 - a)) >> 8);
         }
         else
            pDest += 4;
      }
   }
   cairo_surface_mark_dirty(m_pBackSurface);
}


// ===========================================================================
// Icon drawing
// ===========================================================================

void RenderEngineCairoGtk::drawIcon(float xPos, float yPos, float fWidth, float fHeight, u32 uIconId)
{
   if ( uIconId < 1 ) return;

   int idx = -1;
   for ( int i = 0; i < m_iCountIcons; i++ )
      if ( m_IconIds[i] == uIconId ) { idx = i; break; }
   if ( idx == -1 || NULL == m_pIcons[idx] )
      return;

   int x = (int)(xPos   * m_iRenderWidth);
   int y = (int)(yPos   * m_iRenderHeight);
   int w = (int)(fWidth  * m_iRenderWidth);
   int h = (int)(fHeight * m_iRenderHeight);

   if ( x < 0 || y < 0 || x + w >= m_iRenderWidth || y + h >= m_iRenderHeight )
      return;

   cairo_surface_flush(m_pBackSurface);
   u8* pBackData   = cairo_image_surface_get_data(m_pBackSurface);
   int iBackStride = cairo_image_surface_get_stride(m_pBackSurface);
   u8* pIconData   = cairo_image_surface_get_data(m_pIcons[idx]);
   int iIconStride = cairo_image_surface_get_stride(m_pIcons[idx]);

   float fIconW = (float)cairo_image_surface_get_width(m_pIcons[idx]);
   float fIconH = (float)cairo_image_surface_get_height(m_pIcons[idx]);
   float dxIcon = fIconW / (float)w;
   float dyIcon = fIconH / (float)h;
   float yIcon  = 0.0f;

   for ( int sy = 0; sy < h; sy++ )
   {
      int   iyIcon = (int)yIcon;
      if ( iyIcon >= (int)fIconH ) break;

      u8*   pDest  = pBackData + (y + sy) * iBackStride + x * 4;
      float xIcon  = 0.0f;
      for ( int sx = 0; sx < w; sx++ )
      {
         u8* pSrc = pIconData + iyIcon * iIconStride + (int)xIcon * 4;
         u8 b = pSrc[0], g = pSrc[1], r = pSrc[2], a = pSrc[3];
         if ( a > 4 )
         {
            pDest[0] = (u8)(((unsigned int)b * (unsigned int)m_ColorFill[2]) >> 8);
            pDest[1] = (u8)(((unsigned int)g * (unsigned int)m_ColorFill[1]) >> 8);
            pDest[2] = (u8)(((unsigned int)r * (unsigned int)m_ColorFill[0]) >> 8);
            pDest[3] = (u8)(((unsigned int)a * (unsigned int)m_ColorFill[3]) >> 8);
         }
         pDest  += 4;
         xIcon  += dxIcon;
      }
      yIcon += dyIcon;
   }
   cairo_surface_mark_dirty(m_pBackSurface);
}

void RenderEngineCairoGtk::bltIcon(float xPosDest, float yPosDest,
                                    int iSrcX, int iSrcY, int iSrcWidth, int iSrcHeight,
                                    u32 uIconId)
{
   if ( uIconId < 1 ) return;

   int idx = -1;
   for ( int i = 0; i < m_iCountIcons; i++ )
      if ( m_IconIds[i] == uIconId ) { idx = i; break; }
   if ( idx == -1 || NULL == m_pIcons[idx] )
      return;

   int xDest = (int)(xPosDest * m_iRenderWidth);
   int yDest = (int)(yPosDest * m_iRenderHeight);

   if ( xDest < 0 || yDest < 0 || xDest + iSrcWidth >= m_iRenderWidth || yDest + iSrcHeight >= m_iRenderHeight )
      return;

   cairo_surface_flush(m_pBackSurface);
   u8* pBackData   = cairo_image_surface_get_data(m_pBackSurface);
   int iBackStride = cairo_image_surface_get_stride(m_pBackSurface);
   u8* pIconData   = cairo_image_surface_get_data(m_pIcons[idx]);
   int iIconStride = cairo_image_surface_get_stride(m_pIcons[idx]);

   for ( int y = 0; y < iSrcHeight; y++ )
   {
      u8* pDest = pBackData + (yDest + y) * iBackStride + xDest * 4;
      u8* pSrc  = pIconData + (iSrcY + y) * iIconStride + iSrcX * 4;
      for ( int x = 0; x < iSrcWidth; x++ )
      {
         u8 uAlpha = pSrc[3];
         pDest[0] = (u8)((pSrc[0] * uAlpha + pDest[0] * (255 - uAlpha)) / 256);
         pDest[1] = (u8)((pSrc[1] * uAlpha + pDest[1] * (255 - uAlpha)) / 256);
         pDest[2] = (u8)((pSrc[2] * uAlpha + pDest[2] * (255 - uAlpha)) / 256);
         pDest[3] = pDest[3]; // alpha channel of destination unchanged
         pDest += 4;
         pSrc  += 4;
      }
   }
   cairo_surface_mark_dirty(m_pBackSurface);
}


// ===========================================================================
// Misc
// ===========================================================================

void RenderEngineCairoGtk::rotate180()
{
   // No-op: GTK windows are not rotated at the kernel/display level.
   // If display rotation is required it should be handled via a
   // GtkFixed/GtkLayout transformation or a CSS transform on the widget.
}


// ===========================================================================
// Low-level pixel helpers (operate on the GTK back-buffer)
// ===========================================================================

inline void RenderEngineCairoGtk::_blend_pixel(unsigned char* pixel,
                                                unsigned char r, unsigned char g,
                                                unsigned char b, unsigned char a)
{
   // BGRA order
   if ( pixel[3] == 0 )
   {
      pixel[0] = b; pixel[1] = g; pixel[2] = r; pixel[3] = a;
   }
   else
   {
      pixel[0] = (unsigned char)((a * b + (255 - a) * pixel[0]) >> 8);
      pixel[1] = (unsigned char)((a * g + (255 - a) * pixel[1]) >> 8);
      pixel[2] = (unsigned char)((a * r + (255 - a) * pixel[2]) >> 8);
      if ( pixel[3] != 255 )
         pixel[3] = (unsigned char)(pixel[3] + (((255 - pixel[3]) * a) >> 8));
   }
}

void RenderEngineCairoGtk::_draw_hline(int x, int y, int w,
                                        unsigned char r, unsigned char g,
                                        unsigned char b, unsigned char a)
{
   if ( NULL == m_pBackSurface ) return;
   u8* pData   = cairo_image_surface_get_data(m_pBackSurface);
   int iStride = cairo_image_surface_get_stride(m_pBackSurface);
   u8* pLine   = pData + y * iStride + 4 * x;
   for ( int i = 0; i < w; i++ )
   {
      *pLine++ = b;
      *pLine++ = g;
      *pLine++ = r;
      *pLine++ = a;
   }
}

void RenderEngineCairoGtk::_draw_vline(int x, int y, int h,
                                        unsigned char r, unsigned char g,
                                        unsigned char b, unsigned char a)
{
   if ( NULL == m_pBackSurface ) return;
   u8* pData   = cairo_image_surface_get_data(m_pBackSurface);
   int iStride = cairo_image_surface_get_stride(m_pBackSurface);
   u8* pLine   = pData + y * iStride + 4 * x;
   for ( int i = 0; i < h; i++ )
   {
      pLine[0] = b;
      pLine[1] = g;
      pLine[2] = r;
      pLine[3] = a;
      pLine   += iStride;
   }
}


// ===========================================================================
// Drawing primitives
// ===========================================================================

void RenderEngineCairoGtk::drawLine(float x1, float y1, float x2, float y2)
{
   // Fast paths for axis-aligned lines (same logic as DRM backend).
   if ( fabs(y1 - y2) < 0.0001f )
   {
      if ( x1 < 0 ) x1 = 0.0f;
      if ( x2 < 0 ) x2 = 0.0f;
      if ( x1 > 1.0f - m_fPixelWidth ) x1 = 1.0f - m_fPixelWidth;
      if ( x2 > 1.0f - m_fPixelWidth ) x2 = 1.0f - m_fPixelWidth;
      if ( fabs(x2 - x1) < 0.0001f ) return;

      cairo_surface_flush(m_pBackSurface);
      if ( x1 < x2 )
         _draw_hline((int)(x1*m_iRenderWidth), (int)(y1*m_iRenderHeight),
                     (int)((x2-x1)*m_iRenderWidth),
                     m_ColorStroke[0], m_ColorStroke[1], m_ColorStroke[2], m_ColorStroke[3]);
      else
         _draw_hline((int)(x2*m_iRenderWidth), (int)(y1*m_iRenderHeight),
                     (int)((x1-x2)*m_iRenderWidth),
                     m_ColorStroke[0], m_ColorStroke[1], m_ColorStroke[2], m_ColorStroke[3]);
      cairo_surface_mark_dirty(m_pBackSurface);
      return;
   }
   if ( fabs(x1 - x2) < 0.0001f )
   {
      if ( y1 < 0 ) y1 = 0.0f;
      if ( y2 < 0 ) y2 = 0.0f;
      if ( y1 > 1.0f - m_fPixelHeight ) y1 = 1.0f - m_fPixelHeight;
      if ( y2 > 1.0f - m_fPixelHeight ) y2 = 1.0f - m_fPixelHeight;
      if ( fabs(y2 - y1) < 0.0001f ) return;

      float yPos = y1, h = y2 - y1;
      if ( y1 > y2 ) { yPos = y2; h = y1 - y2; }

      cairo_surface_flush(m_pBackSurface);
      if ( m_fStrokeSizePx < 1.5f )
      {
         _draw_vline((int)(x1*m_iRenderWidth), (int)(yPos*m_iRenderHeight),
                     (int)(h*m_iRenderHeight),
                     m_ColorStroke[0], m_ColorStroke[1], m_ColorStroke[2], m_ColorStroke[3]);
      }
      else
      {
         _draw_vline((int)(x1*m_iRenderWidth)-1, (int)(yPos*m_iRenderHeight),
                     (int)(h*m_iRenderHeight),
                     m_ColorStroke[0], m_ColorStroke[1], m_ColorStroke[2], m_ColorStroke[3]);
         _draw_vline((int)(x1*m_iRenderWidth)+1, (int)(yPos*m_iRenderHeight),
                     (int)(h*m_iRenderHeight),
                     m_ColorStroke[0], m_ColorStroke[1], m_ColorStroke[2], m_ColorStroke[3]);
      }
      cairo_surface_mark_dirty(m_pBackSurface);
      return;
   }

   // Diagonal line – use Cairo.
   if ( NULL == m_pCairoCtx ) return;
   cairo_set_source_rgba(m_pCairoCtx, 1, 1, 1, 1);
   cairo_move_to(m_pCairoCtx, x1 * m_iRenderWidth, y1 * m_iRenderHeight);
   cairo_line_to(m_pCairoCtx, x2 * m_iRenderWidth, y2 * m_iRenderHeight);
   cairo_stroke(m_pCairoCtx);
}

void RenderEngineCairoGtk::drawRect(float xPos, float yPos, float fWidth, float fHeight)
{
   int xSt = (int)(xPos   * m_iRenderWidth);
   int ySt = (int)(yPos   * m_iRenderHeight);
   int w   = (int)(fWidth  * m_iRenderWidth);
   int h   = (int)(fHeight * m_iRenderHeight);

   if ( (xSt + w <= 0) || (ySt + h <= 0) || xSt >= m_iRenderWidth || ySt >= m_iRenderHeight )
      return;
   if ( xSt < 0 ) { w += xSt; xSt = 0; }
   if ( ySt < 0 ) { h += ySt; ySt = 0; }
   if ( xSt + w > m_iRenderWidth  ) w = m_iRenderWidth  - xSt;
   if ( ySt + h > m_iRenderHeight ) h = m_iRenderHeight - ySt;
   if ( w <= 0 || h <= 0 ) return;

   cairo_surface_flush(m_pBackSurface);

   // Fill
   if ( m_ColorFill[3] > 2 )
   {
      u8* pData   = cairo_image_surface_get_data(m_pBackSurface);
      int iStride = cairo_image_surface_get_stride(m_pBackSurface);
      u8 b = m_ColorFill[2], g = m_ColorFill[1], r = m_ColorFill[0], a = m_ColorFill[3];
      for ( int y = 0; y < h; y++ )
      {
         u8* pLine = pData + (ySt + y) * iStride + xSt * 4;
         for ( int x = 0; x < w; x++ )
         {
            *pLine++ = b; *pLine++ = g; *pLine++ = r; *pLine++ = a;
         }
      }
   }

   // Stroke border
   if ( m_ColorStroke[3] > 2 && m_fStrokeSizePx > 0.00001f )
   {
      u8 r = m_ColorStroke[0], g = m_ColorStroke[1], b = m_ColorStroke[2], a = m_ColorStroke[3];
      if ( yPos >= 0 )          _draw_hline(xSt, ySt,       w, r, g, b, a);
      if ( yPos + fHeight < 1 ) _draw_hline(xSt, ySt+h-1,  w, r, g, b, a);
      if ( xPos >= 0 )          _draw_vline(xSt,   ySt+1,   h-1, r, g, b, a);
      if ( xPos + fWidth < 1 )  _draw_vline(xSt+w-1, ySt+1, h-1, r, g, b, a);
   }

   cairo_surface_mark_dirty(m_pBackSurface);
}

void RenderEngineCairoGtk::drawRoundRect(float xPos, float yPos, float fWidth, float fHeight, float fCornerRadius)
{
   int xSt = (int)(xPos   * m_iRenderWidth);
   int ySt = (int)(yPos   * m_iRenderHeight);
   int w   = (int)(fWidth  * m_iRenderWidth);
   int h   = (int)(fHeight * m_iRenderHeight);

   if ( xSt >= m_iRenderWidth  || ySt >= m_iRenderHeight  ) return;
   if ( xSt + w <= 0 || ySt + h <= 0 ) return;
   if ( xSt < 0 ) { w += xSt; xSt = 0; }
   if ( ySt < 0 ) { h += ySt; ySt = 0; }
   if ( xSt + w >= m_iRenderWidth  ) w = m_iRenderWidth  - xSt - 1;
   if ( ySt + h >= m_iRenderHeight ) h = m_iRenderHeight - ySt - 1;
   if ( w < (int)(6.0f * m_fPixelWidth) || h < (int)(6.0f * m_fPixelHeight) ) return;

   cairo_surface_flush(m_pBackSurface);

   if ( m_ColorFill[3] > 2 )
   {
      u8* pData   = cairo_image_surface_get_data(m_pBackSurface);
      int iStride = cairo_image_surface_get_stride(m_pBackSurface);
      u8 r = m_ColorFill[0], g = m_ColorFill[1], b = m_ColorFill[2], a = m_ColorFill[3];

      for ( int y = 0; y < h; y++ )
      {
         u8* pLine = pData + (ySt + y) * iStride + (xSt + 3) * 4;
         for ( int x = 0; x < (w - 5); x++ )
         {
            *pLine++ = b; *pLine++ = g; *pLine++ = r; *pLine++ = a;
         }
      }
      _draw_vline(xSt+2, ySt+1, h-2, r, g, b, a);
      _draw_vline(xSt+1, ySt+1, h-2, r, g, b, a);
      _draw_vline(xSt,   ySt+3, h-6, r, g, b, a);
      _draw_vline(xSt+w-2, ySt+1, h-2, r, g, b, a);
      _draw_vline(xSt+w-1, ySt+1, h-2, r, g, b, a);
      _draw_vline(xSt+w,   ySt+3, h-6, r, g, b, a);
   }

   if ( m_ColorStroke[3] > 2 && m_fStrokeSizePx >= 0.9f )
   if ( m_ColorStroke[0] != m_ColorFill[0] || m_ColorStroke[1] != m_ColorFill[1] ||
        m_ColorStroke[2] != m_ColorFill[2] || m_ColorStroke[3] != m_ColorFill[3] )
   {
      u8 r = m_ColorStroke[0], g = m_ColorStroke[1], b = m_ColorStroke[2], a = m_ColorStroke[3];
      _draw_hline(xSt+3,   ySt,    w-6, r, g, b, a);
      _draw_hline(xSt+1,   ySt+1,  2,   r, g, b, a);
      _draw_hline(xSt+w-4, ySt+1,  2,   r, g, b, a);
      _draw_hline(xSt+3,   ySt+h,  w-6, r, g, b, a);
      _draw_hline(xSt+1,   ySt+h-1,2,   r, g, b, a);
      _draw_hline(xSt+w-4, ySt+h-1,2,   r, g, b, a);
      _draw_vline(xSt,     ySt+3,  h-6, r, g, b, a);
      _draw_vline(xSt+1,   ySt+1,  2,   r, g, b, a);
      _draw_vline(xSt+1,   ySt+h-3,2,   r, g, b, a);
      _draw_vline(xSt+w,   ySt+3,  h-6, r, g, b, a);
      _draw_vline(xSt+w-1, ySt+1,  2,   r, g, b, a);
      _draw_vline(xSt+w-1, ySt+h-3,2,   r, g, b, a);
   }

   cairo_surface_mark_dirty(m_pBackSurface);
}

void RenderEngineCairoGtk::drawRoundRectMenu(float xPos, float yPos, float fWidth, float fHeight, float fCornerRadius)
{
   if ( m_ColorFill[3] < 150 )
   {
      drawRoundRect(xPos, yPos, fWidth, fHeight, fCornerRadius);
      return;
   }

   int xSt = (int)(xPos   * m_iRenderWidth);
   int ySt = (int)(yPos   * m_iRenderHeight);
   int w   = (int)(fWidth  * m_iRenderWidth);
   int h   = (int)(fHeight * m_iRenderHeight);

   if ( xSt >= m_iRenderWidth  || ySt >= m_iRenderHeight  ) return;
   if ( xSt + w <= 0 || ySt + h <= 0 ) return;
   if ( xSt < 0 ) { w += xSt; xSt = 0; }
   if ( ySt < 0 ) { h += ySt; ySt = 0; }
   if ( xSt + w >= m_iRenderWidth  ) w = m_iRenderWidth  - xSt - 1;
   if ( ySt + h >= m_iRenderHeight ) h = m_iRenderHeight - ySt - 1;
   if ( w < (int)(6.0f * m_fPixelWidth) || h < (int)(6.0f * m_fPixelHeight) ) return;

   if ( NULL == m_pCairoCtx ) return;

   if ( m_ColorFill[3] > 2 )
   {
      cairo_surface_flush(m_pBackSurface);

      // Use Cairo for the filled rectangle (allows proper alpha blending).
      cairo_rectangle(m_pCairoCtx, xSt, ySt, w, h);
      cairo_set_source_rgba(m_pCairoCtx,
                            m_ColorFill[0] / 255.0,
                            m_ColorFill[1] / 255.0,
                            m_ColorFill[2] / 255.0,
                            m_ColorFill[3] / 255.0);
      cairo_fill(m_pCairoCtx);

      // Rounded corner columns via direct pixel access.
      u8 r = m_ColorFill[0], g = m_ColorFill[1], b = m_ColorFill[2], a = m_ColorFill[3];
      _draw_vline(xSt+2, ySt+1, h-2, r, g, b, a);
      _draw_vline(xSt+1, ySt+1, h-2, r, g, b, a);
      _draw_vline(xSt,   ySt+3, h-6, r, g, b, a);
      _draw_vline(xSt+w-2, ySt+1, h-2, r, g, b, a);
      _draw_vline(xSt+w-1, ySt+1, h-2, r, g, b, a);
      _draw_vline(xSt+w,   ySt+3, h-6, r, g, b, a);

      cairo_surface_mark_dirty(m_pBackSurface);
   }

   if ( m_ColorStroke[3] > 2 && m_fStrokeSizePx >= 0.9f )
   if ( m_ColorStroke[0] != m_ColorFill[0] || m_ColorStroke[1] != m_ColorFill[1] ||
        m_ColorStroke[2] != m_ColorFill[2] || m_ColorStroke[3] != m_ColorFill[3] )
   {
      cairo_surface_flush(m_pBackSurface);
      u8 r = m_ColorStroke[0], g = m_ColorStroke[1], b = m_ColorStroke[2], a = m_ColorStroke[3];
      _draw_hline(xSt+3,   ySt,    w-6, r, g, b, a);
      _draw_hline(xSt+1,   ySt+1,  2,   r, g, b, a);
      _draw_hline(xSt+w-4, ySt+1,  2,   r, g, b, a);
      _draw_hline(xSt+3,   ySt+h,  w-6, r, g, b, a);
      _draw_hline(xSt+1,   ySt+h-1,2,   r, g, b, a);
      _draw_hline(xSt+w-4, ySt+h-1,2,   r, g, b, a);
      _draw_vline(xSt,     ySt+3,  h-6, r, g, b, a);
      _draw_vline(xSt+1,   ySt+1,  2,   r, g, b, a);
      _draw_vline(xSt+1,   ySt+h-3,2,   r, g, b, a);
      _draw_vline(xSt+w,   ySt+3,  h-6, r, g, b, a);
      _draw_vline(xSt+w-1, ySt+1,  2,   r, g, b, a);
      _draw_vline(xSt+w-1, ySt+h-3,2,   r, g, b, a);
      cairo_surface_mark_dirty(m_pBackSurface);
   }
}

void RenderEngineCairoGtk::drawTriangle(float x1, float y1, float x2, float y2, float x3, float y3)
{
   if ( NULL == m_pCairoCtx ) return;
   cairo_move_to(m_pCairoCtx, x1 * m_iRenderWidth, y1 * m_iRenderHeight);
   cairo_line_to(m_pCairoCtx, x2 * m_iRenderWidth, y2 * m_iRenderHeight);
   cairo_line_to(m_pCairoCtx, x3 * m_iRenderWidth, y3 * m_iRenderHeight);
   cairo_close_path(m_pCairoCtx);
   cairo_set_source_rgba(m_pCairoCtx,
                         m_ColorStroke[0] / 255.0, m_ColorStroke[1] / 255.0,
                         m_ColorStroke[2] / 255.0, m_ColorStroke[3] / 255.0);
   cairo_stroke(m_pCairoCtx);
}

void RenderEngineCairoGtk::fillTriangle(float x1, float y1, float x2, float y2, float x3, float y3)
{
   if ( NULL == m_pCairoCtx ) return;
   cairo_move_to(m_pCairoCtx, x1 * m_iRenderWidth, y1 * m_iRenderHeight);
   cairo_line_to(m_pCairoCtx, x2 * m_iRenderWidth, y2 * m_iRenderHeight);
   cairo_line_to(m_pCairoCtx, x3 * m_iRenderWidth, y3 * m_iRenderHeight);
   cairo_close_path(m_pCairoCtx);

   bool bStroke = (m_ColorStroke[3] > 2) && (m_fStrokeSizePx > 0.00001f);

   if ( m_ColorFill[3] > 2 )
   {
      cairo_set_source_rgba(m_pCairoCtx,
                            m_ColorFill[0] / 255.0, m_ColorFill[1] / 255.0,
                            m_ColorFill[2] / 255.0, m_ColorFill[3] / 255.0);
      if ( bStroke )
         cairo_fill_preserve(m_pCairoCtx);
      else
         cairo_fill(m_pCairoCtx);
   }
   if ( bStroke )
   {
      cairo_set_source_rgba(m_pCairoCtx,
                            m_ColorStroke[0] / 255.0, m_ColorStroke[1] / 255.0,
                            m_ColorStroke[2] / 255.0, m_ColorStroke[3] / 255.0);
      cairo_stroke(m_pCairoCtx);
   }
}

void RenderEngineCairoGtk::drawPolyLine(float* x, float* y, int count)
{
   for ( int i = 0; i < count - 1; i++ )
      drawLine(x[i], y[i], x[i+1], y[i+1]);
   drawLine(x[count-1], y[count-1], x[0], y[0]);
}

void RenderEngineCairoGtk::fillPolygon(float* x, float* y, int count)
{
   if ( count < 3 || count > 120 ) return;

   float xIntersections[256];
   int   countIntersections = 0;
   float yMin, yMax, xMin, xMax;

   xMin = xMax = x[0];
   yMin = yMax = y[0];
   for ( int i = 1; i < count; i++ )
   {
      if ( x[i] < xMin ) xMin = x[i];
      if ( y[i] < yMin ) yMin = y[i];
      if ( x[i] > xMax ) xMax = x[i];
      if ( y[i] > yMax ) yMax = y[i];
   }

   for ( float yLine = yMin; yLine <= yMax; yLine += m_fPixelHeight )
   {
      countIntersections = 0;
      for ( int i = 0; i < count; i++ )
      {
         int j = (i + 1) % count;
         if ( fabs(y[i] - yLine) < 0.3f * m_fPixelHeight && fabs(y[j] - yLine) < 0.3f * m_fPixelHeight )
            drawLine(x[i], y[i], x[j], y[j]);
         else if ( y[i] <= yLine && y[j] >= yLine )
            xIntersections[countIntersections++] = x[i] + (x[j]-x[i])*(yLine-y[i])/(y[j]-y[i]);
         else if ( y[i] >= yLine && y[j] <= yLine )
            xIntersections[countIntersections++] = x[j] + (x[i]-x[j])*(yLine-y[j])/(y[i]-y[j]);
      }

      for ( int i = 0; i < countIntersections - 1; i++ )
      for ( int j = i + 1; j < countIntersections; j++ )
         if ( xIntersections[i] > xIntersections[j] )
         {
            float tmp = xIntersections[i];
            xIntersections[i] = xIntersections[j];
            xIntersections[j] = tmp;
         }

      if ( countIntersections > 2 && countIntersections % 2 )
      for ( int i = 0; i < countIntersections - 1; i++ )
         if ( fabs(xIntersections[i] - xIntersections[i+1]) < 0.0001f )
         {
            while ( i < countIntersections - 1 )
               xIntersections[i] = xIntersections[++i];
            countIntersections--;
            break;
         }

      for ( int i = 0; i < countIntersections; i += 2 )
         if ( xIntersections[i] >= xMin && xIntersections[i] <= xMax &&
              xIntersections[i+1] >= xMin && xIntersections[i+1] <= xMax )
            drawLine(xIntersections[i], yLine, xIntersections[i+1], yLine);
   }

   for ( int i = 0; i < count - 1; i++ )
      drawLine(x[i], y[i], x[i+1], y[i+1]);
   drawLine(x[count-1], y[count-1], x[0], y[0]);
}

void RenderEngineCairoGtk::fillCircle(float x, float y, float r)
{
   if ( NULL == m_pCairoCtx ) return;
   if ( m_ColorFill[3] > 2 )
   {
      cairo_set_source_rgba(m_pCairoCtx,
                            m_ColorFill[0] / 255.0, m_ColorFill[1] / 255.0,
                            m_ColorFill[2] / 255.0, m_ColorFill[3] / 255.0);
      cairo_move_to(m_pCairoCtx, x * m_iRenderWidth + r * m_iRenderHeight, y * m_iRenderHeight);
      cairo_arc(m_pCairoCtx, x * m_iRenderWidth, y * m_iRenderHeight, r * m_iRenderHeight, 0.0, 2.0 * M_PI);
      cairo_fill(m_pCairoCtx);
   }
   if ( m_ColorStroke[3] > 2 )
   {
      cairo_set_source_rgba(m_pCairoCtx,
                            m_ColorStroke[0] / 255.0, m_ColorStroke[1] / 255.0,
                            m_ColorStroke[2] / 255.0, m_ColorStroke[3] / 255.0);
      cairo_move_to(m_pCairoCtx, x * m_iRenderWidth + r * m_iRenderHeight, y * m_iRenderHeight);
      cairo_arc(m_pCairoCtx, x * m_iRenderWidth, y * m_iRenderHeight, r * m_iRenderHeight, 0.0, 2.0 * M_PI);
      cairo_stroke(m_pCairoCtx);
   }
}

void RenderEngineCairoGtk::drawCircle(float x, float y, float r)
{
   if ( NULL == m_pCairoCtx ) return;
   if ( m_ColorStroke[3] > 2 )
   {
      cairo_set_source_rgba(m_pCairoCtx,
                            m_ColorStroke[0] / 255.0, m_ColorStroke[1] / 255.0,
                            m_ColorStroke[2] / 255.0, m_ColorStroke[3] / 255.0);
      cairo_move_to(m_pCairoCtx, x * m_iRenderWidth + r * m_iRenderHeight, y * m_iRenderHeight);
      cairo_arc(m_pCairoCtx, x * m_iRenderWidth, y * m_iRenderHeight, r * m_iRenderHeight, 0.0, 2.0 * M_PI);
      cairo_stroke(m_pCairoCtx);
   }
}

void RenderEngineCairoGtk::drawArc(float /*x*/, float /*y*/, float /*r*/, float /*a1*/, float /*a2*/)
{
   // Not yet implemented – mirrors DRM backend.
}


// ===========================================================================
// Font handling
// ===========================================================================

void RenderEngineCairoGtk::_updateCurrentFontToUse(RenderEngineRawFont* pFont, bool bForce)
{
   cairo_t* pCairoCtx = _getActiveCairoContext();
   if ( NULL == pCairoCtx )
      pCairoCtx = _createTempDrawContext();
   if ( NULL == pCairoCtx )
      return;

   if ( m_bMustTestFontAccess )
   {
      log_line("[RenderEngineCairoGtk] Testing access to fonts...");
      m_bMustTestFontAccess = false;
      m_bHasNewFont = false;
#if defined(HW_PLATFORM_RASPBERRY) || defined(HW_PLATFORM_RADXA) || defined(HW_PLATFORM_LINUX_GENERIC)
      if ( access("/usr/share/fonts/truetype/noto/noto.ttf", R_OK) != -1 )
         m_bHasNewFont = true;
#endif
      log_line("[RenderEngineCairoGtk] Font test result: %s", m_bHasNewFont ? "ok" : "failed");
   }

   if ( NULL == pFont )
   {
      log_softerror_and_alarm("[RenderEngineCairoGtk] NULL font object – using default.");
      if ( m_bHasNewFont )
         cairo_select_font_face(pCairoCtx, "Noto Sans SC", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      else
         cairo_select_font_face(pCairoCtx, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      s_iLastCairoFontFamilyId  = -1;
      s_bLastCairoFontStyleBold = false;
      return;
   }

   if ( bForce || (s_iLastCairoFontFamilyId != pFont->iFamilyId) || (s_bLastCairoFontStyleBold != pFont->bBold) )
   {
      s_iLastCairoFontFamilyId  = pFont->iFamilyId;
      s_bLastCairoFontStyleBold = pFont->bBold;

      cairo_font_weight_t iStyle = pFont->bBold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL;

      if ( !m_bHasNewFont )
         cairo_select_font_face(pCairoCtx, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      else if ( s_iLastCairoFontFamilyId == 0 )
         cairo_select_font_face(pCairoCtx, "Noto Sans SC", CAIRO_FONT_SLANT_NORMAL, iStyle);
      else if ( s_iLastCairoFontFamilyId == 1 )
         cairo_select_font_face(pCairoCtx, "DejaVu Sans",  CAIRO_FONT_SLANT_NORMAL, iStyle);
      else
         cairo_select_font_face(pCairoCtx, "Nimbus Sans",  CAIRO_FONT_SLANT_NORMAL, iStyle);
   }
}

float RenderEngineCairoGtk::textRawWidthScaled(u32 fontId, float fScale, const char* szText)
{
   RenderEngineRawFont* pFont = _getRawFontFromId(fontId);
   if ( NULL == pFont || NULL == szText || 0 == szText[0] )
      return 0.0f;

   cairo_t* pCairoCtx = _getActiveCairoContext();
   if ( NULL == pCairoCtx )
      pCairoCtx = _createTempDrawContext();
   if ( NULL == pCairoCtx )
      return 0.0f;

   _updateCurrentFontToUse(pFont, true);
   int iPixels = (int)(pFont->lineHeight * 0.8f * fScale);
   if ( iPixels < 6 ) iPixels = 6;
   cairo_set_font_size(pCairoCtx, iPixels);

   char szTxt[256];
   memset(szTxt, 0, sizeof(szTxt));
   strncpy(szTxt, szText, sizeof(szTxt) - 3);

   cairo_scaled_font_t*         pSFont       = cairo_get_scaled_font(pCairoCtx);
   cairo_glyph_t*               glyphs       = NULL;
   int                          glyph_count  = 0;
   cairo_text_cluster_t*        clusters     = NULL;
   int                          cluster_count= 0;
   cairo_text_cluster_flags_t   clusterflags;

   cairo_status_t result = cairo_scaled_font_text_to_glyphs(pSFont, 0, 0, szTxt, (int)strlen(szTxt),
                                                             &glyphs, &glyph_count,
                                                             &clusters, &cluster_count, &clusterflags);
   if ( result != CAIRO_STATUS_SUCCESS )
   {
      if ( glyphs   ) cairo_glyph_free(glyphs);
      if ( clusters ) cairo_text_cluster_free(clusters);
      log_softerror_and_alarm("[RenderEngineCairoGtk] Failed to get text width for (%s)", szTxt);
      return 0.0f;
   }
   if ( glyph_count == 0 && cluster_count == 0 )
   {
      if ( glyphs   ) cairo_glyph_free(glyphs);
      if ( clusters ) cairo_text_cluster_free(clusters);
      return 0.0f;
   }

   float fWidthPixels = 0.0f;
   int   glyph_index  = 0;
   for ( int i = 0; i < cluster_count; i++ )
   {
      cairo_text_extents_t ext;
      cairo_scaled_font_glyph_extents(pSFont, &glyphs[glyph_index], clusters[i].num_glyphs, &ext);
      fWidthPixels += (float)ext.x_advance;
      glyph_index  += clusters[i].num_glyphs;
   }

   if ( glyphs   ) cairo_glyph_free(glyphs);
   if ( clusters ) cairo_text_cluster_free(clusters);

   if ( fWidthPixels <= 1.0f ) return 0.0f;
   return fWidthPixels * m_fPixelWidth * fScale;
}

void RenderEngineCairoGtk::_drawSimpleText(RenderEngineRawFont* pFont, const char* szText, float xPos, float yPos)
{
   _drawSimpleTextScaled(pFont, szText, xPos, yPos, 1.0f);
}

void RenderEngineCairoGtk::_drawSimpleTextScaled(RenderEngineRawFont* pFont, const char* szText,
                                                  float xPos, float yPos, float fScale)
{
   if ( NULL == pFont )
   {
      log_error_and_alarm("[RenderEngineCairoGtk] _drawSimpleTextScaled: NULL font.");
      return;
   }
   if ( NULL == szText || 0 == szText[0] )
   {
      log_softerror_and_alarm("[RenderEngineCairoGtk] _drawSimpleTextScaled: empty string.");
      return;
   }
   if ( !m_bStartedFrame )
   {
      log_error_and_alarm("[RenderEngineCairoGtk] _drawSimpleTextScaled: called outside render frame.");
      return;
   }
   if ( yPos < 0 ) return;
   if ( xPos >= 1.0f ) return;
   if ( yPos + pFont->lineHeight * fScale * m_fPixelHeight >= 1.0f ) return;

   char szTxt[256];
   memset(szTxt, 0, sizeof(szTxt));
   strncpy(szTxt, szText, sizeof(szTxt) - 3);

   u32 uFontId = _getRawFontId(pFont);
   float fRenderWidth = textRawWidthScaled(uFontId, fScale, szTxt);
   if ( fRenderWidth <= m_fPixelWidth )
   {
      log_softerror_and_alarm("[RenderEngineCairoGtk] _drawSimpleTextScaled: zero-width text (%s)", szTxt);
      return;
   }

   if ( m_bDrawBackgroundBoundingBoxes )
      _drawSimpleTextBoundingBox(pFont, szTxt, xPos, yPos, 1.0f);

   float fColor[4];
   if ( m_bDrawBackgroundBoundingBoxes && m_bDrawBackgroundBoundingBoxesTextUsesSameStrokeColor )
   {
      fColor[0] = m_ColorTextBackgroundBoundingBoxStrike[0] / 255.0f;
      fColor[1] = m_ColorTextBackgroundBoundingBoxStrike[1] / 255.0f;
      fColor[2] = m_ColorTextBackgroundBoundingBoxStrike[2] / 255.0f;
      fColor[3] = m_ColorTextBackgroundBoundingBoxStrike[3] / 255.0f;
   }
   else
   {
      fColor[0] = m_ColorFill[0] / 255.0f;
      fColor[1] = m_ColorFill[1] / 255.0f;
      fColor[2] = m_ColorFill[2] / 255.0f;
      fColor[3] = m_ColorFill[3] / 255.0f;
   }
   if ( fColor[3] < 0.25f || fColor[3] >= 1.0f )
      fColor[3] = 1.0f;

   cairo_set_source_rgba(m_pCairoCtx, fColor[0], fColor[1], fColor[2], fColor[3]);
   cairo_move_to(m_pCairoCtx,
                 xPos * m_iRenderWidth,
                 yPos * m_iRenderHeight + pFont->baseLine);
   _updateCurrentFontToUse(pFont, true);
   int iPixels = (int)(pFont->lineHeight * 0.8f * fScale);
   if ( iPixels < 6 ) iPixels = 6;
   cairo_set_font_size(m_pCairoCtx, iPixels);
   cairo_show_text(m_pCairoCtx, szTxt);
}

void RenderEngineCairoGtk::_bltFontChar(int iDestX, int iDestY,
                                         int iSrcX, int iSrcY,
                                         int iSrcWidth, int iSrcHeight,
                                         RenderEngineRawFont* pFont)
{
   if ( NULL == pFont || NULL == pFont->pImageObject ) return;
   if ( iDestX < 0 || iDestY < 0 ||
        iDestX + iSrcWidth  >= m_iRenderWidth ||
        iDestY + iSrcHeight >= m_iRenderHeight )
      return;

   cairo_surface_flush(m_pBackSurface);
   u8* pBackData   = cairo_image_surface_get_data(m_pBackSurface);
   int iBackStride = cairo_image_surface_get_stride(m_pBackSurface);
   u8* pSrcData    = cairo_image_surface_get_data((cairo_surface_t*)pFont->pImageObject);
   int iSrcStride  = cairo_image_surface_get_stride((cairo_surface_t*)pFont->pImageObject);

   for ( int y = 0; y < iSrcHeight; y++ )
   {
      u8* pDest = pBackData + (iDestY + y) * iBackStride + iDestX * 4;
      u8* pSrc  = pSrcData  + (iSrcY  + y) * iSrcStride  + iSrcX  * 4;
      for ( int x = 0; x < iSrcWidth; x++ )
      {
         u8 uAlpha = pSrc[3];
         pDest[0] = (u8)((pSrc[0] * uAlpha + pDest[0] * (255 - uAlpha)) / 256);
         pDest[1] = (u8)((pSrc[1] * uAlpha + pDest[1] * (255 - uAlpha)) / 256);
         pDest[2] = (u8)((pSrc[2] * uAlpha + pDest[2] * (255 - uAlpha)) / 256);
         pDest   += 4;
         pSrc    += 4;
      }
   }
   cairo_surface_mark_dirty(m_pBackSurface);
}