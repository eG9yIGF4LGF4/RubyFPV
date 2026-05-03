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

// ---------------------------------------------------------------------------
// ruby_player_gtk.cpp
//
// Drop-in replacement for ruby_player_radxa.cpp that uses GTK3 + Cairo for
// the UI overlay instead of DRM.  All four playback modes are preserved:
//   -f   play H264/H265 file
//   -p   play from pipe (FIFO)
//   -sm  play from shared memory
//   -u   play from UDP socket
//
// The video plane is still handled by mpp / DRM (NV12 format) exactly as
// before.  Only the UI overlay plane (DRM_FORMAT_ARGB8888) is replaced by
// a GTK3 window that hosts a RenderEngineCairoGtk drawing area.
// ---------------------------------------------------------------------------

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/random.h>
#include <inttypes.h>
#include <semaphore.h>
#include <pthread.h>
#include <ctype.h>

#include "../base/base.h"
#include "../base/config.h"
#include "../base/config_obj_names.h"
#include "../base/hardware.h"
#include "../base/hw_procs.h"
#include "../base/hdmi.h"
#include "../renderer/drm_core.h"
#include "../base/ctrl_settings.h"
#include "../renderer/render_engine.h"
#include "../renderer/render_engine_cairo_gtk.h"
#include "mpp_core.h"

#if !(defined(HW_PLATFORM_RADXA) || defined(HW_PLATFORM_LINUX_GENERIC))
#error "ONLY FOR RADXA OR LINUX GENERIC PLATFORM!"
#endif


// ---------------------------------------------------------------------------
// Global state (mirrors ruby_player_radxa.cpp)
// ---------------------------------------------------------------------------

bool g_bQuit            = false;
bool g_bDebug           = false;
bool g_bPlayFile        = false;
bool g_bPlayingIntro    = false;
bool g_bExitOnEnd       = false;
bool g_bPlayStreamPipe  = false;
bool g_bPlayStreamUDP   = false;
bool g_bPlayStreamSM    = false;
bool g_bInitUILayerToo  = false;
bool g_bUseH265Decoder  = false;

char g_szPlayFileName[MAX_FILE_PATH_SIZE];
int  g_iFileFPS           = 30;
int  g_iFileTempSlices    = 1;
int  g_iFileDetectedSlices= 1;
int  g_iCustomWidth       = 0;
int  g_iCustomHeight      = 0;
int  g_iCustomRefresh     = 0;

#define PIPE_BUFFER_SIZE 200000
u8  g_uPipeBuffer[PIPE_BUFFER_SIZE];
int g_iPipeBufferWritePos = 0;
int g_iPipeBufferReadPos  = 0;

sem_t* s_pSemaphoreSMData = NULL;

// ---------------------------------------------------------------------------
// GTK / render engine global state
// ---------------------------------------------------------------------------

static RenderEngineCairoGtk* s_pRenderEngine = NULL;
static GtkWidget*             s_pGtkWindow    = NULL;
static GtkWidget*             s_pDrawingArea  = NULL;

// GTK main loop runs on a dedicated thread so it doesn't block the video
// feed loops below.
static pthread_t s_gtkThread;

// ---------------------------------------------------------------------------
// GTK thread entry – creates the window and runs gtk_main()
// ---------------------------------------------------------------------------

static void* _gtk_thread_func(void* /*pArg*/)
{
   log_line("[GTK] Thread started.");

   s_pGtkWindow   = gtk_window_new(GTK_WINDOW_TOPLEVEL);
   s_pDrawingArea = gtk_drawing_area_new();

   gtk_window_set_title(GTK_WINDOW(s_pGtkWindow), "Ruby Player");
   gtk_window_set_decorated(GTK_WINDOW(s_pGtkWindow), FALSE);
   gtk_window_fullscreen(GTK_WINDOW(s_pGtkWindow));

   // Transparent background so the DRM video plane underneath shows through.
   GdkScreen*   pScreen  = gtk_widget_get_screen(s_pGtkWindow);
   GdkVisual*   pVisual  = gdk_screen_get_rgba_visual(pScreen);
   if ( NULL != pVisual )
      gtk_widget_set_visual(s_pGtkWindow, pVisual);
   gtk_widget_set_app_paintable(s_pGtkWindow, TRUE);

   gtk_container_add(GTK_CONTAINER(s_pGtkWindow), s_pDrawingArea);

   g_signal_connect(s_pGtkWindow, "destroy", G_CALLBACK(gtk_main_quit), NULL);

   // Connect the engine draw callback to the drawing area.
   if ( NULL != s_pRenderEngine )
      s_pRenderEngine->connectDrawSignal(s_pDrawingArea);

   gtk_widget_show_all(s_pGtkWindow);

   log_line("[GTK] Entering gtk_main().");
   gtk_main();
   log_line("[GTK] gtk_main() returned.");

   g_bQuit = true;
   return NULL;
}

// ---------------------------------------------------------------------------
// Start the GTK thread.  Must be called after s_pRenderEngine is created.
// ---------------------------------------------------------------------------

static void _start_gtk_thread()
{
   if ( 0 != pthread_create(&s_gtkThread, NULL, _gtk_thread_func, NULL) )
      log_softerror_and_alarm("[GTK] Failed to create GTK thread.");
   else
      log_line("[GTK] GTK thread created.");

   // Give GTK time to create the window before any rendering starts.
   hardware_sleep_ms(200);
}

static void _stop_gtk_thread()
{
   // Ask GTK to quit from whichever thread we're on.
   if ( NULL != s_pGtkWindow )
      gdk_threads_add_idle((GSourceFunc)gtk_main_quit, NULL);

   pthread_join(s_gtkThread, NULL);
   log_line("[GTK] GTK thread joined.");
}

// ---------------------------------------------------------------------------
// Init / uninit the UI overlay (replaces ruby_drm_core_init for ARGB plane)
// ---------------------------------------------------------------------------

static void _init_ui_layer()
{
   log_line("Init UI overlay layer (GTK).");

   // gtk_init() is called inside RenderEngineCairoGtk constructor via
   // gtk_init_check(), but we must call it here first on the main thread
   // before spawning the GTK thread (GTK is not thread-safe at init time).
   int   argc_dummy = 0;
   char* argv_dummy = NULL;
   char**ppArgv = &argv_dummy;
   if ( !gtk_init_check(&argc_dummy, &ppArgv) )
      log_softerror_and_alarm("gtk_init_check failed – no display?");

   s_pRenderEngine = new RenderEngineCairoGtk();
   _start_gtk_thread();

   // Draw a minimal first frame so the overlay is transparent on startup.
   s_pRenderEngine->startFrame();
   s_pRenderEngine->endFrame();

   log_line("UI overlay layer (GTK) initialised.");
}

static void _uninit_ui_layer()
{
   log_line("Uninit UI overlay layer (GTK).");
   _stop_gtk_thread();
   delete s_pRenderEngine;
   s_pRenderEngine = NULL;
   s_pDrawingArea  = NULL;
   s_pGtkWindow    = NULL;
}


// ===========================================================================
// _do_player_mode  (play H264/H265 file)
// ===========================================================================

void _do_player_mode()
{
   ControllerSettings* pCS = get_ControllerSettings();

   ruby_drm_core_wait_for_display_connected();
   if ( hdmi_enum_modes() < 0 )
   {
      log_error_and_alarm("Failed to enumerate HDMI modes. Exit player.");
      return;
   }
   int iHDMIIndex = hdmi_load_current_mode();
   if ( iHDMIIndex < 0 )
      iHDMIIndex = hdmi_get_best_resolution_index_for(DEFAULT_RADXA_DISPLAY_WIDTH, DEFAULT_RADXA_DISPLAY_HEIGHT, DEFAULT_RADXA_DISPLAY_REFRESH);
   log_line("HDMI mode to use: %d (%d x %d @ %d)", iHDMIIndex,
      hdmi_get_current_resolution_width(), hdmi_get_current_resolution_height(),
      hdmi_get_current_resolution_refresh());

   hw_increase_current_thread_priority("RubyPlayer", 10);

   // UI overlay via GTK (replaces DRM ARGB8888 plane init).
   if ( g_bInitUILayerToo || g_bPlayingIntro )
   {
      _init_ui_layer();

      if ( !g_bPlayingIntro && NULL != s_pRenderEngine )
      {
         // Draw the same minimal 1% red dot as the original.
         s_pRenderEngine->startFrame();
         s_pRenderEngine->setFill(250, 0, 0, 0.5);
         s_pRenderEngine->setStroke(0, 0);
         s_pRenderEngine->drawRect(0.0, 0.0, 0.01, 0.01);
         s_pRenderEngine->endFrame();
      }
   }

   // Video plane – unchanged from original.
   log_line("Init display video layer.");
   ruby_drm_core_init(1, DRM_FORMAT_NV12,
      hdmi_get_current_resolution_width(),
      hdmi_get_current_resolution_height(),
      hdmi_get_current_resolution_refresh());

   if ( g_bPlayingIntro )
      for ( int i = 0; i < 26; i++ )
         hardware_sleep_ms(100);

   if ( mpp_init(g_bUseH265Decoder, pCS->iVideoMPPBuffersSize) != 0 )
   {
      log_softerror_and_alarm("Failed to init MPP. Exit player.");
      ruby_drm_core_uninit();
      if ( g_bInitUILayerToo || g_bPlayingIntro )
         _uninit_ui_layer();
      return;
   }

   if ( g_bPlayingIntro )
      for ( int i = 0; i < 10; i++ )
         hardware_sleep_ms(50);

   FILE* fp = fopen(g_szPlayFileName, "rb");
   if ( NULL == fp )
   {
      log_error_and_alarm("Failed to open input file [%s]. Exit.", g_szPlayFileName);
      ruby_drm_core_uninit();
      mpp_uninit();
      if ( g_bInitUILayerToo || g_bPlayingIntro )
         _uninit_ui_layer();
      return;
   }

   log_line("Opened input video file (%s), has %d FPS", g_szPlayFileName, g_iFileFPS);
   mpp_enable_vsync(pCS->iHDMIVSync ? true : false);
   mpp_start_decoding_thread();

   u32           uTimeLastCheck    = get_current_timestamp_ms();
   unsigned char uBuffer[4096];
   int           nRead             = 1;
   int           iCount            = 0;
   int           iTotalRead        = 0;
   u32           uCurrentParseToken= 0x11111111;
   u32           uNALType          = 0;
   u32           uPrevNALType      = 0;
   u32           uTimeLastFrame    = 0;

   while ( (nRead > 0) && (!g_bQuit) )
   {
      iCount++;
      nRead = fread(uBuffer, 1, sizeof(uBuffer), fp);
      if ( nRead <= 0 )
         break;

      // NAL boundary detection & frame-rate throttle – identical to original.
      u8* pTmp = &(uBuffer[0]);
      for ( int i = 0; i < nRead; i++ )
      {
         uCurrentParseToken = (uCurrentParseToken << 8) | (*pTmp);
         pTmp++;
         if ( uCurrentParseToken == 0x00000001 )
         if ( i < (nRead - 1) )
         {
            uNALType = (*pTmp) & 0x1F;

            if ( (uPrevNALType == 5) && (uNALType != 5) )
               g_iFileDetectedSlices = g_iFileTempSlices;

            if ( uPrevNALType == uNALType )
               g_iFileTempSlices++;
            else
               g_iFileTempSlices = 1;

            uPrevNALType = uNALType;

            if ( (g_iFileTempSlices % g_iFileDetectedSlices) == 0 )
            if ( (uNALType != 7) && (uNALType != 8) )
            {
               u32  uTimeNow = get_current_timestamp_ms();
               long miliSecs = (1000 / (g_iFileFPS - 4));
               miliSecs -= (long)(uTimeNow - uTimeLastFrame);
               uTimeLastFrame = uTimeNow;
               if ( miliSecs > 0 )
                  hardware_sleep_ms(miliSecs);
            }
         }
      }

      while ( (access("/tmp/pausedvr", R_OK) != -1) && (!g_bQuit) )
      {
         struct timespec to_sleep = { 0, (long int)(50 * 1000 * 1000) };
         clock_nanosleep(RUBY_HW_CLOCK_ID, 0, &to_sleep, NULL);
      }

      if ( g_bQuit )
         break;

      mpp_feed_data_to_decoder(uBuffer, nRead);
      iTotalRead += nRead;
      if ( (iCount % 10) == 0 )
      {
         u32 uTime = get_current_timestamp_ms();
         if ( uTime > uTimeLastCheck + 4000 )
         {
            uTimeLastCheck = uTime;
            log_line("Video player alive, reading %d bits/sec", iTotalRead * 8 / 4);
            iTotalRead = 0;
         }
      }
   }

   fclose(fp);
   log_line("Playback of file finished. End of file (%s). Exit on end: %s",
      g_szPlayFileName, g_bExitOnEnd ? "yes" : "no");

   if ( g_bExitOnEnd )
   {
      log_line("Exit on End.");
      char szFile[MAX_FILE_PATH_SIZE];
      strcpy(szFile, FOLDER_RUBY_TEMP);
      strcat(szFile, FILE_TEMP_INTRO_PLAYING);
      char szComm[256];
      snprintf(szComm, sizeof(szComm), "rm -rf %s", szFile);
      hw_execute_bash_command(szComm, NULL);
   }

   mpp_mark_end_of_stream();
   mpp_uninit();

   if ( g_bInitUILayerToo || g_bPlayingIntro )
      _uninit_ui_layer();
   if ( !g_bPlayingIntro )
      ruby_drm_core_uninit();
}


// ===========================================================================
// _do_stream_mode_pipe  (play from FIFO pipe)
// ===========================================================================

void _do_stream_mode_pipe()
{
   ControllerSettings* pCS = get_ControllerSettings();

   if ( hdmi_enum_modes() < 0 )
   {
      log_error_and_alarm("Failed to enumerate HDMI modes. Exit pipe player.");
      return;
   }

   int iHDMIIndex = hdmi_load_current_mode();
   if ( iHDMIIndex < 0 )
      iHDMIIndex = hdmi_get_best_resolution_index_for(DEFAULT_RADXA_DISPLAY_WIDTH, DEFAULT_RADXA_DISPLAY_HEIGHT, DEFAULT_RADXA_DISPLAY_REFRESH);
   log_line("HDMI mode to use: %d (%d x %d @ %d)", iHDMIIndex,
      hdmi_get_current_resolution_width(), hdmi_get_current_resolution_height(),
      hdmi_get_current_resolution_refresh());

   ruby_drm_core_init(1, DRM_FORMAT_NV12,
      hdmi_get_current_resolution_width(),
      hdmi_get_current_resolution_height(),
      hdmi_get_current_resolution_refresh());

   hw_increase_current_thread_priority("RubyPlayer", 10);

   if ( g_bInitUILayerToo )
      _init_ui_layer();

   if ( mpp_init(g_bUseH265Decoder, pCS->iVideoMPPBuffersSize) != 0 )
   {
      ruby_drm_core_uninit();
      if ( g_bInitUILayerToo )
         _uninit_ui_layer();
      return;
   }

   int readfd = open(FIFO_RUBY_STATION_VIDEO_STREAM_DISPLAY, O_RDONLY);
   if ( -1 == readfd )
   {
      log_error_and_alarm("Failed to open video stream fifo.");
      mpp_uninit();
      ruby_drm_core_uninit();
      if ( g_bInitUILayerToo )
         _uninit_ui_layer();
      return;
   }

   log_line("Opened input video stream fifo (%s)", FIFO_RUBY_STATION_VIDEO_STREAM_DISPLAY);
   mpp_enable_vsync(pCS->iHDMIVSync ? true : false);
   mpp_start_decoding_thread();

   u32  uTimeLastCheck          = get_current_timestamp_ms();
   int  nRead                   = 1;
   int  iCount                  = 0;
   int  iTotalRead              = 0;
   bool bAnyInputEver           = false;
   u32  uTimeStartReceivingStream = 0;

   while ( !g_bQuit )
   {
      g_pSMProcessStats->lastActiveTime = get_current_timestamp_ms();
      nRead = read(readfd, g_uPipeBuffer, PIPE_BUFFER_SIZE);
      if ( (nRead < 0) || g_bQuit )
      {
         if ( nRead < 0 )
         {
            log_line("Reached end of input stream data. Ending video streaming. errno: %d, (%s)",
               errno, strerror(errno));
            g_bQuit = true;
         }
         break;
      }
      if ( nRead == 0 )
      {
         hardware_sleep_micros(bAnyInputEver ? 1 * 1000 : 2 * 1000);
         continue;
      }
      g_pSMProcessStats->lastIPCIncomingTime = get_current_timestamp_ms();

      if ( !bAnyInputEver )
      {
         log_line("Start receiving video stream data through pipe (%d bytes)", nRead);
         bAnyInputEver = true;
         uTimeStartReceivingStream = get_current_timestamp_ms();
      }

      iCount++;
      iTotalRead += nRead;
      if ( (iCount % 10) == 0 )
      {
         u32 uTime = get_current_timestamp_ms();
         if ( uTime >= uTimeLastCheck + 4000 )
         {
            uTimeLastCheck = uTime;
            log_line("Video player alive, reading %d kbits/sec", iTotalRead * 8 / 4 / 1000);
            iTotalRead = 0;
         }
      }

      int iRes = mpp_feed_data_to_decoder(g_uPipeBuffer, nRead);
      if ( iRes > 5 )
      {
         log_line("Stalled consuming %d bytes, stall for %d ms. Signaling alarm", nRead, iRes);
         if ( get_current_timestamp_ms() > uTimeStartReceivingStream + 5000 )
         {
            sem_t* ps = sem_open(SEMAPHORE_VIDEO_STREAMER_OVERLOAD, O_CREAT, S_IWUSR | S_IRUSR, 0);
            sem_post(ps);
            sem_close(ps);
         }
      }
   }

   if ( g_bQuit )
      log_line("Ending video stream play due to quit signal.");

   close(readfd);
   mpp_mark_end_of_stream();
   mpp_uninit();

   if ( g_bInitUILayerToo )
      _uninit_ui_layer();
   ruby_drm_core_uninit();
}


// ===========================================================================
// _do_stream_mode_sm  (play from shared memory)
// ===========================================================================

void _do_stream_mode_sm()
{
   ControllerSettings* pCS = get_ControllerSettings();

   s_pSemaphoreSMData = sem_open(SEMAPHORE_SM_VIDEO_DATA_AVAILABLE, O_RDONLY);
   if ( (NULL == s_pSemaphoreSMData) || (SEM_FAILED == s_pSemaphoreSMData) )
   {
      log_error_and_alarm("Failed to create SM data read semaphore: %s", SEMAPHORE_SM_VIDEO_DATA_AVAILABLE);
      return;
   }
   int iSemVal = 0;
   if ( 0 == sem_getvalue(s_pSemaphoreSMData, &iSemVal) )
      log_line("SM data semaphore initial value: %d", iSemVal);
   else
      log_softerror_and_alarm("Failed to get SM data sem value.");

   if ( hdmi_enum_modes() < 0 )
   {
      log_error_and_alarm("Failed to enumerate HDMI modes. Exit SM player.");
      sem_close(s_pSemaphoreSMData);
      s_pSemaphoreSMData = NULL;
      return;
   }

   int iHDMIIndex = hdmi_load_current_mode();
   if ( iHDMIIndex < 0 )
      iHDMIIndex = hdmi_get_best_resolution_index_for(DEFAULT_RADXA_DISPLAY_WIDTH, DEFAULT_RADXA_DISPLAY_HEIGHT, DEFAULT_RADXA_DISPLAY_REFRESH);
   log_line("HDMI mode to use: %d (%d x %d @ %d)", iHDMIIndex,
      hdmi_get_current_resolution_width(), hdmi_get_current_resolution_height(),
      hdmi_get_current_resolution_refresh());

   ruby_drm_core_init(1, DRM_FORMAT_NV12,
      hdmi_get_current_resolution_width(),
      hdmi_get_current_resolution_height(),
      hdmi_get_current_resolution_refresh());

   hw_increase_current_thread_priority("RubyPlayer", 10);

   if ( g_bInitUILayerToo )
      _init_ui_layer();

   if ( mpp_init(g_bUseH265Decoder, pCS->iVideoMPPBuffersSize) != 0 )
   {
      sem_close(s_pSemaphoreSMData);
      s_pSemaphoreSMData = NULL;
      ruby_drm_core_uninit();
      if ( g_bInitUILayerToo )
         _uninit_ui_layer();
      return;
   }

   int            fdSMem          = -1;
   unsigned char* pSMem           = NULL;
   uint32_t       uSharedMemReadPos = 0;

   fdSMem = shm_open(SM_STREAMER_NAME, O_RDONLY, S_IRUSR | S_IWUSR);
   if ( fdSMem < 0 )
   {
      log_softerror_and_alarm("Failed to open shared memory for read: %s, error: %d %s",
         SM_STREAMER_NAME, errno, strerror(errno));
      sem_close(s_pSemaphoreSMData);
      s_pSemaphoreSMData = NULL;
      mpp_uninit();
      ruby_drm_core_uninit();
      if ( g_bInitUILayerToo )
         _uninit_ui_layer();
      return;
   }

   pSMem = (unsigned char*)mmap(NULL, SM_STREAMER_SIZE, PROT_READ, MAP_SHARED, fdSMem, 0);
   if ( (pSMem == MAP_FAILED) || (pSMem == NULL) )
   {
      log_softerror_and_alarm("Failed to map shared memory: %s, error: %d %s",
         SM_STREAMER_NAME, errno, strerror(errno));
      if ( pSMem && pSMem != MAP_FAILED )
         munmap(pSMem, SM_STREAMER_SIZE);
      sem_close(s_pSemaphoreSMData);
      s_pSemaphoreSMData = NULL;
      mpp_uninit();
      ruby_drm_core_uninit();
      if ( g_bInitUILayerToo )
         _uninit_ui_layer();
      return;
   }
   close(fdSMem);
   log_line("Mapped shared mem: %s", SM_STREAMER_NAME);

   mpp_enable_vsync(pCS->iHDMIVSync ? true : false);
   mpp_start_decoding_thread();

   u32  uTimeLastCheck           = get_current_timestamp_ms();
   int  nRead                    = 1;
   int  iCount                   = 0;
   int  iTotalRead               = 0;
   bool bAnyInputEver            = false;
   u32  uTimeStartReceivingStream= 0;

   while ( !g_bQuit )
   {
      u32* pTmp1       = (u32*)pSMem;
      u32* pTmp2       = (u32*)(&(pSMem[sizeof(u32)]));
      u32  uWritePos1  = 0;
      u32  uWritePos2  = 0;
      u32  uBytesToRead= 0;

      while ( (uBytesToRead == 0) && (!g_bQuit) )
      {
         g_pSMProcessStats->lastActiveTime = get_current_timestamp_ms();
         uWritePos1 = *pTmp1;
         uWritePos2 = *pTmp2;
         if ( (uSharedMemReadPos == uWritePos1) || (uWritePos1 != uWritePos2) )
         {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 1000LL * (long long)10000; // 10 ms
            int iResSem = sem_timedwait(s_pSemaphoreSMData, &ts);
            if ( 0 != iResSem )
               continue;
            if ( 0 == sem_getvalue(s_pSemaphoreSMData, &iSemVal) && iSemVal > 0 )
               for ( int i = 0; i < iSemVal; i++ )
                  sem_trywait(s_pSemaphoreSMData);
            uWritePos1 = *pTmp1;
            uWritePos2 = *pTmp2;
            if ( (uSharedMemReadPos == uWritePos1) || (uWritePos1 != uWritePos2) )
               continue;
         }
         if ( uWritePos1 > uSharedMemReadPos )
         {
            uBytesToRead = uWritePos1 - uSharedMemReadPos;
            if ( uBytesToRead > PIPE_BUFFER_SIZE )
               uBytesToRead = PIPE_BUFFER_SIZE;
            memcpy(g_uPipeBuffer, &pSMem[uSharedMemReadPos], uBytesToRead);
            uSharedMemReadPos += uBytesToRead;
            if ( uSharedMemReadPos >= SM_STREAMER_SIZE )
               uSharedMemReadPos = 2 * sizeof(u32);
         }
         else
         {
            uBytesToRead = SM_STREAMER_SIZE - uSharedMemReadPos;
            if ( uBytesToRead > PIPE_BUFFER_SIZE )
               uBytesToRead = PIPE_BUFFER_SIZE;
            memcpy(g_uPipeBuffer, &pSMem[uSharedMemReadPos], uBytesToRead);
            uSharedMemReadPos += uBytesToRead;
            if ( uSharedMemReadPos >= SM_STREAMER_SIZE )
               uSharedMemReadPos = 2 * sizeof(u32);
            if ( (PIPE_BUFFER_SIZE - uBytesToRead > 0) && (uWritePos1 > 0) )
            {
               unsigned char* pTmpB      = g_uPipeBuffer + uBytesToRead;
               u32            uBytes2    = uWritePos1 - 2 * sizeof(u32);
               if ( uBytes2 + uBytesToRead > PIPE_BUFFER_SIZE )
                  uBytes2 = PIPE_BUFFER_SIZE - uBytesToRead;
               memcpy(pTmpB, &pSMem[uSharedMemReadPos], uBytes2);
               uSharedMemReadPos += uBytes2;
               uBytesToRead      += uBytes2;
            }
         }
      }

      if ( g_bQuit )
         break;

      g_pSMProcessStats->lastIPCIncomingTime = get_current_timestamp_ms();

      if ( !bAnyInputEver )
      {
         log_line("Start receiving video stream data through sharedmem (%d bytes)", nRead);
         bAnyInputEver = true;
         uTimeStartReceivingStream = get_current_timestamp_ms();
      }

      nRead = (int)uBytesToRead;
      iCount++;
      iTotalRead += nRead;
      if ( (iCount % 10) == 0 )
      {
         u32 uTime = get_current_timestamp_ms();
         if ( uTime >= uTimeLastCheck + 4000 )
         {
            uTimeLastCheck = uTime;
            log_line("Video player alive, reading %d kbits/sec", iTotalRead * 8 / 4 / 1000);
            iTotalRead = 0;
         }
      }

      int iRes = mpp_feed_data_to_decoder(g_uPipeBuffer, nRead);
      if ( iRes > 5 )
      {
         log_line("Stalled consuming %d bytes, stall for %d ms. Signaling alarm", nRead, iRes);
         if ( get_current_timestamp_ms() > uTimeStartReceivingStream + 5000 )
         {
            sem_t* ps = sem_open(SEMAPHORE_VIDEO_STREAMER_OVERLOAD, O_CREAT, S_IWUSR | S_IRUSR, 0);
            sem_post(ps);
            sem_close(ps);
         }
      }
   }

   if ( g_bQuit )
      log_line("Ending video stream play due to quit signal.");

   mpp_mark_end_of_stream();
   mpp_uninit();

   if ( pSMem )
   {
      munmap(pSMem, SM_STREAMER_SIZE);
      log_line("Unmapped shared mem: %s", SM_STREAMER_NAME);
   }
   if ( NULL != s_pSemaphoreSMData )
      sem_close(s_pSemaphoreSMData);
   s_pSemaphoreSMData = NULL;

   if ( g_bInitUILayerToo )
      _uninit_ui_layer();
   ruby_drm_core_uninit();
}


// ===========================================================================
// _do_stream_mode_udp  (play from UDP socket)
// ===========================================================================

void _do_stream_mode_udp()
{
   ControllerSettings* pCS = get_ControllerSettings();

   if ( hdmi_enum_modes() < 0 )
   {
      log_error_and_alarm("Failed to enumerate HDMI modes. Exit udp player.");
      return;
   }

   int iHDMIIndex = hdmi_load_current_mode();
   if ( iHDMIIndex < 0 )
      iHDMIIndex = hdmi_get_best_resolution_index_for(DEFAULT_RADXA_DISPLAY_WIDTH, DEFAULT_RADXA_DISPLAY_HEIGHT, DEFAULT_RADXA_DISPLAY_REFRESH);
   log_line("HDMI mode to use: %d (%d x %d @ %d)", iHDMIIndex,
      hdmi_get_current_resolution_width(), hdmi_get_current_resolution_height(),
      hdmi_get_current_resolution_refresh());

   ruby_drm_core_init(1, DRM_FORMAT_NV12,
      hdmi_get_current_resolution_width(),
      hdmi_get_current_resolution_height(),
      hdmi_get_current_resolution_refresh());

   if ( g_bInitUILayerToo )
      _init_ui_layer();

   if ( mpp_init(g_bUseH265Decoder, pCS->iVideoMPPBuffersSize) != 0 )
   {
      ruby_drm_core_uninit();
      if ( g_bInitUILayerToo )
         _uninit_ui_layer();
      return;
   }

   int iSock = socket(AF_INET, SOCK_DGRAM, 0);
   if ( iSock < 0 )
   {
      log_error_and_alarm("Failed to create socket");
      mpp_uninit();
      ruby_drm_core_uninit();
      if ( g_bInitUILayerToo )
         _uninit_ui_layer();
      return;
   }

   struct sockaddr_in udpAddr;
   memset(&udpAddr, 0, sizeof(udpAddr));
   udpAddr.sin_family      = AF_INET;
   udpAddr.sin_addr.s_addr = htonl(INADDR_ANY);
   udpAddr.sin_port        = htons(DEFAULT_LOCAL_VIDEO_PLAYER_UDP_PORT);

   if ( bind(iSock, (struct sockaddr*)&udpAddr, sizeof(udpAddr)) < 0 )
   {
      log_error_and_alarm("Failed to bind socket on port %d", DEFAULT_LOCAL_VIDEO_PLAYER_UDP_PORT);
      mpp_uninit();
      ruby_drm_core_uninit();
      if ( g_bInitUILayerToo )
         _uninit_ui_layer();
      return;
   }

   log_line("Opened input video stream udp socket on port %d", DEFAULT_LOCAL_VIDEO_PLAYER_UDP_PORT);
   mpp_enable_vsync(pCS->iHDMIVSync ? true : false);
   mpp_start_decoding_thread();

   u32  uTimeLastCheck = get_current_timestamp_ms();
   unsigned char uBuffer[2049];
   int  iCount         = 0;
   int  iTotalRead     = 0;
   bool bAnyInputEver  = false;

   while ( !g_bQuit )
   {
      iCount++;

      fd_set readset;
      FD_ZERO(&readset);
      FD_SET(iSock, &readset);

      struct timeval timeUDPInput;
      timeUDPInput.tv_sec  = 0;
      timeUDPInput.tv_usec = 10 * 1000; // 10 ms timeout

      int iSelectResult = select(iSock + 1, &readset, NULL, NULL, &timeUDPInput);
      if ( iSelectResult < 0 )
      {
         log_error_and_alarm("Failed to select socket.");
         continue;
      }
      if ( iSelectResult == 0 || !FD_ISSET(iSock, &readset) )
         continue;

      socklen_t          recvLen = 0;
      struct sockaddr_in addrClient;
      int iRecv = recvfrom(iSock, uBuffer, 2048, MSG_WAITALL,
                           (sockaddr*)&addrClient, &recvLen);
      if ( iRecv < 0 )
      {
         if ( !bAnyInputEver )
         {
            hardware_sleep_micros(2 * 1000);
            u32 uTime = get_current_timestamp_ms();
            if ( uTime > uTimeLastCheck + 3000 )
            {
               log_softerror_and_alarm("No video input data ever read and timed out on UDP port. Exit.");
               break;
            }
            continue;
         }
         log_line("Reached end of input stream data, failed to read UDP port. errno: %d, (%s)",
            errno, strerror(errno));
         break;
      }

      if ( !bAnyInputEver )
      {
         log_line("Start receiving video stream data through udp port (%d bytes)", iRecv);
         bAnyInputEver = true;
      }

      mpp_feed_data_to_decoder(uBuffer, iRecv);
      iTotalRead += iRecv;
      hardware_sleep_micros(2 * 1000);
      if ( (iCount % 10) == 0 )
      {
         u32 uTime = get_current_timestamp_ms();
         if ( uTime > uTimeLastCheck + 4000 )
         {
            uTimeLastCheck = uTime;
            log_line("Video player alive, reading %d bits/sec", iTotalRead * 8 / 4);
            iTotalRead = 0;
         }
      }
   }

   if ( g_bQuit )
      log_line("Ending video stream play due to quit signal.");

   close(iSock);
   mpp_mark_end_of_stream();
   mpp_uninit();

   if ( g_bInitUILayerToo )
      _uninit_ui_layer();
   ruby_drm_core_uninit();
}


// ===========================================================================
// Signal handler
// ===========================================================================

void handle_sigint(int sig)
{
   log_line("Caught signal to stop: %d", sig);
   g_bQuit = true;
   // Also ask GTK to quit so the GTK thread unblocks.
   gdk_threads_add_idle((GSourceFunc)gtk_main_quit, NULL);
}


// ===========================================================================
// main
// ===========================================================================

int main(int argc, char* argv[])
{
   if ( strcmp(argv[argc - 1], "-ver") == 0 )
   {
      printf("%d.%d (b%d)", SYSTEM_SW_VERSION_MAJOR, SYSTEM_SW_VERSION_MINOR / 10, SYSTEM_SW_BUILD_NUMBER);
      return 0;
   }

   signal(SIGINT,  handle_sigint);
   signal(SIGTERM, handle_sigint);
   signal(SIGQUIT, handle_sigint);

   log_init("RubyPlayer");

   if ( argc < 2 )
   {
      printf("\nUsage: ruby_player_gtk [params]\nParams:\n\n");
      printf("-p   Play the live video stream from pipe\n");
      printf("-u   Play the live video stream from UDP socket\n");
      printf("-sm  Play the live video stream from sharedmem\n");
      printf("-h265 use H265 decoder\n");
      printf("-f [filename] [fps] Play H264/H265 file\n");
      printf("-m [wxh@r] Sets a custom video mode\n");
      printf("-b   playing intro\n");
      printf("-i   init UI layer too when playing stream or files\n");
      printf("-d   debug output to stdout\n\n");
      return 0;
   }

   load_ControllerSettings();

   g_pSMProcessStats = shared_mem_process_stats_open_write(SHARED_MEM_WATCHDOG_MPP_PLAYER);
   if ( NULL == g_pSMProcessStats )
      log_softerror_and_alarm("Failed to open shared mem for process watchdog for writing: %s",
         SHARED_MEM_WATCHDOG_MPP_PLAYER);
   else
      log_line("Opened shared mem for process watchdog for writing (%s).", SHARED_MEM_WATCHDOG_MPP_PLAYER);

   g_szPlayFileName[0] = 0;
   int iParam = 0;

   do
   {
      if ( 0 == strcmp(argv[iParam], "-p") )
         g_bPlayStreamPipe = true;
      if ( 0 == strcmp(argv[iParam], "-u") )
         g_bPlayStreamUDP  = true;
      if ( 0 == strcmp(argv[iParam], "-sm") )
         g_bPlayStreamSM   = true;
      if ( 0 == strcmp(argv[iParam], "-i") )
         g_bInitUILayerToo = true;
      if ( 0 == strcmp(argv[iParam], "-b") )
         g_bPlayingIntro   = true;
      if ( 0 == strcmp(argv[iParam], "-h265") )
         g_bUseH265Decoder = true;
      if ( 0 == strcmp(argv[iParam], "-d") )
      {
         g_bDebug = true;
         log_enable_stdout();
      }
      if ( 0 == strcmp(argv[iParam], "-f") )
      {
         g_bPlayFile = true;
         iParam++;
         strncpy(g_szPlayFileName, argv[iParam], MAX_FILE_PATH_SIZE);
         if ( NULL != strstr(g_szPlayFileName, ".h265") )
            g_bUseH265Decoder = true;
         iParam++;
         if ( iParam < argc )
            g_iFileFPS = atoi(argv[iParam]);
         if ( (g_iFileFPS < 10) || (g_iFileFPS > 240) )
            g_iFileFPS = 30;
         iParam++;
         if ( iParam < argc )
         if ( 0 == strcmp(argv[iParam], "-endexit") )
            g_bExitOnEnd = true;
         continue;
      }
      if ( 0 == strcmp(argv[iParam], "-m") )
      {
         iParam++;
         char szTmp[256];
         strncpy(szTmp, argv[iParam], 255);
         szTmp[255] = 0;
         for ( int i = 0; i < (int)strlen(szTmp); i++ )
            if ( !isdigit(szTmp[i]) )
               szTmp[i] = ' ';
         if ( 3 != sscanf(szTmp, "%d %d %d", &g_iCustomWidth, &g_iCustomHeight, &g_iCustomRefresh) )
            g_iCustomWidth = g_iCustomHeight = g_iCustomRefresh = 0;
         continue;
      }
      iParam++;
   }
   while ( iParam < argc );

   if ( g_bPlayFile )
      log_line("Running mode: play file: [%s] [%d FPS] [exit on end: %s] [playing intro: %s]",
         g_szPlayFileName, g_iFileFPS, g_bExitOnEnd ? "yes" : "no", g_bPlayingIntro ? "yes" : "no");
   if ( g_bPlayStreamPipe )
      log_line("Running mode: stream from pipe");
   if ( g_bPlayStreamUDP )
      log_line("Running mode: stream from UDP");
   if ( g_bPlayStreamSM )
      log_line("Running mode: stream from sharedmem");
   if ( 0 != g_iCustomWidth )
      log_line("Set custom video mode: %dx%d@%d", g_iCustomWidth, g_iCustomHeight, g_iCustomRefresh);

   if ( (!g_bPlayFile) && (!g_bPlayStreamPipe) && (!g_bPlayStreamUDP) && (!g_bPlayStreamSM) )
   {
      log_softerror_and_alarm("Invalid params, no mode specified. Exit.");
      shared_mem_process_stats_close(SHARED_MEM_WATCHDOG_MPP_PLAYER, g_pSMProcessStats);
      return 0;
   }
   else if ( g_bPlayFile )
      _do_player_mode();
   else if ( g_bPlayStreamPipe )
      _do_stream_mode_pipe();
   else if ( g_bPlayStreamUDP )
      _do_stream_mode_udp();
   else if ( g_bPlayStreamSM )
      _do_stream_mode_sm();

   shared_mem_process_stats_close(SHARED_MEM_WATCHDOG_MPP_PLAYER, g_pSMProcessStats);
   return 0;
}