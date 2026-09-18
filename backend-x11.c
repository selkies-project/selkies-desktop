#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/extensions/shape.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include "desktop.h"

#define MAX_SCALE 4
#define MAX_SHAPE_RECTS (1 + 2 * (2 * MENU_RADIUS * MAX_SCALE + 1))
#define MAX_ICON_PROPERTY_LENGTH (1L << 20)

/**
 * @brief An X11 window paired with the client side pixel buffer the shared UI
 * code renders into and the rectangles of it that are currently on screen.
 */
typedef struct {
    Window window;
    unsigned char *data;
    size_t size;
    int buffer_w;
    int buffer_h;
    cairo_surface_t *target;
    XRectangle visible[3];
    int visible_count;
} X11Surface;

static Display *dpy = NULL;
static int screen;
static Window root;
static int screen_w;
static int screen_h;
static Time last_event_time = CurrentTime;
static bool pointer_grabbed = false;

static X11Surface panel = {0};
static X11Surface desktop = {0};

static Atom atom_utf8_string;
static Atom atom_net_client_list;
static Atom atom_net_active_window;
static Atom atom_net_wm_name;
static Atom atom_net_wm_icon;
static Atom atom_net_wm_desktop;
static Atom atom_net_wm_state;
static Atom atom_net_wm_state_hidden;
static Atom atom_net_wm_state_sticky;
static Atom atom_net_wm_state_skip_taskbar;
static Atom atom_net_wm_state_skip_pager;
static Atom atom_net_wm_window_type;
static Atom atom_net_wm_window_type_dock;
static Atom atom_net_wm_window_type_desktop;
static Atom atom_net_wm_strut;
static Atom atom_net_wm_strut_partial;

/**
 * @brief Swallows X protocol errors. Other clients' windows can disappear
 * between being listed and being queried, which must not terminate the panel.
 */
static int ignore_x_error(Display *d, XErrorEvent *e) {
    return 0;
}

/**
 * @brief Fetches a window property.
 *
 * @param w The window to query.
 * @param prop The property atom.
 * @param type The expected property type, or AnyPropertyType.
 * @param max_len Maximum length to read in 32-bit units.
 * @param count Output number of items read.
 * @return void* Property data to release with XFree, or NULL when absent.
 */
static void* get_property(
    Window w, Atom prop, Atom type, long max_len, unsigned long *count) {
    Atom actual_type;
    int actual_format;
    unsigned long bytes_after;
    unsigned char *data = NULL;

    *count = 0;
    if (XGetWindowProperty(dpy, w, prop, 0, max_len, False, type,
                           &actual_type, &actual_format, count, &bytes_after,
                           &data) != Success) {
        return NULL;
    }
    if (!data || *count == 0) {
        if (data) XFree(data);
        *count = 0;
        return NULL;
    }
    return data;
}

/**
 * @brief Checks whether an atom list property on a window contains a value.
 */
static bool property_has_atom(Window w, Atom prop, Atom value) {
    unsigned long count;
    Atom *list = get_property(w, prop, XA_ATOM, 1024, &count);
    bool found = false;

    for (unsigned long i = 0; i < count; i++) {
        if (list[i] == value) {
            found = true;
            break;
        }
    }
    if (list) XFree(list);
    return found;
}

/**
 * @brief Derives the integer UI scale from the Xft.dpi resource, the value
 * toolkits follow for HiDPI on X11 where outputs carry no scale of their own.
 *
 * @return int The scale factor, 1 when no DPI is configured.
 */
static int read_scale() {
    int scale = 1;
    unsigned long count;
    char *resources = get_property(
        root, XA_RESOURCE_MANAGER, XA_STRING, 1L << 16, &count);
    if (!resources) {
        return scale;
    }

    XrmDatabase db = XrmGetStringDatabase(resources);
    if (db) {
        char *type = NULL;
        XrmValue value;
        if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value) &&
            value.addr) {
            scale = (int)((atof(value.addr) + 48.0) / 96.0);
        }
        XrmDestroyDatabase(db);
    }
    XFree(resources);

    if (scale < 1) scale = 1;
    if (scale > MAX_SCALE) scale = MAX_SCALE;
    return scale;
}

/**
 * @brief Finds the toplevel tracking an X11 window.
 */
static Toplevel* find_toplevel(Window w) {
    Toplevel *tl = toplevels_head;
    while (tl) {
        if (tl->window == w) {
            return tl;
        }
        tl = tl->next;
    }
    return NULL;
}

/**
 * @brief Refreshes a toplevel's title, preferring the UTF-8 EWMH name over
 * the legacy window name.
 */
static void update_title(Toplevel *tl) {
    unsigned long count;
    char *name = get_property(
        tl->window, atom_net_wm_name, atom_utf8_string, 1024, &count);
    if (!name) {
        name = get_property(
            tl->window, XA_WM_NAME, AnyPropertyType, 1024, &count);
    }

    tl->title[0] = '\0';
    if (name) {
        snprintf(tl->title, sizeof(tl->title), "%s", name);
        XFree(name);
    }
}

/**
 * @brief Builds a Cairo surface from the icon a window publishes through
 * _NET_WM_ICON. Picks the smallest image that still covers twice the panel
 * icon size, or the largest available, and premultiplies it for Cairo.
 *
 * @param w The window to read the icon from.
 * @return cairo_surface_t* The icon, or NULL when the window provides none.
 */
static cairo_surface_t* load_window_icon(Window w) {
    unsigned long count;
    unsigned long *data = get_property(
        w, atom_net_wm_icon, XA_CARDINAL, MAX_ICON_PROPERTY_LENGTH, &count);
    if (!data) {
        return NULL;
    }

    unsigned long target = ICON_SIZE * 2;
    unsigned long best_pos = 0;
    unsigned long best_w = 0;
    unsigned long best_h = 0;
    unsigned long pos = 0;

    while (pos + 2 <= count) {
        unsigned long w_px = data[pos];
        unsigned long h_px = data[pos + 1];
        if (w_px == 0 || h_px == 0 || w_px > 1024 || h_px > 1024 ||
            pos + 2 + w_px * h_px > count) {
            break;
        }

        bool better;
        if (best_w == 0) {
            better = true;
        } else if (best_w >= target) {
            better = w_px >= target && w_px < best_w;
        } else {
            better = w_px > best_w;
        }
        if (better) {
            best_pos = pos + 2;
            best_w = w_px;
            best_h = h_px;
        }
        pos += 2 + w_px * h_px;
    }

    if (best_w == 0) {
        XFree(data);
        return NULL;
    }

    cairo_surface_t *surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, best_w, best_h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        XFree(data);
        return NULL;
    }

    unsigned char *pixels = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);

    for (unsigned long y = 0; y < best_h; y++) {
        uint32_t *row = (uint32_t*)(pixels + y * stride);
        for (unsigned long x = 0; x < best_w; x++) {
            uint32_t argb = (uint32_t)data[best_pos + y * best_w + x];
            uint32_t a = argb >> 24;
            uint32_t r = ((argb >> 16) & 0xff) * a / 255;
            uint32_t g = ((argb >> 8) & 0xff) * a / 255;
            uint32_t b = (argb & 0xff) * a / 255;
            row[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    cairo_surface_mark_dirty(surf);
    XFree(data);
    return surf;
}

/**
 * @brief Refreshes a toplevel's application ID and icon. Looks up a themed
 * icon by window class, then by instance name, and finally falls back to the
 * icon the window itself provides.
 */
static void update_app_id(Toplevel *tl) {
    XClassHint hint = {0};

    if (XGetClassHint(dpy, tl->window, &hint)) {
        toplevel_set_app_id(tl, hint.res_class ? hint.res_class : "");
        if (!tl->icon && hint.res_name) {
            tl->icon = get_icon(hint.res_name, ICON_SIZE);
        }
        if (hint.res_class) XFree(hint.res_class);
        if (hint.res_name) XFree(hint.res_name);
    }

    if (!tl->icon) {
        tl->icon = load_window_icon(tl->window);
    }
}

/**
 * @brief Refreshes a toplevel's minimized flag from its EWMH state.
 */
static void update_state(Toplevel *tl) {
    tl->minimized = property_has_atom(
        tl->window, atom_net_wm_state, atom_net_wm_state_hidden);
}

/**
 * @brief Marks the toplevel matching the window manager's active window.
 */
static void sync_active() {
    unsigned long count;
    Window active = None;
    Window *data = get_property(
        root, atom_net_active_window, XA_WINDOW, 1, &count);
    if (data) {
        active = data[0];
        XFree(data);
    }

    Toplevel *tl = toplevels_head;
    while (tl) {
        tl->active = (tl->window == active);
        tl = tl->next;
    }
}

/**
 * @brief Decides whether a managed window belongs on the taskbar. Docks,
 * desktops and windows that opt out through their EWMH state are left off.
 */
static bool wants_taskbar_button(Window w) {
    if (property_has_atom(
            w, atom_net_wm_window_type, atom_net_wm_window_type_dock) ||
        property_has_atom(
            w, atom_net_wm_window_type, atom_net_wm_window_type_desktop)) {
        return false;
    }
    return !property_has_atom(
        w, atom_net_wm_state, atom_net_wm_state_skip_taskbar);
}

/**
 * @brief Reconciles the taskbar with the window manager's client list. Drops
 * toplevels whose window is gone or no longer eligible, adds newly eligible
 * windows, and subscribes to property changes on every client so later title,
 * class and state updates are seen.
 */
static void sync_clients() {
    unsigned long count;
    Window *list = get_property(
        root, atom_net_client_list, XA_WINDOW, 4096, &count);
    bool *eligible = calloc(count + 1, sizeof(bool));
    if (!eligible) {
        if (list) XFree(list);
        return;
    }

    for (unsigned long i = 0; i < count; i++) {
        if (list[i] == panel.window || list[i] == desktop.window) {
            continue;
        }
        XSelectInput(dpy, list[i], PropertyChangeMask);
        eligible[i] = wants_taskbar_button(list[i]);
    }

    Toplevel *tl = toplevels_head;
    while (tl) {
        Toplevel *next = tl->next;
        bool found = false;
        for (unsigned long i = 0; i < count; i++) {
            if (eligible[i] && list[i] == tl->window) {
                found = true;
                break;
            }
        }
        if (!found) {
            toplevel_destroy(tl);
        }
        tl = next;
    }

    for (unsigned long i = 0; i < count; i++) {
        if (!eligible[i] || find_toplevel(list[i])) {
            continue;
        }
        Toplevel *added = toplevel_create();
        if (!added) {
            break;
        }
        added->window = list[i];
        update_title(added);
        update_app_id(added);
        update_state(added);
    }

    free(eligible);
    if (list) XFree(list);

    sync_active();
    if (configured) {
        draw_frame();
    }
}

/**
 * @brief Sends an EWMH client message about a window to the window manager.
 */
static void send_wm_message(Window w, Atom type, long d0, long d1, long d2) {
    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = type;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = d0;
    ev.xclient.data.l[1] = d1;
    ev.xclient.data.l[2] = d2;
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

/**
 * @brief Asks the window manager to focus and raise a window, identifying the
 * request as coming from a pager so it is not treated as focus stealing.
 */
static void x11_toplevel_activate(Toplevel *tl) {
    send_wm_message(tl->window, atom_net_active_window, 2, last_event_time, 0);
}

/**
 * @brief Iconifies a window. Restoring needs no request of its own since the
 * window manager deiconifies a window when it is activated.
 */
static void x11_toplevel_set_minimized(Toplevel *tl, bool minimized) {
    if (minimized) {
        XIconifyWindow(dpy, tl->window, screen);
    }
}

/**
 * @brief Dock windows already stack above regular windows, so opening the
 * menu needs no layer change on X11.
 */
static void x11_update_panel_layer() {}

/**
 * @brief Provides a Cairo surface over the client side pixel buffer of the
 * requested desktop surface, growing the buffer when the size changes.
 */
static cairo_surface_t* x11_begin_draw(SurfaceId id, int w, int h, int scale) {
    X11Surface *s = (id == SURFACE_BG) ? &desktop : &panel;
    int buffer_w = w * scale;
    int buffer_h = h * scale;
    size_t size = (size_t)buffer_w * buffer_h * 4;

    if (size == 0) {
        return NULL;
    }

    if (s->size != size) {
        unsigned char *data = realloc(s->data, size);
        if (!data) {
            return NULL;
        }
        s->data = data;
        s->size = size;
    }
    s->buffer_w = buffer_w;
    s->buffer_h = buffer_h;

    return cairo_image_surface_create_for_data(
        s->data, CAIRO_FORMAT_ARGB32, buffer_w, buffer_h, buffer_w * 4);
}

/**
 * @brief Copies the visible rectangles of a surface's pixel buffer to its
 * window. Also used to repaint after expose events without re-rendering.
 */
static void blit(X11Surface *s) {
    if (!s->data || !s->target || s->visible_count == 0) {
        return;
    }

    cairo_surface_t *src = cairo_image_surface_create_for_data(
        s->data, CAIRO_FORMAT_ARGB32, s->buffer_w, s->buffer_h,
        s->buffer_w * 4);
    cairo_t *cr = cairo_create(s->target);

    for (int i = 0; i < s->visible_count; i++) {
        cairo_rectangle(cr, s->visible[i].x, s->visible[i].y,
                        s->visible[i].width, s->visible[i].height);
    }
    cairo_clip(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_paint(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(src);
    cairo_surface_flush(s->target);
}

/**
 * @brief Appends the rectangles approximating a rounded rectangle to a shape
 * list. Each corner row is inset to follow the circle drawn by the UI code,
 * since X11 shapes are one bit and cannot carry the antialiased edge.
 *
 * @param rects The rectangle list to append to.
 * @param n The current list length, updated on return.
 * @param r The rectangle in physical pixels.
 * @param radius The corner radius in physical pixels.
 */
static void add_rounded_shape(XRectangle *rects, int *n, Rect r, int radius) {
    if (r.w <= 0 || r.h <= 0) {
        return;
    }
    if (radius * 2 > r.h) radius = r.h / 2;
    if (radius * 2 > r.w) radius = r.w / 2;

    for (int i = 0; i < radius; i++) {
        double dy = radius - i - 0.5;
        int inset = (int)ceil(radius - sqrt(radius * radius - dy * dy));

        rects[*n].x = r.x + inset;
        rects[*n].y = r.y + i;
        rects[*n].width = r.w - 2 * inset;
        rects[*n].height = 1;
        (*n)++;

        rects[*n].x = r.x + inset;
        rects[*n].y = r.y + r.h - 1 - i;
        rects[*n].width = r.w - 2 * inset;
        rects[*n].height = 1;
        (*n)++;
    }

    rects[*n].x = r.x;
    rects[*n].y = r.y + radius;
    rects[*n].width = r.w;
    rects[*n].height = r.h - 2 * radius;
    (*n)++;
}

/**
 * @brief Holds the pointer while the menu is open so a click anywhere on the
 * screen reaches the panel and can dismiss the menu, then releases it again.
 * A failed grab is retried on the next redraw.
 */
static void sync_pointer_grab() {
    if (menu_open && !pointer_grabbed) {
        int result = XGrabPointer(
            dpy, panel.window, False,
            ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
            GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        pointer_grabbed = (result == GrabSuccess);
    } else if (!menu_open && pointer_grabbed) {
        XUngrabPointer(dpy, CurrentTime);
        pointer_grabbed = false;
    }
}

/**
 * @brief Restricts the background window's input to its icon cells, letting
 * every other click fall through to the root window and the window manager's
 * root menu.
 */
static void apply_desktop_input_shape() {
    int s = output_scale;
    XRectangle *cells = calloc(desktop_app_count + 1, sizeof(XRectangle));
    if (!cells) {
        return;
    }

    for (int i = 0; i < desktop_app_count; i++) {
        int cell_x, cell_y;
        get_desktop_cell(i, &cell_x, &cell_y);
        cells[i].x = cell_x * s;
        cells[i].y = cell_y * s;
        cells[i].width = DESKTOP_CELL_WIDTH * s;
        cells[i].height = DESKTOP_CELL_HEIGHT * s;
    }
    XShapeCombineRectangles(
        dpy, desktop.window, ShapeInput, 0, 0, cells, desktop_app_count,
        ShapeSet, Unsorted);
    free(cells);
}

/**
 * @brief Cuts the panel window down to the bar plus the open menu and records
 * those areas as the visible rectangles to repaint. Without a compositing
 * manager a window cannot be transparent, so the shape extension stands in
 * for the transparent regions of the Wayland surface.
 */
static void apply_panel_shape() {
    int s = output_scale;
    XRectangle shape[MAX_SHAPE_RECTS];
    int shape_count = 0;
    Rect menu_rects[2];
    int menu_rect_count = get_menu_rects(menu_rects);

    shape[shape_count].x = 0;
    shape[shape_count].y = (current_height - PANEL_HEIGHT) * s;
    shape[shape_count].width = width * s;
    shape[shape_count].height = PANEL_HEIGHT * s;
    shape_count++;

    panel.visible[0] = shape[0];
    panel.visible_count = 1;

    for (int i = 0; i < menu_rect_count; i++) {
        Rect r = {
            menu_rects[i].x * s, menu_rects[i].y * s,
            menu_rects[i].w * s, menu_rects[i].h * s
        };
        if (r.y < 0) {
            r.h += r.y;
            r.y = 0;
        }
        if (r.h <= 0) {
            continue;
        }
        add_rounded_shape(shape, &shape_count, r, MENU_RADIUS * s);

        panel.visible[panel.visible_count].x = r.x;
        panel.visible[panel.visible_count].y = r.y;
        panel.visible[panel.visible_count].width = r.w;
        panel.visible[panel.visible_count].height = r.h;
        panel.visible_count++;
    }

    XShapeCombineRectangles(
        dpy, panel.window, ShapeBounding, 0, 0, shape, shape_count,
        ShapeSet, Unsorted);
}

/**
 * @brief Presents a rendered buffer after updating the window's shape to
 * match what was drawn.
 */
static void x11_commit(SurfaceId id) {
    if (id == SURFACE_BG) {
        apply_desktop_input_shape();

        desktop.visible[0].x = 0;
        desktop.visible[0].y = 0;
        desktop.visible[0].width = desktop.buffer_w;
        desktop.visible[0].height = desktop.buffer_h;
        desktop.visible_count = 1;
        blit(&desktop);
        XFlush(dpy);
        return;
    }

    apply_panel_shape();
    sync_pointer_grab();
    blit(&panel);
    XFlush(dpy);
}

/**
 * @brief Reserves the bar's strip at the bottom of the screen so the window
 * manager keeps maximized windows off it.
 */
static void update_struts() {
    long bar = PANEL_HEIGHT * output_scale;
    long strut[12] = {0};
    strut[3] = bar;
    strut[10] = 0;
    strut[11] = screen_w - 1;

    XChangeProperty(dpy, panel.window, atom_net_wm_strut, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char*)strut, 4);
    XChangeProperty(dpy, panel.window, atom_net_wm_strut_partial, XA_CARDINAL,
                    32, PropModeReplace, (unsigned char*)strut, 12);
}

/**
 * @brief Pins a window's position and size through its normal hints so the
 * window manager places it exactly where requested.
 */
static void set_fixed_geometry(Window w, int x, int y, int win_w, int win_h) {
    XSizeHints hints = {0};
    hints.flags = USPosition | PPosition | USSize | PSize |
                  PMinSize | PMaxSize;
    hints.x = x;
    hints.y = y;
    hints.width = hints.min_width = hints.max_width = win_w;
    hints.height = hints.min_height = hints.max_height = win_h;
    XSetWMNormalHints(dpy, w, &hints);
    XMoveResizeWindow(dpy, w, x, y, win_w, win_h);
}

/**
 * @brief Lays both windows out for the current screen size and scale, then
 * lets the shared UI code render them. The panel window only spans the bar
 * plus the tallest possible menu rather than the full screen, which keeps its
 * pixel buffer small. Runs at startup and whenever the screen is resized.
 */
static void apply_geometry() {
    int s = output_scale;
    int logical_w = (screen_w + s - 1) / s;
    int logical_h = (screen_h + s - 1) / s;
    int panel_h = PANEL_HEIGHT + MAX_MENU_HEIGHT + 20;
    if (panel_h > logical_h) panel_h = logical_h;

    set_fixed_geometry(
        panel.window, 0, screen_h - panel_h * s, screen_w, panel_h * s);
    set_fixed_geometry(desktop.window, 0, 0, screen_w, screen_h);
    update_struts();

    cairo_xlib_surface_set_size(panel.target, screen_w, panel_h * s);
    cairo_xlib_surface_set_size(desktop.target, screen_w, screen_h);

    bg_configure(logical_w, logical_h);
    panel_configure(logical_w, panel_h);
}

/**
 * @brief Creates one of the desktop's windows and tags it with the EWMH type
 * and states that make the window manager treat it as part of the desktop
 * shell. The window never takes keyboard focus, so clicking the taskbar does
 * not change which window is active before the click is handled.
 *
 * @param s The surface to create the window for.
 * @param title The window title.
 * @param type The _NET_WM_WINDOW_TYPE atom to assign.
 * @param event_mask The events to select on the window.
 */
static void create_window(
    X11Surface *s, const char *title, Atom type, long event_mask) {
    XSetWindowAttributes attrs = {0};
    attrs.background_pixel = BlackPixel(dpy, screen);
    attrs.event_mask = event_mask;

    s->window = XCreateWindow(
        dpy, root, 0, 0, 1, 1, 0, CopyFromParent, InputOutput, CopyFromParent,
        CWBackPixel | CWEventMask, &attrs);

    XStoreName(dpy, s->window, title);

    XClassHint class_hint = { "selkies-desktop", "Selkies-desktop" };
    XSetClassHint(dpy, s->window, &class_hint);

    XWMHints wm_hints = {0};
    wm_hints.flags = InputHint;
    wm_hints.input = False;
    XSetWMHints(dpy, s->window, &wm_hints);

    XChangeProperty(dpy, s->window, atom_net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char*)&type, 1);

    Atom states[3] = {
        atom_net_wm_state_sticky,
        atom_net_wm_state_skip_taskbar,
        atom_net_wm_state_skip_pager
    };
    XChangeProperty(dpy, s->window, atom_net_wm_state, XA_ATOM, 32,
                    PropModeReplace, (unsigned char*)states, 3);

    long all_desktops = 0xFFFFFFFF;
    XChangeProperty(dpy, s->window, atom_net_wm_desktop, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char*)&all_desktops, 1);

    s->target = cairo_xlib_surface_create(
        dpy, s->window, DefaultVisual(dpy, screen), 1, 1);
}

/**
 * @brief Translates a pointer event into the shared UI code's logical
 * coordinates. Wheel buttons become scroll steps and other presses become
 * clicks. While the menu holds the pointer grab every event is reported
 * against the panel window, including clicks outside of it.
 */
static void handle_button(XButtonEvent *ev) {
    SurfaceId id = (ev->window == desktop.window) ? SURFACE_BG : SURFACE_PANEL;
    double x = ev->x / (double)output_scale;
    double y = ev->y / (double)output_scale;

    last_event_time = ev->time;

    if (ev->button == Button4) {
        handle_pointer_axis(id, x, y, -SCROLL_STEP);
    } else if (ev->button == Button5) {
        handle_pointer_axis(id, x, y, SCROLL_STEP);
    } else if (ev->button <= Button3) {
        handle_pointer_button(id, x, y, (uint32_t)ev->time);
    }
}

/**
 * @brief Reacts to property changes on the root window and on client windows
 * to keep the taskbar and the UI scale current.
 */
static void handle_property(XPropertyEvent *ev) {
    if (ev->window == root) {
        if (ev->atom == atom_net_client_list) {
            sync_clients();
        } else if (ev->atom == atom_net_active_window) {
            sync_active();
            if (configured) {
                draw_frame();
            }
        } else if (ev->atom == XA_RESOURCE_MANAGER) {
            int scale = read_scale();
            if (scale != output_scale) {
                output_scale = scale;
                apply_geometry();
            }
        }
        return;
    }

    if (ev->atom == atom_net_wm_state || ev->atom == atom_net_wm_window_type) {
        Toplevel *tl = find_toplevel(ev->window);
        if (tl) {
            update_state(tl);
        }
        sync_clients();
        return;
    }

    Toplevel *tl = find_toplevel(ev->window);
    if (!tl) {
        return;
    }

    if (ev->atom == atom_net_wm_name || ev->atom == XA_WM_NAME) {
        update_title(tl);
    } else if (ev->atom == XA_WM_CLASS || ev->atom == atom_net_wm_icon) {
        update_app_id(tl);
    } else {
        return;
    }

    if (configured) {
        draw_frame();
    }
}

/**
 * @brief Dispatches a single X event. Shapes are applied again whenever the
 * window manager reparents one of the desktop's windows, because openbox only
 * learns about a client's input shape from the change notification and never
 * queries it when it starts managing a window. Without this a shape set before
 * the window manager adopted the window is ignored, and its frame swallows the
 * clicks meant to fall through to the root window.
 */
static void handle_event(XEvent *ev) {
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.count == 0) {
            blit(ev->xexpose.window == desktop.window ? &desktop : &panel);
        }
        break;
    case ButtonPress:
        handle_button(&ev->xbutton);
        break;
    case MotionNotify:
        handle_pointer_motion(
            ev->xmotion.window == desktop.window ? SURFACE_BG : SURFACE_PANEL,
            ev->xmotion.x / (double)output_scale,
            ev->xmotion.y / (double)output_scale);
        break;
    case LeaveNotify:
        if (ev->xcrossing.mode == NotifyNormal) {
            handle_pointer_leave(
                ev->xcrossing.window == desktop.window ?
                SURFACE_BG : SURFACE_PANEL);
        }
        break;
    case ReparentNotify:
        if (configured) {
            apply_desktop_input_shape();
            apply_panel_shape();
        }
        break;
    case ConfigureNotify:
        if (ev->xconfigure.window == root &&
            (ev->xconfigure.width != screen_w ||
             ev->xconfigure.height != screen_h)) {
            screen_w = ev->xconfigure.width;
            screen_h = ev->xconfigure.height;
            apply_geometry();
        }
        break;
    case PropertyNotify:
        handle_property(&ev->xproperty);
        break;
    }
}

/**
 * @brief Handles every event Xlib has queued or can read without blocking.
 * Requests made by handlers can pull further events into the queue, so this
 * only returns once the queue is truly empty and polling is safe.
 */
static void drain_events() {
    while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        handle_event(&ev);
    }
}

/**
 * @brief Connects to the X server and sets up the dock and desktop windows
 * along with the window list tracking on the root window.
 */
static bool x11_init() {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        return false;
    }

    int shape_event, shape_error;
    if (!XShapeQueryExtension(dpy, &shape_event, &shape_error)) {
        XCloseDisplay(dpy);
        dpy = NULL;
        return false;
    }

    XSetErrorHandler(ignore_x_error);
    XrmInitialize();

    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    screen_w = DisplayWidth(dpy, screen);
    screen_h = DisplayHeight(dpy, screen);

    atom_utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    atom_net_client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    atom_net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    atom_net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    atom_net_wm_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
    atom_net_wm_desktop = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    atom_net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    atom_net_wm_state_hidden = XInternAtom(
        dpy, "_NET_WM_STATE_HIDDEN", False);
    atom_net_wm_state_sticky = XInternAtom(
        dpy, "_NET_WM_STATE_STICKY", False);
    atom_net_wm_state_skip_taskbar = XInternAtom(
        dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    atom_net_wm_state_skip_pager = XInternAtom(
        dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    atom_net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    atom_net_wm_window_type_dock = XInternAtom(
        dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    atom_net_wm_window_type_desktop = XInternAtom(
        dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    atom_net_wm_strut = XInternAtom(dpy, "_NET_WM_STRUT", False);
    atom_net_wm_strut_partial = XInternAtom(
        dpy, "_NET_WM_STRUT_PARTIAL", False);

    output_scale = read_scale();

    XSelectInput(dpy, root, PropertyChangeMask | StructureNotifyMask);

    create_window(
        &desktop, "selkies-desktop wallpaper", atom_net_wm_window_type_desktop,
        ExposureMask | ButtonPressMask | LeaveWindowMask |
        StructureNotifyMask);
    create_window(
        &panel, "selkies-desktop panel", atom_net_wm_window_type_dock,
        ExposureMask | ButtonPressMask | PointerMotionMask | LeaveWindowMask |
        StructureNotifyMask);

    apply_geometry();

    XMapWindow(dpy, desktop.window);
    XMapWindow(dpy, panel.window);

    sync_clients();
    XFlush(dpy);
    return true;
}

/**
 * @brief Releases the pointer grab and disconnects from the X server.
 */
static void x11_shutdown() {
    if (pointer_grabbed) {
        XUngrabPointer(dpy, CurrentTime);
    }
    XCloseDisplay(dpy);
}

/**
 * @brief Returns the display connection descriptor for the main poll loop.
 */
static int x11_get_fd() {
    return ConnectionNumber(dpy);
}

/**
 * @brief Handles everything already queued so the main loop can safely block.
 */
static void x11_before_poll() {
    drain_events();
}

/**
 * @brief Handles newly arrived events after the poll wakes up.
 */
static void x11_after_poll(bool readable) {
    if (readable) {
        drain_events();
    }
}

const Backend x11_backend = {
    .name = "x11",
    .init = x11_init,
    .shutdown = x11_shutdown,
    .get_fd = x11_get_fd,
    .before_poll = x11_before_poll,
    .after_poll = x11_after_poll,
    .begin_draw = x11_begin_draw,
    .commit = x11_commit,
    .update_panel_layer = x11_update_panel_layer,
    .toplevel_activate = x11_toplevel_activate,
    .toplevel_set_minimized = x11_toplevel_set_minimized
};
