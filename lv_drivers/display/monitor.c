/**
 * @file monitor.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "monitor.h"
#if USE_MONITOR

#ifndef MONITOR_SDL_INCLUDE_PATH
#  define MONITOR_SDL_INCLUDE_PATH <SDL2/SDL.h>
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include MONITOR_SDL_INCLUDE_PATH
#include "../indev/mouse.h"
#include "../indev/keyboard.h"
#include "../indev/mousewheel.h"

/*********************
 *      DEFINES
 *********************/
#define SDL_REFR_PERIOD     50  /*ms*/

#ifndef MONITOR_ZOOM
#define MONITOR_ZOOM        1
#endif

#if defined(__APPLE__) && defined(TARGET_OS_MAC)
#  if __APPLE__ && TARGET_OS_MAC
#define MONITOR_APPLE
#  endif
#endif

#if defined(__EMSCRIPTEN__)
#  define MONITOR_EMSCRIPTEN
#endif

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    SDL_Window * window;
    SDL_Renderer * renderer;
    SDL_Texture * texture;
    volatile bool sdl_refr_qry;
#if MONITOR_DOUBLE_BUFFERED
    uint32_t * tft_fb_act;
#else
    uint32_t tft_fb[LV_HOR_RES_MAX * LV_VER_RES_MAX];
#endif
}monitor_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static int monitor_sdl_refr_thread(void * param);
static void window_create(monitor_t * m);
static void window_update(monitor_t * m);

/***********************
 *   GLOBAL PROTOTYPES
 ***********************/

/**********************
 *  STATIC VARIABLES
 **********************/
monitor_t monitor;

#if MONITOR_DUAL
monitor_t monitor2;
#endif

static volatile bool sdl_inited = false;
static volatile bool sdl_quit_qry = false;

int quit_filter(void * userdata, SDL_Event * event);
static void monitor_sdl_clean_up(void);
static void monitor_sdl_init(void);
#ifdef MONITOR_EMSCRIPTEN
void monitor_sdl_refr_core(void); /* called from Emscripten loop */
#else
static void monitor_sdl_refr_core(void);
#endif

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Initialize the monitor
 */
void monitor_init(void)
{
    /*OSX needs to initialize SDL here*/
#if defined(MONITOR_APPLE) || defined(MONITOR_EMSCRIPTEN)
    monitor_sdl_init();
#endif

#ifndef MONITOR_EMSCRIPTEN
    SDL_CreateThread(monitor_sdl_refr_thread, "sdl_refr", NULL);
    while(sdl_inited == false); /*Wait until 'sdl_refr' initializes the SDL*/
#endif
}

/**
 * Flush a buffer to the display. Calls 'lv_flush_ready()' when finished
 * @param x1 left coordinate
 * @param y1 top coordinate
 * @param x2 right coordinate
 * @param y2 bottom coordinate
 * @param color_p array of colors to be flushed
 */
void monitor_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    lv_coord_t hres = disp_drv->rotated == 0 ? disp_drv->hor_res : disp_drv->ver_res;
    lv_coord_t vres = disp_drv->rotated == 0 ? disp_drv->ver_res : disp_drv->hor_res;

//    printf("x1:%d,y1:%d,x2:%d,y2:%d\n", area->x1, area->y1, area->x2, area->y2);

    /*Return if the area is out the screen*/
    if(area->x2 < 0 || area->y2 < 0 || area->x1 > hres - 1 || area->y1 > vres - 1) {

        lv_disp_flush_ready(disp_drv);
        return;
    }

#if MONITOR_DOUBLE_BUFFERED
    monitor.tft_fb_act = (uint32_t *)color_p;

    monitor.sdl_refr_qry = true;

    /*IMPORTANT! It must be called to tell the system the flush is ready*/
    lv_disp_flush_ready(disp_drv);
#else

    int32_t y;
#if LV_COLOR_DEPTH != 24 && LV_COLOR_DEPTH != 32    /*32 is valid but support 24 for backward compatibility too*/
    int32_t x;
    for(y = area->y1; y <= area->y2; y++) {
        for(x = area->x1; x <= area->x2; x++) {
            monitor.tft_fb[y * MONITOR_HOR_RES + x] = lv_color_to32(*color_p);
            color_p++;
        }

    }
#else
    uint32_t w = lv_area_get_width(area);
    for(y = area->y1; y <= area->y2 && y < MONITOR_VER_RES; y++) {
        memcpy(&monitor.tft_fb[y * MONITOR_HOR_RES + area->x1], color_p, w * sizeof(lv_color_t));
        color_p += w;
    }
#endif

    monitor.sdl_refr_qry = true;

    /*IMPORTANT! It must be called to tell the system the flush is ready*/
    lv_disp_flush_ready(disp_drv);
#endif
}


#if MONITOR_DUAL
/**
 * Flush a buffer to the display. Calls 'lv_flush_ready()' when finished
 * @param x1 left coordinate
 * @param y1 top coordinate
 * @param x2 right coordinate
 * @param y2 bottom coordinate
 * @param color_p array of colors to be flushed
 */
void monitor_flush2(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    lv_coord_t hres = disp_drv->rotated == 0 ? disp_drv->hor_res : disp_drv->ver_res;
    lv_coord_t vres = disp_drv->rotated == 0 ? disp_drv->ver_res : disp_drv->hor_res;

    /*Return if the area is out the screen*/
    if(area->x2 < 0 || area->y2 < 0 || area->x1 > hres - 1 || area->y1 > vres - 1) {
        lv_disp_flush_ready(disp_drv);
        return;
    }

#if MONITOR_DOUBLE_BUFFERED
    monitor2.tft_fb_act = (uint32_t *)color_p;

    monitor2.sdl_refr_qry = true;

    /*IMPORTANT! It must be called to tell the system the flush is ready*/
    lv_disp_flush_ready(disp_drv);
#else

    int32_t y;
#if LV_COLOR_DEPTH != 24 && LV_COLOR_DEPTH != 32    /*32 is valid but support 24 for backward compatibility too*/
    int32_t x;
    for(y = area->y1; y <= area->y2; y++) {
        for(x = area->x1; x <= area->x2; x++) {
            monitor2.tft_fb[y * MONITOR_HOR_RES + x] = lv_color_to32(*color_p);
            color_p++;
        }

    }
#else
    uint32_t w = lv_area_get_width(area);
    for(y = area->y1; y <= area->y2 && y < MONITOR_VER_RES; y++) {
        memcpy(&monitor2.tft_fb[y * MONITOR_HOR_RES + area->x1], color_p, w * sizeof(lv_color_t));
        color_p += w;
    }
#endif

    monitor2.sdl_refr_qry = true;

    /*IMPORTANT! It must be called to tell the system the flush is ready*/
    lv_disp_flush_ready(disp_drv);
#endif
}
#endif

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * SDL main thread. All SDL related task have to be handled here!
 * It initializes SDL, handles drawing and the mouse.
 */

static int monitor_sdl_refr_thread(void * param)
{
    (void)param;

    /*If not OSX initialize SDL in the Thread*/
#ifndef MONITOR_APPLE
    monitor_sdl_init();
#endif
    /*Run until quit event not arrives*/
    while(sdl_quit_qry == false) {
        /*Refresh handling*/
        monitor_sdl_refr_core();
    }

    monitor_sdl_clean_up();
    exit(0);

    return 0;
}

int quit_filter(void * userdata, SDL_Event * event)
{
    (void)userdata;

    if(event->type == SDL_WINDOWEVENT) {
        if(event->window.event == SDL_WINDOWEVENT_CLOSE) {
            sdl_quit_qry = true;
        }
    }
    else if(event->type == SDL_QUIT) {
        sdl_quit_qry = true;
    }



    return 1;
}

static void monitor_sdl_clean_up(void)
{
    SDL_DestroyTexture(monitor.texture);
    SDL_DestroyRenderer(monitor.renderer);
    SDL_DestroyWindow(monitor.window);

#if MONITOR_DUAL
    SDL_DestroyTexture(monitor2.texture);
    SDL_DestroyRenderer(monitor2.renderer);
    SDL_DestroyWindow(monitor2.window);

#endif

    SDL_Quit();
}

static void monitor_sdl_init(void)
{
    /*Initialize the SDL*/
    SDL_Init(SDL_INIT_VIDEO);

    SDL_SetEventFilter(quit_filter, NULL);

    window_create(&monitor);
#if MONITOR_DUAL
    window_create(&monitor2);
    int x, y;
    SDL_GetWindowPosition(monitor2.window, &x, &y);
    SDL_SetWindowPosition(monitor.window, x + MONITOR_HOR_RES / 2 + 10, y);
    SDL_SetWindowPosition(monitor2.window, x - MONITOR_HOR_RES / 2 - 10, y);
#endif

    sdl_inited = true;
}

#ifdef MONITOR_EMSCRIPTEN
void monitor_sdl_refr_core(void)
#else
static void monitor_sdl_refr_core(void)
#endif
{

    if(monitor.sdl_refr_qry != false) {
        monitor.sdl_refr_qry = false;
        window_update(&monitor);
    }

#if MONITOR_DUAL
    if(monitor2.sdl_refr_qry != false) {
        monitor2.sdl_refr_qry = false;
        window_update(&monitor2);
    }
#endif

#if !defined(MONITOR_APPLE) && !defined(MONITOR_EMSCRIPTEN)
    SDL_Event event;
    while(SDL_PollEvent(&event)) {
#if USE_MOUSE != 0
        mouse_handler(&event);
#endif

#if USE_MOUSEWHEEL != 0
        mousewheel_handler(&event);
#endif

#if USE_KEYBOARD
        keyboard_handler(&event);
#endif
        if((&event)->type == SDL_WINDOWEVENT) {
            switch((&event)->window.event) {
#if SDL_VERSION_ATLEAST(2, 0, 5)
                case SDL_WINDOWEVENT_TAKE_FOCUS:
#endif
                case SDL_WINDOWEVENT_EXPOSED:
                    window_update(&monitor);
#if MONITOR_DUAL
                    window_update(&monitor2);
#endif
                    break;
                default:
                    break;
            }
        }
    }
#endif /*MONITOR_APPLE*/

    /*Sleep some time*/
    SDL_Delay(SDL_REFR_PERIOD);

}

static void window_create(monitor_t * m)
{
    m->window = SDL_CreateWindow("Lvgl Designer",
                              SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              MONITOR_HOR_RES * MONITOR_ZOOM, MONITOR_VER_RES * MONITOR_ZOOM, 0);       /*last param. SDL_WINDOW_BORDERLESS to hide borders*/

#if MONITOR_VIRTUAL_MACHINE || defined(MONITOR_EMSCRIPTEN)
    m->renderer = SDL_CreateRenderer(m->window, -1, SDL_RENDERER_SOFTWARE);
#else
    m->renderer = SDL_CreateRenderer(m->window, -1, 0);
#endif
    m->texture = SDL_CreateTexture(m->renderer,
                                SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, MONITOR_HOR_RES, MONITOR_VER_RES);
    SDL_SetTextureBlendMode(m->texture, SDL_BLENDMODE_BLEND);

    /*Initialize the frame buffer to gray (77 is an empirical value) */
#if MONITOR_DOUBLE_BUFFERED
    SDL_UpdateTexture(m->texture, NULL, m->tft_fb_act, MONITOR_HOR_RES * sizeof(uint32_t));
#else
    memset(m->tft_fb, 0x44, MONITOR_HOR_RES * MONITOR_VER_RES * sizeof(uint32_t));
#endif

    m->sdl_refr_qry = true;

}

static void window_update(monitor_t * m)
{
#if MONITOR_DOUBLE_BUFFERED == 0
    SDL_UpdateTexture(m->texture, NULL, m->tft_fb, MONITOR_HOR_RES * sizeof(uint32_t));
#else
    if(m->tft_fb_act == NULL) return;
    SDL_UpdateTexture(m->texture, NULL, m->tft_fb_act, MONITOR_HOR_RES * sizeof(uint32_t));
#endif
    SDL_RenderClear(m->renderer);
    /*Test: Draw a background to test transparent screens (LV_COLOR_SCREEN_TRANSP)*/
    //        SDL_SetRenderDrawColor(renderer, 0xff, 0, 0, 0xff);
    //        SDL_Rect r;
    //        r.x = 0; r.y = 0; r.w = MONITOR_HOR_RES; r.w = MONITOR_VER_RES;
    //        SDL_RenderDrawRect(renderer, &r);

    /*Update the renderer with the texture containing the rendered image*/
    SDL_RenderCopy(m->renderer, m->texture, NULL, NULL);
    SDL_RenderPresent(m->renderer);
}

#endif /*USE_MONITOR*/
