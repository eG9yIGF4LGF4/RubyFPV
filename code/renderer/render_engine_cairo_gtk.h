#pragma once
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

#include "render_engine.h"
#include <cairo.h>
#include <gtk/gtk.h>

// GTK-backed Cairo render engine.
// Surface lifecycle: a single cairo_image_surface_t (ARGB32) is maintained as
// the back-buffer.  On endFrame() its pixel data is blitted into the GTK
// drawing-area via a "draw" / "render" signal handler that is connected by the
// caller after construction (see connectDrawSignal()).
//
// Coordinate convention: identical to the DRM backend – all x/y/w/h arguments
// are normalised [0,1] floats; pixel coordinates are obtained by multiplying
// with m_iRenderWidth / m_iRenderHeight.

class RenderEngineCairoGtk : public RenderEngine
{
   public:
      RenderEngineCairoGtk();
      virtual ~RenderEngineCairoGtk();

      // Must be called after construction to hook the engine into a GtkWidget
      // (typically a GtkDrawingArea).  The widget's "draw" signal is connected
      // internally; on each signal the back-buffer contents are painted into
      // the provided cairo_t supplied by GTK.
      void connectDrawSignal(GtkWidget* pWidget);

      // Called by the application's main loop whenever a new frame should be
      // displayed.  Triggers gtk_widget_queue_draw() on the connected widget.
      void requestRedraw();

      // --- RenderEngine interface -------------------------------------------
      virtual void* getDrawContext();
      virtual void setStroke(const double* color, float fStrokeSize);
      virtual void setStrokeSize(float fStrokeSize);

      virtual void setFontOutlineColor(u32 idFont, u8 r, u8 g, u8 b, u8 a);
      virtual u32  loadImage(const char* szFile);
      virtual void freeImage(u32 idImage);
      virtual u32  loadIcon(const char* szFile);
      virtual void freeIcon(u32 idIcon);
      virtual int  getImageWidth(u32 uImageId);
      virtual int  getImageHeight(u32 uImageId);
      virtual void changeImageHue(u32 uImageId, u8 r, u8 g, u8 b);

      virtual void startFrame();
      virtual void endFrame();
      virtual void rotate180();

      virtual void drawImage(float xPos, float yPos, float fWidth, float fHeight, u32 uImageId);
      virtual void drawImageAlpha(float xPos, float yPos, float fWidth, float fHeight, u32 uImageId, u8 uAlpha);
      virtual void bltImage(float xPosDest, float yPosDest, float fWidthDest, float fHeightDest, int iSrcX, int iSrcY, int iSrcWidth, int iSrcHeight, u32 uImageId);
      virtual void bltSprite(float xPosDest, float yPosDest, int iSrcX, int iSrcY, int iSrcWidth, int iSrcHeight, u32 uImageId);
      virtual void drawIcon(float xPos, float yPos, float fWidth, float fHeight, u32 uIconId);
      virtual void bltIcon(float xPosDest, float yPosDest, int iSrcX, int iSrcY, int iSrcWidth, int iSrcHeight, u32 uIconId);

      virtual float textRawWidthScaled(u32 fontId, float fScale, const char* szText);

      virtual void drawLine(float x1, float y1, float x2, float y2);
      virtual void drawRect(float xPos, float yPos, float fWidth, float fHeight);
      virtual void drawRoundRect(float xPos, float yPos, float fWidth, float fHeight, float fCornerRadius);
      virtual void drawRoundRectMenu(float xPos, float yPos, float fWidth, float fHeight, float fCornerRadius);
      virtual void drawTriangle(float x1, float y1, float x2, float y2, float x3, float y3);
      virtual void fillTriangle(float x1, float y1, float x2, float y2, float x3, float y3);
      virtual void drawPolyLine(float* x, float* y, int count);
      virtual void fillPolygon(float* x, float* y, int count);

      virtual void fillCircle(float x, float y, float r);
      virtual void drawCircle(float x, float y, float r);
      virtual void drawArc(float x, float y, float r, float a1, float a2);

      virtual void* _loadRawFontImageObject(const char* szFileName);
      virtual void  _freeRawFontImageObject(void* pImageObject);

   protected:
      // Returns the active drawing context (frame ctx during a frame, temp ctx
      // outside of a frame – matches DRM backend semantics exactly).
      cairo_t* _getActiveCairoContext();

      // Creates (and caches) a temporary context bound to the back-buffer.
      // Used for text-width queries that arrive outside startFrame/endFrame.
      cairo_t* _createTempDrawContext();

      void _updateCurrentFontToUse(RenderEngineRawFont* pFont, bool bForce);
      virtual void _drawSimpleText(RenderEngineRawFont* pFont, const char* szText, float xPos, float yPos);
      virtual void _drawSimpleTextScaled(RenderEngineRawFont* pFont, const char* szText, float xPos, float yPos, float fScale);

      // Per-pixel helpers operating directly on the back-buffer data.
      void _bltFontChar(int iDestX, int iDestY, int iSrcX, int iSrcY, int iSrcWidth, int iSrcHeight, RenderEngineRawFont* pFont);
      void _blend_pixel(unsigned char* pixel, unsigned char r, unsigned char g, unsigned char b, unsigned char a);
      void _draw_hline(int x, int y, int w, unsigned char r, unsigned char g, unsigned char b, unsigned char a);
      void _draw_vline(int x, int y, int h, unsigned char r, unsigned char g, unsigned char b, unsigned char a);

      // GTK "draw" signal callback – paints the back-buffer surface into the
      // cairo_t provided by GTK's expose machinery.
      static gboolean _onDraw(GtkWidget* pWidget, cairo_t* pCr, gpointer pUserData);

      // --- back-buffer -------------------------------------------------------
      // Single off-screen ARGB32 surface used as the render target.
      cairo_surface_t* m_pBackSurface;  // image surface, size = render w×h
      cairo_t*         m_pCairoCtx;     // context bound to m_pBackSurface (valid inside frame)
      cairo_t*         m_pCairoTempCtx; // temporary context for out-of-frame queries

      // --- GTK plumbing ------------------------------------------------------
      GtkWidget* m_pGtkWidget;          // the drawing area we paint into
      gulong     m_uDrawSignalId;       // signal handler id (for disconnect on destroy)

      // --- font state --------------------------------------------------------
      bool m_bMustTestFontAccess;
      bool m_bHasNewFont;

      // --- image / icon tables (mirrors DRM backend layout) ------------------
      cairo_surface_t* m_pImages[MAX_RAW_IMAGES];
      u32              m_ImageIds[MAX_RAW_IMAGES];
      u32              m_CurrentImageId;
      int              m_iCountImages;

      cairo_surface_t* m_pIcons[MAX_RAW_ICONS];
      cairo_surface_t* m_pIconsMip[MAX_RAW_ICONS][2];
      u32              m_IconIds[MAX_RAW_ICONS];
      u32              m_CurrentIconId;
      int              m_iCountIcons;
};
