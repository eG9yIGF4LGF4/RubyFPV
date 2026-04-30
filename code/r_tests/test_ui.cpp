#include "../base/shared_mem.h"
#include "../base/hardware.h"
#include "../base/hw_procs.h"

#include <time.h>
#include <sys/resource.h>

#include "../renderer/render_engine.h"
#include "../renderer/drm_core.h"
#include "../base/hdmi.h"

bool bQuit = false;

void handle_sigint(int sig) 
{ 
   log_line("Caught signal to stop: %d\n", sig);
   bQuit = true;
}   

int main(int argc, char *argv[])
{
   signal(SIGINT, handle_sigint);
   signal(SIGTERM, handle_sigint);
   signal(SIGQUIT, handle_sigint);

   log_init("TestUI");
   // log_enable_stdout();
   log_line("\nStarted.\n");

   ruby_drm_core_wait_for_display_connected(); 
   if ( hdmi_enum_modes() < 0 )
   {
      log_error_and_alarm("Failed to enumerate HDMI modes. Exit player.");
      return -1;
   }
   int iHDMIIndex = hdmi_load_current_mode();
   if ( iHDMIIndex < 0 )
      iHDMIIndex = hdmi_get_best_resolution_index_for(DEFAULT_RADXA_DISPLAY_WIDTH, DEFAULT_RADXA_DISPLAY_HEIGHT, DEFAULT_RADXA_DISPLAY_REFRESH);

   int _w = hdmi_get_current_resolution_width();
   int _h = hdmi_get_current_resolution_height();
   int _r = hdmi_get_current_resolution_refresh();
   //ruby_drm_core_init(0, DRM_FORMAT_ARGB8888, _w, _h, _r);
   ruby_drm_core_init(1, DRM_FORMAT_NV12, 1920, 1080, 120);

   ruby_drm_core_set_plane_properties_and_buffer(ruby_drm_core_get_main_draw_buffer_id());

   log_line("HDMI mode to use: %d (%d x %d @ %d)", iHDMIIndex, _w, _h, _r );

   RenderEngine* g_pRenderEngine = render_init_engine();

   u32 idFontMenu = g_pRenderEngine->loadRawFont(0, "res/noto.ttf", 0);
   int x = 0;
   int y = 0;
   int w = _w;
   uint8_t* col = new uint8_t[]{255, 0, 0, 1};
   while (! bQuit )
   {
      hardware_sleep_ms(50);
      g_pRenderEngine->startFrame();
      g_pRenderEngine->setFontColor(idFontMenu, (double*)col);
      g_pRenderEngine->drawText(20, 20,  idFontMenu, "Test UI: Hello, World!");
      g_pRenderEngine->setFill(100,100,100,1);
      g_pRenderEngine->setStrokeSize(0);
      g_pRenderEngine->drawRect((float)x/w, 0.1, 0.1, 0.1);
      g_pRenderEngine->endFrame();

      x++;
      if ( x > 500 ) x = 0;
   }
   log_line("\nEnded\n");
   exit(0);
}
