#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cairo/cairo.h>
#include "desktop.h"
#include "wlr-layer-shell.h"
#include "wlr-foreign-toplevel-management-unstable-v1.h"

/**
 * @brief Manages a persistent shared memory buffer for Wayland rendering.
 */
typedef struct {
    int fd;
    uint32_t *data;
    struct wl_buffer *buffer;
    int size;
    int width;
    int height;
    int scale;
} PersistentBuffer;

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_surface *bg_surface;
static struct zwlr_layer_surface_v1 *bg_layer_surface;
static struct wl_seat *default_seat = NULL;
static struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager = NULL;
static uint32_t layer_shell_version = 1;

static struct wl_cursor_theme *cursor_theme = NULL;
static struct wl_cursor *default_cursor = NULL;
static struct wl_surface *cursor_surface = NULL;

static struct wl_surface *current_pointer_surface = NULL;

static PersistentBuffer ui_buffer = {0};
static PersistentBuffer bg_buffer = {0};

/**
 * @brief Allocates an anonymous shared memory file descriptor.
 *
 * @param size Desired size of the shared memory region in bytes.
 * @return int The file descriptor, or -1 on failure.
 */
static int create_shm_file(off_t size) {
    int fd = memfd_create("panel-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    ftruncate(fd, size);
    return fd;
}

/**
 * @brief Ensures a persistent shared memory buffer is correctly sized and mapped.
 *
 * @param b Pointer to the PersistentBuffer structure.
 * @param w Target width in logical pixels.
 * @param h Target height in logical pixels.
 * @param scale Output scaling factor.
 */
static void ensure_buffer(PersistentBuffer *b, int w, int h, int scale) {
    int buffer_w = w * scale;
    int buffer_h = h * scale;
    int stride = buffer_w * 4;
    int size = stride * buffer_h;

    if (b->buffer && b->size == size) {
        return;
    }

    if (b->buffer) {
        wl_buffer_destroy(b->buffer);
        munmap(b->data, b->size);
        close(b->fd);
    }

    b->size = size;
    b->width = w;
    b->height = h;
    b->scale = scale;
    b->fd = create_shm_file(size);
    b->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, b->fd, size);
    b->buffer = wl_shm_pool_create_buffer(
        pool, 0, buffer_w, buffer_h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
}

/**
 * @brief Maps a Wayland surface to the shared surface identifier.
 */
static SurfaceId surface_id(struct wl_surface *surf) {
    if (surf && surf == surface) {
        return SURFACE_PANEL;
    }
    if (surf && surf == bg_surface) {
        return SURFACE_BG;
    }
    return SURFACE_NONE;
}

/**
 * @brief Handles geometry updates from the Wayland output.
 */
static void output_geometry(
    void *data, struct wl_output *wl_output, int32_t x, int32_t y,
    int32_t physical_width, int32_t physical_height, int32_t subpixel,
    const char *make, const char *model, int32_t transform) {}

/**
 * @brief Handles mode updates from the Wayland output.
 */
static void output_mode(
    void *data, struct wl_output *wl_output, uint32_t flags, int32_t width,
    int32_t height, int32_t refresh) {}

/**
 * @brief Handles output configuration completion events.
 */
static void output_done(void *data, struct wl_output *wl_output) {}

/**
 * @brief Updates the global output scale factor based on display configuration.
 */
static void output_scale_handler(
    void *data, struct wl_output *wl_output, int32_t factor) {
    if (factor > 0) {
        output_scale = factor;
    }
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale_handler
};

/**
 * @brief Updates the title of an active window toplevel.
 */
static void toplevel_title(
    void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
    const char *title) {
    Toplevel *tl = data;
    snprintf(tl->title, sizeof(tl->title), "%s", title);
    if (configured) {
        draw_frame();
    }
}

/**
 * @brief Updates the application ID and icon for an active window toplevel.
 */
static void toplevel_app_id(
    void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
    const char *app_id) {
    Toplevel *tl = data;
    toplevel_set_app_id(tl, app_id);
    if (configured) {
        draw_frame();
    }
}

/**
 * @brief Handles a window toplevel entering an output.
 */
static void toplevel_output_enter(
    void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
    struct wl_output *output) {}

/**
 * @brief Handles a window toplevel leaving an output.
 */
static void toplevel_output_leave(
    void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
    struct wl_output *output) {}

/**
 * @brief Updates the visual state (active, minimized) of a window toplevel.
 */
static void toplevel_state(
    void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
    struct wl_array *state) {
    Toplevel *tl = data;
    tl->minimized = false;
    tl->active = false;

    uint32_t *s;
    wl_array_for_each(s, state) {
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) {
            tl->minimized = true;
        }
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
            tl->active = true;
        }
    }
    if (configured) {
        draw_frame();
    }
}

/**
 * @brief Handles the completion of state updates for a toplevel.
 */
static void toplevel_done(
    void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {}

/**
 * @brief Handles the closing and destruction of a window toplevel.
 */
static void toplevel_closed(
    void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    Toplevel *tl = data;
    tl->closed = true;
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
    toplevel_destroy(tl);
    if (configured) {
        draw_frame();
    }
}

static const struct zwlr_foreign_toplevel_handle_v1_listener
toplevel_handle_listener = {
    .title = toplevel_title,
    .app_id = toplevel_app_id,
    .output_enter = toplevel_output_enter,
    .output_leave = toplevel_output_leave,
    .state = toplevel_state,
    .done = toplevel_done,
    .closed = toplevel_closed
};

/**
 * @brief Registers a newly opened window with the toplevel manager.
 */
static void manager_toplevel(
    void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
    struct zwlr_foreign_toplevel_handle_v1 *handle) {
    Toplevel *tl = toplevel_create();
    if (!tl) {
        return;
    }
    tl->handle = handle;
    zwlr_foreign_toplevel_handle_v1_add_listener(
        handle, &toplevel_handle_listener, tl);
}

/**
 * @brief Handles completion events from the toplevel manager.
 */
static void manager_finished(
    void *data, struct zwlr_foreign_toplevel_manager_v1 *manager) {}

static const struct zwlr_foreign_toplevel_manager_v1_listener
toplevel_manager_listener = {
    .toplevel = manager_toplevel,
    .finished = manager_finished
};

/**
 * @brief Provides a Cairo surface backed by the shared memory buffer of the
 * requested desktop surface.
 */
static cairo_surface_t* wayland_begin_draw(
    SurfaceId id, int w, int h, int scale) {
    PersistentBuffer *b = (id == SURFACE_BG) ? &bg_buffer : &ui_buffer;
    ensure_buffer(b, w, h, scale);

    int buffer_w = w * scale;
    int buffer_h = h * scale;
    int stride = buffer_w * 4;

    return cairo_image_surface_create_for_data(
        (unsigned char*)b->data, CAIRO_FORMAT_ARGB32,
        buffer_w, buffer_h, stride);
}

/**
 * @brief Presents a rendered buffer. The background only accepts input over
 * its icon cells, and the panel only over its bar unless the menu is open, so
 * every other click falls through to the compositor.
 */
static void wayland_commit(SurfaceId id) {
    struct wl_region *region = wl_compositor_create_region(compositor);

    if (id == SURFACE_BG) {
        for (int i = 0; i < desktop_app_count; i++) {
            int cell_x, cell_y;
            get_desktop_cell(i, &cell_x, &cell_y);
            wl_region_add(
                region, cell_x, cell_y, DESKTOP_CELL_WIDTH, DESKTOP_CELL_HEIGHT);
        }
        wl_surface_set_input_region(bg_surface, region);
        wl_region_destroy(region);

        wl_surface_set_buffer_scale(bg_surface, output_scale);
        wl_surface_attach(bg_surface, bg_buffer.buffer, 0, 0);
        wl_surface_damage(bg_surface, 0, 0, bg_width, bg_height);
        wl_surface_commit(bg_surface);
        return;
    }

    if (menu_open) {
        wl_region_add(region, 0, 0, width, current_height);
    } else {
        wl_region_add(
            region, 0, current_height - PANEL_HEIGHT, width, PANEL_HEIGHT);
    }
    wl_surface_set_input_region(surface, region);
    wl_region_destroy(region);

    wl_surface_set_buffer_scale(surface, output_scale);
    wl_surface_attach(surface, ui_buffer.buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, width, current_height);
    wl_surface_commit(surface);
}

/**
 * @brief Updates the panel's layer dynamically based on menu state.
 */
static void wayland_update_panel_layer() {
    if (layer_shell_version >= 2 && layer_surface) {
        zwlr_layer_surface_v1_set_layer(layer_surface,
            menu_open ? ZWLR_LAYER_SHELL_V1_LAYER_TOP : ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
    }
}

/**
 * @brief Requests focus for a window through the foreign toplevel protocol.
 */
static void wayland_toplevel_activate(Toplevel *tl) {
    if (default_seat) {
        zwlr_foreign_toplevel_handle_v1_activate(tl->handle, default_seat);
    }
}

/**
 * @brief Minimizes or restores a window through the foreign toplevel protocol.
 */
static void wayland_toplevel_set_minimized(Toplevel *tl, bool minimized) {
    if (minimized) {
        zwlr_foreign_toplevel_handle_v1_set_minimized(tl->handle);
    } else {
        zwlr_foreign_toplevel_handle_v1_unset_minimized(tl->handle);
    }
}

/**
 * @brief Forwards pointer button presses to the shared UI code.
 */
static void pointer_button(
    void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
    uint32_t button, uint32_t state) {
    if (state != 1) {
        return;
    }

    double *pos = data;
    handle_pointer_button(
        surface_id(current_pointer_surface), pos[0], pos[1], time);
}

/**
 * @brief Tracks the pointer position and forwards motion to the shared UI code.
 */
static void pointer_motion(
    void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x,
    wl_fixed_t y) {
    double *pos = data;
    pos[0] = wl_fixed_to_double(x);
    pos[1] = wl_fixed_to_double(y);

    handle_pointer_motion(surface_id(current_pointer_surface), pos[0], pos[1]);
}

/**
 * @brief Forwards vertical scroll wheel events to the shared UI code.
 */
static void pointer_axis(
    void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
    wl_fixed_t value) {
    if (axis != 0) {
        return;
    }

    double *pos = data;
    handle_pointer_axis(
        surface_id(current_pointer_surface), pos[0], pos[1],
        wl_fixed_to_double(value));
}

/**
 * @brief Handles pointer enter events, tracking active surfaces and setting the cursor.
 */
static void pointer_enter(
    void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surf, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    current_pointer_surface = surf;
    double *pos = data;
    pos[0] = wl_fixed_to_double(surface_x);
    pos[1] = wl_fixed_to_double(surface_y);

    if (default_cursor && cursor_surface) {
        struct wl_cursor_image *image = default_cursor->images[0];
        struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
        wl_pointer_set_cursor(
            pointer, serial, cursor_surface, image->hotspot_x,
            image->hotspot_y);
        wl_surface_attach(cursor_surface, buffer, 0, 0);
        wl_surface_damage(
            cursor_surface, 0, 0, image->width, image->height);
        wl_surface_commit(cursor_surface);
    }
}

/**
 * @brief Handles pointer leave events, clearing hover and active states.
 */
static void pointer_leave(
    void *data, struct wl_pointer *pointer, uint32_t serial,
    struct wl_surface *surf) {
    if (current_pointer_surface == surf) {
        current_pointer_surface = NULL;
    }
    handle_pointer_leave(surface_id(surf));
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis
};

/**
 * @brief Handles incoming seat capabilities and binds pointer devices.
 */
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        static double mouse_pos[2] = {0, 0};
        struct wl_pointer *pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pointer, &pointer_listener, mouse_pos);
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities
};

/**
 * @brief Configures the bounds for the main layer surface.
 */
static void layer_surface_configure(
    void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
    uint32_t w, uint32_t h) {
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    panel_configure(w, h);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_surface_configure
};

/**
 * @brief Handles configuration events for the background layer surface.
 */
static void bg_layer_surface_configure(
    void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
    uint32_t w, uint32_t h) {
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    bg_configure(w, h);
}

static const struct zwlr_layer_surface_v1_listener bg_layer_listener = {
    .configure = bg_layer_surface_configure
};

/**
 * @brief Global Wayland registry handler for binding essential interfaces.
 */
static void registry_handler(
    void *data, struct wl_registry *registry, uint32_t id,
    const char *interface, uint32_t version) {
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 3);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell_version = version < 4 ? version : 4;
        layer_shell = wl_registry_bind(
            registry, id, &zwlr_layer_shell_v1_interface, layer_shell_version);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        default_seat = wl_registry_bind(registry, id, &wl_seat_interface, 1);
        wl_seat_add_listener(default_seat, &seat_listener, NULL);
    } else if (strcmp(interface,
                      zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        toplevel_manager = wl_registry_bind(
            registry, id, &zwlr_foreign_toplevel_manager_v1_interface, 1);
        zwlr_foreign_toplevel_manager_v1_add_listener(
            toplevel_manager, &toplevel_manager_listener, NULL);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wl_output *output = wl_registry_bind(
            registry, id, &wl_output_interface, 2);
        wl_output_add_listener(output, &output_listener, NULL);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handler
};

/**
 * @brief Connects to the Wayland compositor and sets up the panel and
 * wallpaper layer surfaces. Fails without side effects when there is no
 * compositor or it lacks layer shell support, so another backend can be tried.
 */
static bool wayland_init() {
    display = wl_display_connect(NULL);
    if (!display) {
        return false;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);

    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !layer_shell) {
        while (toplevels_head) {
            toplevel_destroy(toplevels_head);
        }
        wl_display_disconnect(display);
        display = NULL;
        return false;
    }

    cursor_surface = wl_compositor_create_surface(compositor);
    const char *theme = getenv("XCURSOR_THEME");
    const char *size_str = getenv("XCURSOR_SIZE");
    int cursor_size = 24;

    if (size_str) {
        cursor_size = atoi(size_str);
        if (cursor_size <= 0) {
            cursor_size = 24;
        }
    }

    cursor_theme = wl_cursor_theme_load(theme, cursor_size, shm);
    if (cursor_theme) {
        default_cursor = wl_cursor_theme_get_cursor(cursor_theme, "left_ptr");
        if (!default_cursor) {
            default_cursor = wl_cursor_theme_get_cursor(
                cursor_theme, "default");
        }
        if (!default_cursor) {
            default_cursor = wl_cursor_theme_get_cursor(cursor_theme, "arrow");
        }
    }

    surface = wl_compositor_create_surface(compositor);
    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "panel");

    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);

    zwlr_layer_surface_v1_set_size(layer_surface, 0, 4000);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, PANEL_HEIGHT);
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);

    wl_surface_commit(surface);

    bg_surface = wl_compositor_create_surface(compositor);
    bg_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, bg_surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");

    zwlr_layer_surface_v1_set_anchor(bg_layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);

    zwlr_layer_surface_v1_set_exclusive_zone(bg_layer_surface, -1);
    zwlr_layer_surface_v1_add_listener(
        bg_layer_surface, &bg_layer_listener, NULL);

    wl_surface_commit(bg_surface);
    return true;
}

/**
 * @brief Releases cursor resources and disconnects from the compositor.
 */
static void wayland_shutdown() {
    if (cursor_theme) {
        wl_cursor_theme_destroy(cursor_theme);
    }
    if (cursor_surface) {
        wl_surface_destroy(cursor_surface);
    }

    wl_display_disconnect(display);
}

/**
 * @brief Returns the display connection descriptor for the main poll loop.
 */
static int wayland_get_fd() {
    return wl_display_get_fd(display);
}

/**
 * @brief Dispatches queued events and announces the intent to read so the
 * main loop can safely block in poll.
 */
static void wayland_before_poll() {
    while (wl_display_prepare_read(display) != 0) {
        wl_display_dispatch_pending(display);
    }
    wl_display_flush(display);
}

/**
 * @brief Reads and dispatches new events, or cancels the announced read when
 * the poll woke up for another reason.
 */
static void wayland_after_poll(bool readable) {
    if (readable) {
        wl_display_read_events(display);
        wl_display_dispatch_pending(display);
    } else {
        wl_display_cancel_read(display);
    }
}

const Backend wayland_backend = {
    .name = "wayland",
    .init = wayland_init,
    .shutdown = wayland_shutdown,
    .get_fd = wayland_get_fd,
    .before_poll = wayland_before_poll,
    .after_poll = wayland_after_poll,
    .begin_draw = wayland_begin_draw,
    .commit = wayland_commit,
    .update_panel_layer = wayland_update_panel_layer,
    .toplevel_activate = wayland_toplevel_activate,
    .toplevel_set_minimized = wayland_toplevel_set_minimized
};
