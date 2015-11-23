/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2015 - Daniel De Matteis
 *  Copyright (C) 2012-2014 - OV2
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef _XBOX
#include <xtl.h>
#include <xgraphics.h>
#endif

#include <formats/image.h>
#include <compat/strl.h>
#include <compat/posix_string.h>
#include <file/file_path.h>

#include "d3d.h"
#include "../video_common.h"
#include "../../dynamic.h"
#include "render_chain_driver.h"

#include "../common/win32_common.h"

#ifndef _XBOX
#define HAVE_MONITOR
#define HAVE_WINDOW
#endif

#include "../../performance.h"

#include "../../defines/d3d_defines.h"
#include "../../verbosity.h"

#if defined(HAVE_CG) || defined(HAVE_GLSL) || defined(HAVE_HLSL)

#if defined(HAVE_CG)
#define HAVE_SHADERS
#endif

#ifdef HAVE_HLSL
#include "../drivers_shader/shader_hlsl.h"
#endif
#endif

/* forward declarations */
static bool d3d_init_luts(d3d_video_t *d3d)
{
#ifndef _XBOX
   unsigned i;
#endif
   settings_t *settings = config_get_ptr();

   if (!d3d->renderchain_driver->add_lut)
      return true;

#ifndef _XBOX
   for (i = 0; i < d3d->shader.luts; i++)
   {
      bool ret = d3d->renderchain_driver->add_lut(
            d3d->renderchain_data,
			d3d->shader.lut[i].id, d3d->shader.lut[i].path,
         d3d->shader.lut[i].filter == RARCH_FILTER_UNSPEC ?
            settings->video.smooth :
            (d3d->shader.lut[i].filter == RARCH_FILTER_LINEAR));

      if (!ret)
         return ret;
   }
#endif

   return true;
}

static bool d3d_process_shader(d3d_video_t *d3d);
static bool d3d_init_chain(d3d_video_t *d3d,
      const video_info_t *video_info);

#ifdef HAVE_OVERLAY
static void d3d_free_overlays(d3d_video_t *d3d);
#endif
#ifdef HAVE_MENU
static void d3d_free_overlay(d3d_video_t *d3d, overlay_t *overlay);
#endif

static void d3d_deinit_chain(d3d_video_t *d3d)
{
   d3d->renderchain_driver->chain_free(d3d->renderchain_data);

   d3d->renderchain_driver = NULL;
   d3d->renderchain_data   = NULL;

#ifndef _XBOX
   d3d->needs_restore      = false;
#endif
}

static void d3d_deinitialize(d3d_video_t *d3d)
{
   driver_t *driver = driver_get_ptr();
   const font_renderer_t *font_ctx = NULL;
   if (!d3d)
      return;

   font_ctx = (const font_renderer_t*)driver->font_osd_driver;

   if (font_ctx->free)
      font_ctx->free(driver->font_osd_data);
   font_ctx = NULL;
   d3d_deinit_chain(d3d);
}

void d3d_make_d3dpp(void *data,
      const video_info_t *info, D3DPRESENT_PARAMETERS *d3dpp)
{
   d3d_video_t     *d3d = (d3d_video_t*)data;
   settings_t *settings = config_get_ptr();
   /* TODO/FIXME - get rid of global state dependencies. */
   global_t *global     = global_get_ptr();

   memset(d3dpp, 0, sizeof(*d3dpp));

   d3dpp->Windowed             = false;
#ifndef _XBOX
   d3dpp->Windowed             = settings->video.windowed_fullscreen || !info->fullscreen;
#endif
   d3dpp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

   if (info->vsync)
   {
      switch (settings->video.swap_interval)
      {
         default:
         case 1:
            d3dpp->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
            break;
         case 2:
            d3dpp->PresentationInterval = D3DPRESENT_INTERVAL_TWO;
            break;
         case 3:
            d3dpp->PresentationInterval = D3DPRESENT_INTERVAL_THREE;
            break;
         case 4:
            d3dpp->PresentationInterval = D3DPRESENT_INTERVAL_FOUR;
            break;
      }
   }

   d3dpp->SwapEffect = D3DSWAPEFFECT_DISCARD;
   d3dpp->BackBufferCount = 2;
#ifdef _XBOX
   d3dpp->BackBufferFormat =
#ifdef _XBOX360
      global->console.screen.gamma_correction ?
      (D3DFORMAT)MAKESRGBFMT(info->rgb32 ? D3DFMT_X8R8G8B8 : D3DFMT_LIN_R5G6B5) :
#endif
      info->rgb32 ? D3DFMT_X8R8G8B8 : D3DFMT_LIN_R5G6B5;
#else
   d3dpp->hDeviceWindow    = win32_get_window();
   d3dpp->BackBufferFormat = !d3dpp->Windowed ? D3DFMT_X8R8G8B8 : D3DFMT_UNKNOWN;
#endif

   if (!d3dpp->Windowed)
   {
#ifdef _XBOX
      unsigned width          = 0;
      unsigned height         = 0;

      gfx_ctx_get_video_size(d3d, &width, &height);
      video_driver_set_size(&width, &height);
#endif
      video_driver_get_size(&d3dpp->BackBufferWidth, &d3dpp->BackBufferHeight);
   }

#ifdef _XBOX
   d3dpp->MultiSampleType         = D3DMULTISAMPLE_NONE;
   d3dpp->EnableAutoDepthStencil  = FALSE;
#if defined(_XBOX1)
   /* Get the "video mode" */
   DWORD video_mode               = XGetVideoFlags();

   /* Check if we are able to use progressive mode. */
   if (video_mode & XC_VIDEO_FLAGS_HDTV_480p)
      d3dpp->Flags = D3DPRESENTFLAG_PROGRESSIVE;
   else
      d3dpp->Flags = D3DPRESENTFLAG_INTERLACED;

   /* Only valid in PAL mode, not valid for HDTV modes. */
   if (XGetVideoStandard() == XC_VIDEO_STANDARD_PAL_I)
   {
      if (video_mode & XC_VIDEO_FLAGS_PAL_60Hz)
         d3dpp->FullScreen_RefreshRateInHz = 60;
      else
         d3dpp->FullScreen_RefreshRateInHz = 50;
   }

   if (XGetAVPack() == XC_AV_PACK_HDTV)
   {
      if (video_mode & XC_VIDEO_FLAGS_HDTV_480p)
         d3dpp->Flags = D3DPRESENTFLAG_PROGRESSIVE;
      else if (video_mode & XC_VIDEO_FLAGS_HDTV_720p)
         d3dpp->Flags = D3DPRESENTFLAG_PROGRESSIVE;
      else if (video_mode & XC_VIDEO_FLAGS_HDTV_1080i)
         d3dpp->Flags = D3DPRESENTFLAG_INTERLACED;
   }

   if (widescreen_mode)
      d3dpp->Flags |= D3DPRESENTFLAG_WIDESCREEN;
#elif defined(_XBOX360)
   if (!widescreen_mode)
      d3dpp->Flags |= D3DPRESENTFLAG_NO_LETTERBOX;

   if (global->console.screen.gamma_correction)
      d3dpp->FrontBufferFormat       = (D3DFORMAT)MAKESRGBFMT(D3DFMT_LE_X8R8G8B8);
   else
      d3dpp->FrontBufferFormat       = D3DFMT_LE_X8R8G8B8;
   d3dpp->MultiSampleQuality      = 0;
#endif
#endif
}

static bool d3d_init_base(void *data, const video_info_t *info)
{
   D3DPRESENT_PARAMETERS d3dpp;
   d3d_video_t *d3d = (d3d_video_t*)data;

   d3d_make_d3dpp(d3d, info, &d3dpp);

   d3d->g_pD3D = D3DCREATE_CTX(D3D_SDK_VERSION);
   if (!d3d->g_pD3D)
   {
      RARCH_ERR("Failed to create D3D interface.\n");
      return false;
   }

#ifdef _XBOX360
   d3d->cur_mon_id = 0;
#endif

   if (FAILED(d3d->d3d_err = d3d->g_pD3D->CreateDevice(
            d3d->cur_mon_id,
            D3DDEVTYPE_HAL,
            win32_get_window(),
            D3DCREATE_HARDWARE_VERTEXPROCESSING,
            &d3dpp,
            &d3d->dev)))
   {
      RARCH_WARN("[D3D]: Failed to init device with hardware vertex processing (code: 0x%x). Trying to fall back to software vertex processing.\n",
                 (unsigned)d3d->d3d_err);

      if (FAILED(d3d->d3d_err = d3d->g_pD3D->CreateDevice(
                  d3d->cur_mon_id,
                  D3DDEVTYPE_HAL,
                  win32_get_window(),
                  D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                  &d3dpp,
                  &d3d->dev)))
      {
         RARCH_ERR("Failed to initialize device.\n");
         return false;
      }
   }

   return true;
}

static void d3d_set_viewport(void *data,
      unsigned width, unsigned height,
      bool force_full,
      bool allow_rotate)
{
   D3DVIEWPORT viewport;
   d3d_video_t *d3d = (d3d_video_t*)data;
   int x               = 0;
   int y               = 0;
   float device_aspect = (float)width / height;
   settings_t *settings = config_get_ptr();
   float desired_aspect = video_driver_get_aspect_ratio();

   video_driver_get_size(&width, &height);

   gfx_ctx_translate_aspect(d3d, &device_aspect, width, height);

   if (settings->video.scale_integer && !force_full)
   {
      struct video_viewport vp = {0};
      video_viewport_get_scaled_integer(&vp, width, height, desired_aspect, d3d->keep_aspect);
      x          = vp.x;
      y          = vp.y;
      width  = vp.width;
      height = vp.height;
   }
   else if (d3d->keep_aspect && !force_full)
   {
      if (settings->video.aspect_ratio_idx == ASPECT_RATIO_CUSTOM)
      {
         video_viewport_t *custom = video_viewport_get_custom();

         if (custom)
         {
            x          = custom->x;
            y          = custom->y;
            width      = custom->width;
            height     = custom->height;
         }
      }
      else
      {
         float delta;

         if (fabsf(device_aspect - desired_aspect) < 0.0001f) { }
         else if (device_aspect > desired_aspect)
         {
            delta       = (desired_aspect / device_aspect - 1.0f) / 2.0f + 0.5f;
            x           = int(roundf(width * (0.5f - delta)));
            y           = 0;
            width       = unsigned(roundf(2.0f * width * delta));
         }
         else
         {
            delta       = (device_aspect / desired_aspect - 1.0f) / 2.0f + 0.5f;
            x           = 0;
            y           = int(roundf(height * (0.5f - delta)));
            height      = unsigned(roundf(2.0f * height * delta));
         }
      }
   }


   /* D3D doesn't support negative X/Y viewports ... */
   if (x < 0)
      x = 0;
   if (y < 0)
      y = 0;

   viewport.X          = x;
   viewport.Y          = y;
   viewport.Width      = width;
   viewport.Height     = height;
   viewport.MinZ       = 0.0f;
   viewport.MaxZ       = 1.0f;

   d3d->final_viewport = viewport;

   if (d3d && d3d->renderchain_driver && d3d->renderchain_data)
   {
      if (d3d->renderchain_driver->set_font_rect)
         d3d->renderchain_driver->set_font_rect(d3d, NULL);
   }
}

static bool d3d_initialize(d3d_video_t *d3d, const video_info_t *info)
{
   unsigned width, height;
   bool ret             = true;
   settings_t *settings = config_get_ptr();
   driver_t   *driver   = driver_get_ptr();

   if (!d3d)
      return false;

   if (!d3d->g_pD3D)
      ret = d3d_init_base(d3d, info);
   else if (d3d->needs_restore)
   {
      D3DPRESENT_PARAMETERS d3dpp;

      d3d_make_d3dpp(d3d, info, &d3dpp);

      if (!d3d_reset(d3d->dev, &d3dpp))
      {
         d3d_deinitialize(d3d);
         d3d->g_pD3D->Release();
         d3d->g_pD3D = NULL;

         ret = d3d_init_base(d3d, info);
         if (ret)
            RARCH_LOG("[D3D]: Recovered from dead state.\n");
      }
   }

   if (!ret)
      return ret;

   if (!d3d_init_chain(d3d, info))
   {
      RARCH_ERR("Failed to initialize render chain.\n");
      return false;
   }

   video_driver_get_size(&width, &height);
   d3d_set_viewport(d3d,
	   width, height, false, true);

#if defined(_XBOX360)
   strlcpy(settings->video.font_path, "game:\\media\\Arial_12.xpr",
         sizeof(settings->video.font_path));
#endif
   if (!font_init_first((const void**)&driver->font_osd_driver, &driver->font_osd_data,
         d3d, settings->video.font_path, 0, FONT_DRIVER_RENDER_DIRECT3D_API))
   {
      RARCH_ERR("[D3D]: Failed to initialize font renderer.\n");
      return false;
   }

   return true;
}



bool d3d_restore(d3d_video_t *d3d)
{
   d3d_deinitialize(d3d);
   d3d->needs_restore = !d3d_initialize(d3d, &d3d->video_info);

   if (d3d->needs_restore)
      RARCH_ERR("[D3D]: Restore error.\n");

   return !d3d->needs_restore;
}


static void d3d_set_nonblock_state(void *data, bool state)
{
   d3d_video_t            *d3d = (d3d_video_t*)data;

   if (!d3d)
      return;

   d3d->video_info.vsync = !state;

   gfx_ctx_swap_interval(d3d, state ? 0 : 1);
}

static bool d3d_alive(void *data)
{
   unsigned temp_width = 0, temp_height = 0;
   bool ret = false;
   d3d_video_t *d3d   = (d3d_video_t*)data;
   bool        quit   = false;
   bool        resize = false;

   if (gfx_ctx_check_window(d3d, &quit, &resize,
            &temp_width, &temp_height))
   {
      if (quit)
         d3d->quitting = quit;
      else if (resize)
         d3d->should_resize = true;

      ret = !quit;
   }

   if (temp_width != 0 && temp_height != 0)
      video_driver_set_size(&temp_width, &temp_height);

   return ret;
}

static bool d3d_focus(void *data)
{
   return gfx_ctx_focus(data);
}

static bool d3d_suppress_screensaver(void *data, bool enable)
{
   return gfx_ctx_suppress_screensaver(data, enable);
}

static bool d3d_has_windowed(void *data)
{
   return gfx_ctx_has_windowed(data);
}

static void d3d_set_aspect_ratio(void *data, unsigned aspect_ratio_idx)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   enum rarch_display_ctl_state cmd = RARCH_DISPLAY_CTL_NONE;

   switch (aspect_ratio_idx)
   {
      case ASPECT_RATIO_SQUARE:
         cmd = RARCH_DISPLAY_CTL_SET_VIEWPORT_SQUARE_PIXEL;
         break;

      case ASPECT_RATIO_CORE:
         cmd = RARCH_DISPLAY_CTL_SET_VIEWPORT_CORE;
         break;

      case ASPECT_RATIO_CONFIG:
         cmd = RARCH_DISPLAY_CTL_SET_VIEWPORT_CONFIG;
         break;

      default:
         break;
   }

   if (cmd != RARCH_DISPLAY_CTL_NONE)
      video_driver_ctl(cmd, NULL);

   video_driver_set_aspect_ratio_value(aspectratio_lut[aspect_ratio_idx].value);

   if (!d3d)
      return;

   d3d->keep_aspect   = true;
   d3d->should_resize = true;
}

static void d3d_apply_state_changes(void *data)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   if (d3d)
      d3d->should_resize = true;
}

static void d3d_set_osd_msg(void *data, const char *msg,
      const struct font_params *params, void *font)
{
   d3d_video_t          *d3d = (d3d_video_t*)data;
   driver_t          *driver = driver_get_ptr();
   const font_renderer_t *font_ctx = driver->font_osd_driver;

   if (d3d->renderchain_driver->set_font_rect && params)
      d3d->renderchain_driver->set_font_rect(d3d, params);

   if (font_ctx->render_msg)
      font_ctx->render_msg(driver->font_osd_data, msg, params);
}

/* Delay constructor due to lack of exceptions. */

static bool d3d_construct(d3d_video_t *d3d,
      const video_info_t *info, const input_driver_t **input,
      void **input_data)
{
   unsigned full_x, full_y;
   driver_t    *driver         = driver_get_ptr();
   settings_t    *settings     = config_get_ptr();

   d3d->should_resize = false;

#if defined(HAVE_MENU)
   if (d3d->menu)
      free(d3d->menu);

   d3d->menu                = (overlay_t*)calloc(1, sizeof(overlay_t));

   if (!d3d->menu)
      return false;

   d3d->menu->tex_coords[0]  = 0;
   d3d->menu->tex_coords[1]  = 0;
   d3d->menu->tex_coords[2]  = 1;
   d3d->menu->tex_coords[3]  = 1;
   d3d->menu->vert_coords[0] = 0;
   d3d->menu->vert_coords[1] = 1;
   d3d->menu->vert_coords[2] = 1;
   d3d->menu->vert_coords[3] = -1;
#endif

   memset(&d3d->windowClass, 0, sizeof(d3d->windowClass));
#ifndef _XBOX
   win32_window_init(&d3d->windowClass, true, NULL);
#endif

#ifdef HAVE_MONITOR
   bool windowed_full;
   RECT mon_rect;
   MONITORINFOEX current_mon;
   HMONITOR hm_to_use;

   win32_monitor_info(&current_mon, &hm_to_use, &d3d->cur_mon_id);
   mon_rect = current_mon.rcMonitor;
   g_resize_width  = info->width;
   g_resize_height = info->height;

   windowed_full = settings->video.windowed_fullscreen;

   full_x = (windowed_full || info->width  == 0) ?
      (mon_rect.right  - mon_rect.left) : info->width;
   full_y = (windowed_full || info->height == 0) ?
      (mon_rect.bottom - mon_rect.top)  : info->height;
   RARCH_LOG("[D3D]: Monitor size: %dx%d.\n",
         (int)(mon_rect.right  - mon_rect.left),
         (int)(mon_rect.bottom - mon_rect.top));
#else
   gfx_ctx_get_video_size(d3d, &full_x, &full_y);
#endif
   {
      unsigned new_width  = info->fullscreen ? full_x : info->width;
      unsigned new_height = info->fullscreen ? full_y : info->height;
      video_driver_set_size(&new_width, &new_height);
   }

#ifndef _XBOX
#ifdef HAVE_WINDOW
   DWORD style;
   unsigned win_width, win_height;
   RECT rect = {0};
   /* Windows only reports the refresh rates for modelines as 
    * an integer, so video.refresh_rate needs to be rounded. Also, account 
    * for black frame insertion using video.refresh_rate set to half
    * of the display refresh rate, as well as higher vsync swap intervals. */
   float refresh_mod = settings->video.black_frame_insertion ? 2.0f : 1.0f;
   unsigned refresh     = roundf(settings->video.refresh_rate * refresh_mod * settings->video.swap_interval);

   video_driver_get_size(&win_width, &win_height);

   if (info->fullscreen)
   {
      if (windowed_full)
      {
         style = WS_EX_TOPMOST | WS_POPUP;
         g_resize_width  = win_width  = mon_rect.right - mon_rect.left;
         g_resize_height = win_height = mon_rect.bottom - mon_rect.top;
      }
      else
      {
         style = WS_POPUP | WS_VISIBLE;

         if (!win32_monitor_set_fullscreen(win_width, win_height,
                  refresh, current_mon.szDevice))
			 {}

         /* Display settings might have changed, get new coordinates. */
         GetMonitorInfo(hm_to_use, (MONITORINFO*)&current_mon);
         mon_rect = current_mon.rcMonitor;
      }
   }
   else
   {
      style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
      rect.right  = win_width;
      rect.bottom = win_height;
      AdjustWindowRect(&rect, style, FALSE);
      g_resize_width  = win_width   = rect.right - rect.left;
      g_resize_height = win_height  = rect.bottom - rect.top;
   }

   win32_window_create(d3d, style, &mon_rect, win_width,
         win_height, info->fullscreen);

   if (!info->fullscreen || windowed_full)
   {
      HWND window = win32_get_window();

      if (!info->fullscreen && settings->ui.menubar_enable)
      {
         RECT rc_temp = {0, 0, (LONG)win_height, 0x7FFF};

         SetMenu(window, LoadMenu(GetModuleHandle(NULL),MAKEINTRESOURCE(IDR_MENU)));
         SendMessage(window, WM_NCCALCSIZE, FALSE, (LPARAM)&rc_temp);
         g_resize_height = win_height += rc_temp.top + rect.top;
         SetWindowPos(window, NULL, 0, 0, win_width, win_height, SWP_NOMOVE);
      }

      ShowWindow(window, SW_RESTORE);
      UpdateWindow(window);
      SetForegroundWindow(window);
      SetFocus(window);
   }
#endif

   win32_show_cursor(!info->fullscreen);


#ifdef HAVE_SHADERS
   /* This should only be done once here
    * to avoid set_shader() to be overridden
    * later. */
   enum rarch_shader_type type =
      video_shader_parse_type(settings->video.shader_path, RARCH_SHADER_NONE);
   if (settings->video.shader_enable && type == RARCH_SHADER_CG)
      d3d->shader_path = settings->video.shader_path;

   if (!d3d_process_shader(d3d))
      return false;
#endif
#endif

   d3d->video_info = *info;
   if (!d3d_initialize(d3d, &d3d->video_info))
      return false;

   gfx_ctx_input_driver(d3d, input, input_data);

   RARCH_LOG("[D3D]: Init complete.\n");
   return true;
}

static void d3d_viewport_info(void *data, struct video_viewport *vp)
{
   d3d_video_t *d3d   = (d3d_video_t*)data;

   if (!d3d || !d3d->renderchain_driver || !d3d->renderchain_driver->viewport_info)
      return;

   d3d->renderchain_driver->viewport_info(d3d, vp);
}

static void d3d_set_rotation(void *data, unsigned rot)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   struct gfx_ortho ortho = {0, 1, 0, 1, -1, 1};

   if (!d3d)
      return;

   d3d->dev_rotation = rot;
}

static void d3d_show_mouse(void *data, bool state)
{
   gfx_ctx_show_mouse(data, state);
}

static const gfx_ctx_driver_t *d3d_get_context(void *data)
{
   /* Default to Direct3D9 for now.
   TODO: GL core contexts through ANGLE? */
   unsigned minor       = 0;
   settings_t *settings = config_get_ptr();
#if defined(HAVE_D3D8)
   unsigned major       = 8;
   enum gfx_ctx_api api = GFX_CTX_DIRECT3D8_API;
#else
   unsigned major       = 9;
   enum gfx_ctx_api api = GFX_CTX_DIRECT3D9_API;
#endif
   return gfx_ctx_init_first(video_driver_get_ptr(false),
         settings->video.context_driver,
         api, major, minor, false);
}

static void *d3d_init(const video_info_t *info,
      const input_driver_t **input, void **input_data)
{
   driver_t         *driver    = driver_get_ptr();
   d3d_video_t            *vid = NULL;
   const gfx_ctx_driver_t *ctx = NULL;

#ifdef _XBOX
   if (video_driver_get_ptr(false))
   {
      d3d_video_t *vid = (d3d_video_t*)video_driver_get_ptr(false);

      /* Reinitialize renderchain as we
       * might have changed pixel formats.*/
      if (vid->renderchain_driver->reinit(vid, (const void*)info))
      {
         d3d_deinit_chain(vid);
         d3d_init_chain(vid, info);

         if (input && input_data)
         {
            *input      = driver->input;
            *input_data = driver->input_data;
         }

         driver->video_data_own = true;
         driver->input_data_own = true;
         return vid;
      }
   }
#endif

   vid = new d3d_video_t();
   if (!vid)
      goto error;

   ctx = d3d_get_context(vid);
   if (!ctx)
      goto error;

   /* Default values */
   vid->g_pD3D               = NULL;
   vid->dev                  = NULL;
   vid->dev_rotation         = 0;
   vid->needs_restore        = false;
#ifdef HAVE_OVERLAY
   vid->overlays_enabled     = false;
#endif
#ifdef _XBOX
   vid->should_resize        = false;
#else
#ifdef HAVE_MENU
   vid->menu                 = NULL;
#endif
#endif

   gfx_ctx_set(ctx);

   if (!d3d_construct(vid, info, input, input_data))
   {
      RARCH_ERR("[D3D]: Failed to init D3D.\n");
      goto error;
   }

   vid->keep_aspect       = info->force_aspect;

#ifdef _XBOX
   driver->video_data_own = true;
   driver->input_data_own = true;
#endif

   return vid;

error:
   if (vid)
      delete vid;
   gfx_ctx_destroy(ctx);
   return NULL;
}

static void d3d_free(void *data)
{
   d3d_video_t   *d3d = (d3d_video_t*)data;
   HWND        window = win32_get_window();

   if (!d3d)
      return;

   d3d_deinitialize(d3d);
#ifdef HAVE_OVERLAY
   d3d_free_overlays(d3d);
#endif

#ifdef _XBOX
   gfx_ctx_free(d3d);
#else

#ifdef HAVE_MENU
   d3d_free_overlay(d3d, d3d->menu);
#endif

#endif
   if (d3d->dev)
      d3d->dev->Release();
   if (d3d->g_pD3D)
      d3d->g_pD3D->Release();

   win32_monitor_from_window(window, true);

   if (d3d)
      delete d3d;

   win32_destroy_window();
}

#ifndef DONT_HAVE_STATE_TRACKER
static bool d3d_init_imports(d3d_video_t *d3d)
{
   state_tracker_t *state_tracker = NULL;
   state_tracker_info tracker_info = {0};

   if (!d3d->shader.variables)
      return true;
   if (!d3d->renderchain_driver->add_state_tracker)
      return true;

   tracker_info.wram      = (uint8_t*)
      core.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
   tracker_info.info      = d3d->shader.variable;
   tracker_info.info_elem = d3d->shader.variables;

#ifdef HAVE_PYTHON
   if (*d3d->shader.script_path)
   {
      tracker_info.script = d3d->shader.script_path;
      tracker_info.script_is_file = true;
   }

   tracker_info.script_class =
      *d3d->shader.script_class ? d3d->shader.script_class : NULL;
#endif

   state_tracker = state_tracker_init(&tracker_info);
   if (!state_tracker)
   {
      RARCH_ERR("Failed to initialize state tracker.\n");
      return false;
   }

   d3d->renderchain_driver->add_state_tracker(d3d->renderchain_data, state_tracker);

   return true;
}
#endif

static bool d3d_init_chain(d3d_video_t *d3d, const video_info_t *video_info)
{
   unsigned current_width, current_height, out_width, out_height;
   unsigned i            = 0;
   LPDIRECT3DDEVICE d3dr = (LPDIRECT3DDEVICE)d3d->dev;
   LinkInfo link_info    = {0};

   (void)i;
   (void)current_width;
   (void)current_height;
   (void)out_width;
   (void)out_height;

   /* Setup information for first pass. */
#ifndef _XBOX
   link_info.pass  = &d3d->shader.pass[0];
#endif
   link_info.tex_w = link_info.tex_h =
      video_info->input_scale * RARCH_SCALE_BASE;

   if (!renderchain_init_first(&d3d->renderchain_driver,
	   &d3d->renderchain_data))
   {
	   RARCH_ERR("Renderchain could not be initialized.\n");
	   return false;
   }

   if (!d3d->renderchain_driver || !d3d->renderchain_data)
	   return false;

   RARCH_LOG("Renderchain driver: %s\n", d3d->renderchain_driver->ident);

   if (!d3d->renderchain_driver->init_shader(d3d, d3d->renderchain_data))
   {
      RARCH_ERR("Failed to initialize shader subsystem.\n");
      return false;
   }

   if (
         !d3d->renderchain_driver->init(
            d3d,
            &d3d->video_info,
            d3dr, &d3d->final_viewport, &link_info,
            d3d->video_info.rgb32)
      )
   {
      RARCH_ERR("[D3D]: Failed to init render chain.\n");
      return false;
   }

#ifndef _XBOX
   current_width  = link_info.tex_w;
   current_height = link_info.tex_h;
   out_width      = 0;
   out_height     = 0;

   for (i = 1; i < d3d->shader.passes; i++)
   {
      d3d->renderchain_driver->convert_geometry(d3d->renderchain_data,
		    &link_info,
            &out_width, &out_height,
            current_width, current_height, &d3d->final_viewport);

      link_info.pass  = &d3d->shader.pass[i];
      link_info.tex_w = next_pow2(out_width);
      link_info.tex_h = next_pow2(out_height);

      current_width = out_width;
      current_height = out_height;

      if (!d3d->renderchain_driver->add_pass(d3d->renderchain_data, &link_info))
      {
         RARCH_ERR("[D3D9]: Failed to add pass.\n");
         return false;
      }
   }

   if (!d3d_init_luts(d3d))
   {
      RARCH_ERR("[D3D9]: Failed to init LUTs.\n");
      return false;
   }

#ifndef DONT_HAVE_STATE_TRACKER
   if (!d3d_init_imports(d3d))
   {
      RARCH_ERR("[D3D9]: Failed to init imports.\n");
      return false;
   }
#endif

#endif

   return true;
}

#ifdef _XBOX

#ifdef _XBOX1
#include <formats/image.h>

static bool texture_image_render(d3d_video_t *d3d,
      struct texture_image *out_img,
      int x, int y, int w, int h, bool force_fullscreen)
{
   LPDIRECT3DTEXTURE d3dt;
   LPDIRECT3DVERTEXBUFFER d3dv;
   void *verts           = NULL;
   LPDIRECT3DDEVICE d3dr = (LPDIRECT3DDEVICE)d3d->dev;
   float fX              = (float)(x);
   float fY              = (float)(y);

   if (!d3d)
      return false;

   d3dt = (LPDIRECT3DTEXTURE)out_img->texture_buf;
   d3dv = (LPDIRECT3DVERTEXBUFFER)out_img->vertex_buf;

   if (!d3dt || !d3dv)
      return false;

   /* Create the new vertices. */
   Vertex newVerts[] =
   {
      // x,           y,              z,     color, u ,v
      {fX,            fY,             0.0f,  0,     0, 0},
      {fX + w,        fY,             0.0f,  0,     1, 0},
      {fX + w,        fY + h,         0.0f,  0,     1, 1},
      {fX,            fY + h,         0.0f,  0,     0, 1}
   };

   /* Load the existing vertices */
   verts = d3d_vertex_buffer_lock(d3dv);

   if (!verts)
      return false;

   /* Copy the new verts over the old verts */
   memcpy(verts, newVerts, sizeof(newVerts));
   d3d_vertex_buffer_unlock(d3dv);

   d3d_enable_blend_func(d3d->dev);
   d3d_enable_alpha_blend_texture_func(d3d->dev);

   /* Draw the quad. */
   d3d_set_texture(d3dr, 0, d3dt);
   d3d_set_stream_source(d3dr, 0,
         d3dv, 0, sizeof(Vertex));
   d3d_set_vertex_shader(d3dr, D3DFVF_CUSTOMVERTEX, NULL);

   if (force_fullscreen)
      d3d_set_viewport(d3d, w, h, force_fullscreen, false);
   d3d_draw_primitive(d3dr, D3DPT_QUADLIST, 0, 1);

   return true;
}
#endif

#endif

#ifdef HAVE_FBO
static bool d3d_init_multipass(d3d_video_t *d3d)
{
   unsigned i;
   bool use_extra_pass;
   video_shader_pass *pass = NULL;
   config_file_t *conf     = config_file_new(d3d->shader_path.c_str());

   if (!conf)
   {
      RARCH_ERR("Failed to load preset.\n");
      return false;
   }

   memset(&d3d->shader, 0, sizeof(d3d->shader));

   if (!video_shader_read_conf_cgp(conf, &d3d->shader))
   {
      config_file_free(conf);
      RARCH_ERR("Failed to parse CGP file.\n");
      return false;
   }

   config_file_free(conf);

   video_shader_resolve_relative(&d3d->shader, d3d->shader_path.c_str());

   RARCH_LOG("[D3D9 Meta-Cg] Found %u shaders.\n", d3d->shader.passes);

   for (i = 0; i < d3d->shader.passes; i++)
   {
      if (d3d->shader.pass[i].fbo.valid)
         continue;

      d3d->shader.pass[i].fbo.scale_y = 1.0f;
      d3d->shader.pass[i].fbo.scale_x = 1.0f;
      d3d->shader.pass[i].fbo.type_x  = RARCH_SCALE_INPUT;
      d3d->shader.pass[i].fbo.type_y  = RARCH_SCALE_INPUT;
   }

   use_extra_pass       = d3d->shader.passes < GFX_MAX_SHADERS &&
      d3d->shader.pass[d3d->shader.passes - 1].fbo.valid;

   if (use_extra_pass)
   {
      d3d->shader.passes++;
      pass              = (video_shader_pass*)
         &d3d->shader.pass[d3d->shader.passes - 1];

      pass->fbo.scale_x = pass->fbo.scale_y = 1.0f;
      pass->fbo.type_x  = pass->fbo.type_y = RARCH_SCALE_VIEWPORT;
      pass->filter      = RARCH_FILTER_UNSPEC;
   }
   else
   {
      pass              = (video_shader_pass*)
         &d3d->shader.pass[d3d->shader.passes - 1];

      pass->fbo.scale_x = pass->fbo.scale_y = 1.0f;
      pass->fbo.type_x  = pass->fbo.type_y = RARCH_SCALE_VIEWPORT;
   }

   return true;
}
#endif

static bool d3d_init_singlepass(d3d_video_t *d3d)
{
#ifndef _XBOX
   video_shader_pass *pass = NULL;

   if (!d3d)
      return false;

   memset(&d3d->shader, 0, sizeof(d3d->shader));
   d3d->shader.passes                    = 1;

   pass                                  = (video_shader_pass*)&d3d->shader.pass[0];

   pass->fbo.valid                       = true;
   pass->fbo.scale_y                     = 1.0;
   pass->fbo.type_y                      = RARCH_SCALE_VIEWPORT;
   pass->fbo.scale_x                     = pass->fbo.scale_y;
   pass->fbo.type_x                      = pass->fbo.type_y;
   strlcpy(pass->source.path, d3d->shader_path.c_str(),
         sizeof(pass->source.path));
#endif

   return true;
}

static bool d3d_process_shader(d3d_video_t *d3d)
{
#ifdef HAVE_FBO
   if (strcmp(path_get_extension(
               d3d->shader_path.c_str()), "cgp") == 0)
      return d3d_init_multipass(d3d);
#endif

   return d3d_init_singlepass(d3d);
}

#ifdef HAVE_MENU
static void d3d_overlay_render(d3d_video_t *d3d, overlay_t *overlay)
{
   struct video_viewport vp;
   unsigned width, height;
   void *verts;
   unsigned i;
   float vert[4][9];
   float overlay_width, overlay_height;
#ifndef _XBOX1
   LPDIRECT3DVERTEXDECLARATION vertex_decl;
   /* set vertex declaration for overlay. */
   D3DVERTEXELEMENT vElems[4] = {
      {0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_TEXCOORD, 0},
      {0, 20, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_COLOR, 0},
      D3DDECL_END()
   };
#endif

   if (!d3d)
      return;
   if (!overlay || !overlay->tex)
      return;

   if (!overlay->vert_buf)
   {
      overlay->vert_buf = d3d_vertex_buffer_new(
      d3d->dev, sizeof(vert), 0, 0, D3DPOOL_MANAGED, NULL);

	  if (!overlay->vert_buf)
		  return;
   }

   for (i = 0; i < 4; i++)
   {
      vert[i][2]   = 0.5f;
      vert[i][5]   = 1.0f;
      vert[i][6]   = 1.0f;
      vert[i][7]   = 1.0f;
      vert[i][8]   = overlay->alpha_mod;
   }
   
   d3d_viewport_info(d3d, &vp);

   overlay_width  = vp.width;
   overlay_height = vp.height;

   vert[0][0]      = overlay->vert_coords[0] * overlay_width;
   vert[1][0]      = (overlay->vert_coords[0] + overlay->vert_coords[2])
      * overlay_width;
   vert[2][0]      = overlay->vert_coords[0] * overlay_width;
   vert[3][0]      = (overlay->vert_coords[0] + overlay->vert_coords[2])
      * overlay_width;
   vert[0][1]      = overlay->vert_coords[1] * overlay_height;
   vert[1][1]      = overlay->vert_coords[1] * overlay_height;
   vert[2][1]      = (overlay->vert_coords[1] + overlay->vert_coords[3])
      * overlay_height;
   vert[3][1]      = (overlay->vert_coords[1] + overlay->vert_coords[3])
      * overlay_height;

   vert[0][3]      = overlay->tex_coords[0];
   vert[1][3]      = overlay->tex_coords[0] + overlay->tex_coords[2];
   vert[2][3]      = overlay->tex_coords[0];
   vert[3][3]      = overlay->tex_coords[0] + overlay->tex_coords[2];
   vert[0][4]      = overlay->tex_coords[1];
   vert[1][4]      = overlay->tex_coords[1];
   vert[2][4]      = overlay->tex_coords[1] + overlay->tex_coords[3];
   vert[3][4]      = overlay->tex_coords[1] + overlay->tex_coords[3];

   /* Align texels and vertices. */
   for (i = 0; i < 4; i++)
   {
      vert[i][0]  -= 0.5f;
      vert[i][1]  += 0.5f;
   }

   overlay->vert_buf->Lock(0, sizeof(vert), &verts, 0);
   memcpy(verts, vert, sizeof(vert));
   d3d_vertex_buffer_unlock(overlay->vert_buf);

   d3d_enable_blend_func(d3d->dev);

#ifndef _XBOX1
   d3d->dev->CreateVertexDeclaration(vElems, &vertex_decl);
   d3d_set_vertex_declaration(d3d->dev, vertex_decl);
   vertex_decl->Release();
#endif

   d3d_set_stream_source(d3d->dev, 0, overlay->vert_buf,
         0, sizeof(*vert));

   video_driver_get_size(&width, &height);

   if (overlay->fullscreen)
      d3d_set_viewport(d3d, width, height, true, false);

   /* Render overlay. */
   d3d_set_texture(d3d->dev, 0, overlay->tex);
   d3d_set_sampler_address_u(d3d->dev, 0, D3DTADDRESS_BORDER);
   d3d_set_sampler_address_v(d3d->dev, 0, D3DTADDRESS_BORDER);
   d3d_set_sampler_minfilter(d3d->dev, 0, D3DTEXF_LINEAR);
   d3d_set_sampler_magfilter(d3d->dev, 0, D3DTEXF_LINEAR);
   d3d_draw_primitive(d3d->dev, D3DPT_TRIANGLESTRIP, 0, 2);

   /* Restore previous state. */
   d3d_disable_blend_func(d3d->dev);
   d3d_set_viewport(d3d->dev, width, height, false, false);
}

static void d3d_free_overlay(d3d_video_t *d3d, overlay_t *overlay)
{
   if (!d3d)
      return;

   d3d_texture_free(overlay->tex);
   d3d_vertex_buffer_free(overlay->vert_buf, NULL);
}
#endif

#ifdef HAVE_OVERLAY
static void d3d_free_overlays(d3d_video_t *d3d)
{
   unsigned i;

   if (!d3d)
      return;

   for (i = 0; i < d3d->overlays.size(); i++)
      d3d_free_overlay(d3d, &d3d->overlays[i]);
   d3d->overlays.clear();
}

static void d3d_overlay_tex_geom(
      void *data,
      unsigned index,
      float x, float y,
      float w, float h)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   if (!d3d)
      return;

   d3d->overlays[index].tex_coords[0] = x;
   d3d->overlays[index].tex_coords[1] = y;
   d3d->overlays[index].tex_coords[2] = w;
   d3d->overlays[index].tex_coords[3] = h;
}

static void d3d_overlay_vertex_geom(
      void *data,
      unsigned index,
      float x, float y,
      float w, float h)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   if (!d3d)
      return;

   y                                   = 1.0f - y;
   h                                   = -h;
   d3d->overlays[index].vert_coords[0] = x;
   d3d->overlays[index].vert_coords[1] = y;
   d3d->overlays[index].vert_coords[2] = w;
   d3d->overlays[index].vert_coords[3] = h;
}

static bool d3d_overlay_load(void *data,
      const void *image_data, unsigned num_images)
{
   unsigned i, y;
   d3d_video_t *d3d = (d3d_video_t*)data;
   const struct texture_image *images = (const struct texture_image*)
      image_data;

   if (!d3d)
	   return false;

   d3d_free_overlays(d3d);
   d3d->overlays.resize(num_images);

   for (i = 0; i < num_images; i++)
   {
      D3DLOCKED_RECT d3dlr;
      unsigned width     = images[i].width;
      unsigned height    = images[i].height;
      overlay_t *overlay = (overlay_t*)&d3d->overlays[i];

      overlay->tex       = d3d_texture_new(d3d->dev, NULL,
                  width, height, 1,
                  0,
                  D3DFMT_A8R8G8B8,
                  D3DPOOL_MANAGED, 0, 0, 0,
                  NULL, NULL);

      if (!overlay->tex)
      {
         RARCH_ERR("[D3D]: Failed to create overlay texture\n");
         return false;
      }

      if (d3d_lock_rectangle(overlay->tex, 0, &d3dlr,
               NULL, 0, D3DLOCK_NOSYSLOCK))
      {
         uint32_t       *dst = (uint32_t*)(d3dlr.pBits);
         const uint32_t *src = images[i].pixels;
         unsigned      pitch = d3dlr.Pitch >> 2;

         for (y = 0; y < height; y++, dst += pitch, src += width)
            memcpy(dst, src, width << 2);
         d3d_unlock_rectangle(overlay->tex);
      }

      overlay->tex_w         = width;
      overlay->tex_h         = height;

      /* Default. Stretch to whole screen. */
      d3d_overlay_tex_geom(d3d, i, 0, 0, 1, 1);
      d3d_overlay_vertex_geom(d3d, i, 0, 0, 1, 1);
   }

   return true;
}

static void d3d_overlay_enable(void *data, bool state)
{
   unsigned i;
   d3d_video_t            *d3d = (d3d_video_t*)data;

   if (!d3d)
      return;

   for (i = 0; i < d3d->overlays.size(); i++)
      d3d->overlays_enabled = state;

   gfx_ctx_show_mouse(d3d, state);
}

static void d3d_overlay_full_screen(void *data, bool enable)
{
   unsigned i;
   d3d_video_t *d3d = (d3d_video_t*)data;

   for (i = 0; i < d3d->overlays.size(); i++)
      d3d->overlays[i].fullscreen = enable;
}

static void d3d_overlay_set_alpha(void *data, unsigned index, float mod)
{
   d3d_video_t *d3d = (d3d_video_t*)data;
   if (d3d)
      d3d->overlays[index].alpha_mod = mod;
}

static const video_overlay_interface_t d3d_overlay_interface = {
   d3d_overlay_enable,
   d3d_overlay_load,
   d3d_overlay_tex_geom,
   d3d_overlay_vertex_geom,
   d3d_overlay_full_screen,
   d3d_overlay_set_alpha,
};

static void d3d_get_overlay_interface(void *data,
      const video_overlay_interface_t **iface)
{
   (void)data;
   *iface = &d3d_overlay_interface;
}
#endif

static bool d3d_frame(void *data, const void *frame,
      unsigned frame_width, unsigned frame_height,
      uint64_t frame_count, unsigned pitch,
      const char *msg)
{
   unsigned width, height;
   static struct retro_perf_counter d3d_frame = {0};
   unsigned i                          = 0;
   d3d_video_t *d3d                    = (d3d_video_t*)data;
   LPDIRECT3DDEVICE d3dr               = (LPDIRECT3DDEVICE)d3d->dev;
   driver_t *driver                    = driver_get_ptr();
   settings_t *settings                = config_get_ptr();
   HWND window                         = win32_get_window();
   const font_renderer_t *font_ctx     = driver->font_osd_driver;

   (void)i;

   if (!frame)
      return true;

   video_driver_get_size(&width, &height);

   rarch_perf_init(&d3d_frame, "d3d_frame");
   retro_perf_start(&d3d_frame);

   /* We cannot recover in fullscreen. */
   if (d3d->needs_restore && IsIconic(window))
      return true;
   if (d3d->needs_restore && !d3d_restore(d3d))
   {
      RARCH_ERR("[D3D]: Failed to restore.\n");
      return false;
   }

   if (d3d->should_resize)
   {
      d3d_set_viewport(d3d, width, height, false, true);
      if (d3d->renderchain_driver->set_final_viewport)
         d3d->renderchain_driver->set_final_viewport(d3d,
               d3d->renderchain_data, &d3d->final_viewport);

      d3d->should_resize = false;
   }

   /* render_chain() only clears out viewport,
    * clear out everything. */
   D3DVIEWPORT screen_vp;
   screen_vp.X = 0;
   screen_vp.Y = 0;
   screen_vp.MinZ = 0;
   screen_vp.MaxZ = 1;
   screen_vp.Width = width;
   screen_vp.Height = height;
   d3d_set_viewport(d3dr, &screen_vp);
   d3d_clear(d3dr, 0, 0, D3DCLEAR_TARGET, 0, 1, 0);

   /* Insert black frame first, so we
    * can screenshot, etc. */
   if (settings->video.black_frame_insertion)
   {
      if (!d3d_swap(d3d, d3dr) || d3d->needs_restore)
         return true;
      d3d_clear(d3dr, 0, 0, D3DCLEAR_TARGET, 0, 1, 0);
   }

   if (
         !d3d->renderchain_driver->render(
            d3d,
            frame, frame_width, frame_height,
            pitch, d3d->dev_rotation))
   {
      RARCH_ERR("[D3D]: Failed to render scene.\n");
      return false;
   }

   if (font_ctx->render_msg && msg)
   {
      struct font_params font_parms = {0};
#ifdef _XBOX
#if defined(_XBOX1)
      float msg_width               = 60;
      float msg_height              = 365;
#elif defined(_XBOX360)
      float msg_width               = d3d->resolution_hd_enable ? 160 : 100;
      float msg_height              = 120;
#endif
      font_parms.x                  = msg_width;
      font_parms.y                  = msg_height;
      font_parms.scale              = 21;
#endif
      font_ctx->render_msg(driver->font_osd_data, msg, &font_parms);
   }

#ifdef HAVE_MENU
   if (d3d->menu && d3d->menu->enabled)
      d3d_overlay_render(d3d, d3d->menu);
#endif

#ifdef HAVE_OVERLAY
   if (d3d->overlays_enabled)
   {
      for (i = 0; i < d3d->overlays.size(); i++)
         d3d_overlay_render(d3d, &d3d->overlays[i]);
   }
#endif

#ifdef HAVE_MENU
   if (menu_driver_alive())
      menu_driver_frame();
#endif

   retro_perf_stop(&d3d_frame);

   gfx_ctx_update_window_title(d3d);

   gfx_ctx_swap_buffers(d3d);

   d3d->frame_count++;

   return true;
}

static bool d3d_read_viewport(void *data, uint8_t *buffer)
{
   d3d_video_t *d3d   = (d3d_video_t*)data;

   if (!d3d || !d3d->renderchain_driver || !d3d->renderchain_driver->read_viewport)
      return false;

   return d3d->renderchain_driver->read_viewport(d3d, buffer);
}

static bool d3d_set_shader(void *data,
      enum rarch_shader_type type, const char *path)
{
   bool restore_old   = false;
   d3d_video_t *d3d   = (d3d_video_t*)data;
   std::string shader = "";

   switch (type)
   {
      case RARCH_SHADER_CG:
         if (path)
            shader   = path;
#ifdef HAVE_HLSL
         d3d->shader = &hlsl_backend;
#endif
         break;
      default:
         break;
   }

   std::string old_shader = d3d->shader_path;
   d3d->shader_path       = shader;

   if (!d3d_process_shader(d3d) || !d3d_restore(d3d))
   {
      RARCH_ERR("[D3D]: Setting shader failed.\n");
      restore_old = true;
   }

   if (restore_old)
   {
      d3d->shader_path = old_shader;
      d3d_process_shader(d3d);
      d3d_restore(d3d);
   }

   return !restore_old;
}

#ifdef HAVE_MENU
static void d3d_set_menu_texture_frame(void *data,
      const void *frame, bool rgb32, unsigned width, unsigned height,
      float alpha)
{
   D3DLOCKED_RECT d3dlr;
   d3d_video_t *d3d = (d3d_video_t*)data;

   (void)d3dlr;
   (void)frame;
   (void)rgb32;
   (void)width;
   (void)height;
   (void)alpha;

   if (!d3d->menu->tex || d3d->menu->tex_w != width
         || d3d->menu->tex_h != height)
   {
      if (d3d->menu)
	     d3d_texture_free(d3d->menu->tex);

      d3d->menu->tex = d3d_texture_new(d3d->dev, NULL,
            width, height, 1,
            0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, 0, 0, 0, NULL, NULL);

      if (!d3d->menu->tex)
      {
         RARCH_ERR("[D3D]: Failed to create menu texture.\n");
         return;
      }

      d3d->menu->tex_w = width;
      d3d->menu->tex_h = height;
   }

   d3d->menu->alpha_mod = alpha;

   if (d3d_lock_rectangle(d3d->menu->tex, 0, &d3dlr,
            NULL, 0, D3DLOCK_NOSYSLOCK))
   {
      unsigned h, w;
      if (rgb32)
      {
         uint8_t        *dst = (uint8_t*)d3dlr.pBits;
         const uint32_t *src = (const uint32_t*)frame;

         for (h = 0; h < height; h++, dst += d3dlr.Pitch, src += width)
         {
            memcpy(dst, src, width * sizeof(uint32_t));
            memset(dst + width * sizeof(uint32_t), 0,
                  d3dlr.Pitch - width * sizeof(uint32_t));
         }
      }
      else
      {
         uint32_t       *dst = (uint32_t*)d3dlr.pBits;
         const uint16_t *src = (const uint16_t*)frame;

         for (h = 0; h < height; h++, dst += d3dlr.Pitch >> 2, src += width)
         {
            for (w = 0; w < width; w++)
            {
               uint16_t c = src[w];
               uint32_t r = (c >> 12) & 0xf;
               uint32_t g = (c >>  8) & 0xf;
               uint32_t b = (c >>  4) & 0xf;
               uint32_t a = (c >>  0) & 0xf;
               r          = ((r << 4) | r) << 16;
               g          = ((g << 4) | g) <<  8;
               b          = ((b << 4) | b) <<  0;
               a          = ((a << 4) | a) << 24;
               dst[w]     = r | g | b | a;
            }
         }
      }


      if (d3d->menu)
         d3d_unlock_rectangle(d3d->menu->tex);
   }
}

static void d3d_set_menu_texture_enable(void *data,
      bool state, bool full_screen)
{
   d3d_video_t *d3d = (d3d_video_t*)data;

   if (!d3d || !d3d->menu)
      return;

   d3d->menu->enabled            = state;
   d3d->menu->fullscreen         = full_screen;
}
#endif

static const video_poke_interface_t d3d_poke_interface = {
   NULL,
   NULL,
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_current_framebuffer */
   NULL, /* get_proc_address */
   d3d_set_aspect_ratio,
   d3d_apply_state_changes,
#ifdef HAVE_MENU
   d3d_set_menu_texture_frame,
   d3d_set_menu_texture_enable,
#else
   NULL,
   NULL,
#endif
   d3d_set_osd_msg,

   d3d_show_mouse,
};

static void d3d_get_poke_interface(void *data,
      const video_poke_interface_t **iface)
{
   (void)data;
   *iface = &d3d_poke_interface;
}

video_driver_t video_d3d = {
   d3d_init,
   d3d_frame,
   d3d_set_nonblock_state,
   d3d_alive,
   d3d_focus,
   d3d_suppress_screensaver,
   d3d_has_windowed,
   d3d_set_shader,
   d3d_free,
   "d3d",
   d3d_set_viewport,
   d3d_set_rotation,
   d3d_viewport_info,
   d3d_read_viewport,
   NULL, /* read_frame_raw */
#ifdef HAVE_OVERLAY
   d3d_get_overlay_interface,
#endif
   d3d_get_poke_interface
};
