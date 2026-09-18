#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdbool.h>
#include <stdint.h>
#include <cairo/cairo.h>

#define PANEL_HEIGHT 30
#define START_BTN_WIDTH 80
#define ICON_SIZE 16
#define MAX_MENU_HEIGHT 600
#define MAX_APPS 2048
#define DESKTOP_ICON_SIZE 48
#define DESKTOP_CELL_WIDTH 96
#define DESKTOP_CELL_HEIGHT 104
#define MENU_RADIUS 8
#define SCROLL_STEP 15

/**
 * @brief Represents a desktop application.
 */
typedef struct {
    char name[128];
    char desktop_path[512];
    cairo_surface_t *icon;
} App;

/**
 * @brief Represents a category containing multiple desktop applications.
 */
typedef struct {
    char name[64];
    App *apps[MAX_APPS];
    int app_count;
} Category;

/**
 * @brief Represents an active toplevel window. The handle is used by the
 * Wayland backend and the window id by the X11 backend.
 */
typedef struct Toplevel {
    void *handle;
    unsigned long window;
    char title[256];
    char app_id[128];
    cairo_surface_t *icon;
    bool minimized;
    bool active;
    bool closed;
    struct Toplevel *next;
} Toplevel;

/**
 * @brief Identifies which of the two desktop surfaces an event or draw
 * operation refers to.
 */
typedef enum {
    SURFACE_NONE,
    SURFACE_PANEL,
    SURFACE_BG
} SurfaceId;

/**
 * @brief A rectangle in logical surface coordinates.
 */
typedef struct {
    int x;
    int y;
    int w;
    int h;
} Rect;

/**
 * @brief The display server operations the shared UI code depends on. Each
 * backend hands out a Cairo image surface to draw into, presents it on
 * commit, and feeds pointer and window list events back into the UI code.
 */
typedef struct {
    const char *name;
    bool (*init)(void);
    void (*shutdown)(void);
    int (*get_fd)(void);
    void (*before_poll)(void);
    void (*after_poll)(bool readable);
    cairo_surface_t *(*begin_draw)(SurfaceId id, int w, int h, int scale);
    void (*commit)(SurfaceId id);
    void (*update_panel_layer)(void);
    void (*toplevel_activate)(Toplevel *tl);
    void (*toplevel_set_minimized)(Toplevel *tl, bool minimized);
} Backend;

extern const Backend wayland_backend;
extern const Backend x11_backend;
extern const Backend *backend;

extern App apps[MAX_APPS];
extern int app_count;
extern App desktop_apps[MAX_APPS];
extern int desktop_app_count;
extern Category categories[20];
extern int category_count;
extern cairo_surface_t *start_icon;

extern Toplevel *toplevels_head;

extern int width;
extern int current_height;
extern int output_scale;
extern int bg_width;
extern int bg_height;
extern bool menu_open;
extern bool configured;
extern int hovered_category;
extern int hovered_app;

cairo_surface_t* get_icon(const char *name, int size);
void load_apps();
void load_desktop_apps();
void unload_apps();

void update_menu_heights();
void draw_frame();
void draw_bg();
void panel_configure(int w, int h);
void bg_configure(int w, int h);
int get_menu_rects(Rect *rects);
void get_desktop_cell(int index, int *x, int *y);

Toplevel* toplevel_create();
void toplevel_destroy(Toplevel *tl);
void toplevel_set_app_id(Toplevel *tl, const char *app_id);

void handle_pointer_button(SurfaceId id, double x, double y, uint32_t time);
void handle_pointer_motion(SurfaceId id, double x, double y);
void handle_pointer_axis(SurfaceId id, double x, double y, double amount);
void handle_pointer_leave(SurfaceId id);

#endif
