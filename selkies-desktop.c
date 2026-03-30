#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <dirent.h>
#include <signal.h>
#include <math.h>
#include <glob.h>
#include <poll.h>
#include <sys/inotify.h>
#include <errno.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cairo/cairo.h>
#include "wlr-layer-shell.h"
#include "wlr-foreign-toplevel-management-unstable-v1.h"
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "vendor/nanosvg.h"
#include "vendor/nanosvgrast.h"
#define PANEL_HEIGHT 30
#define START_BTN_WIDTH 80
#define ICON_SIZE 16
#define MAX_MENU_HEIGHT 600
#define MAX_APPS 2048
#define DESKTOP_ICON_SIZE 48
#define DESKTOP_CELL_WIDTH 96
#define DESKTOP_CELL_HEIGHT 104

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
 * @brief Represents an active Wayland toplevel window.
 */
typedef struct Toplevel {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char title[256];
    char app_id[128]; 
    cairo_surface_t *icon;
    bool minimized;
    bool active;
    bool closed;
    struct Toplevel *next;
} Toplevel;

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

App apps[MAX_APPS];
int app_count = 0;

App desktop_apps[MAX_APPS];
int desktop_app_count = 0;
uint32_t last_click_time = 0;
int last_clicked_desktop_index = -1;
struct wl_surface *current_pointer_surface = NULL;

Category categories[20];
int category_count = 0;

Toplevel *toplevels_head = NULL;

struct wl_display *display;
struct wl_compositor *compositor;
struct wl_shm *shm;
struct zwlr_layer_shell_v1 *layer_shell;
struct wl_surface *surface;
struct zwlr_layer_surface_v1 *layer_surface;
struct wl_surface *bg_surface;
struct zwlr_layer_surface_v1 *bg_layer_surface;
struct wl_seat *default_seat = NULL;
struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager = NULL;
uint32_t layer_shell_version = 1;

struct wl_cursor_theme *cursor_theme = NULL;
struct wl_cursor *default_cursor = NULL;
struct wl_surface *cursor_surface = NULL;

PersistentBuffer ui_buffer = {0};
PersistentBuffer bg_buffer = {0};

int width = 1920; 
int current_height = 4000;
int output_scale = 1;

bool menu_open = false;
bool configured = false; 

int bg_width = 0;
int bg_height = 0;
void draw_bg();

int cat_menu_height = 0;
int app_menu_height = 0;
int app_y_offset_from_bottom = 0;

int hovered_category = -1;
int hovered_app = -1;

double cat_scroll = 0;
double app_scroll = 0;

cairo_surface_t *start_icon = NULL;

const char *cat_map[][2] = {
    {"AudioVideo", "Multimedia"},
    {"Audio", "Multimedia"},
    {"Video", "Multimedia"},
    {"Development", "Development"},
    {"Education", "Education"},
    {"Game", "Games"},
    {"Graphics", "Graphics"},
    {"Network", "Internet"},
    {"WebBrowser", "Internet"},
    {"Office", "Office"},
    {"Science", "Science"},
    {"Settings", "Settings"},
    {"System", "System"},
    {"Utility", "Utilities"},
    {"TerminalEmulator", "System"},
    {"FileManager", "System"},
    {NULL, NULL}
};

void draw_frame();

/**
 * @brief Allocates an anonymous shared memory file descriptor.
 * 
 * @param size Desired size of the shared memory region in bytes.
 * @return int The file descriptor, or -1 on failure.
 */
int create_shm_file(off_t size) {
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
void ensure_buffer(PersistentBuffer *b, int w, int h, int scale) {
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
 * @brief Parses and rasterizes an SVG file into a Cairo surface.
 * 
 * @param filepath Path to the SVG file.
 * @param size Target width and height in pixels.
 * @return cairo_surface_t* Rendered image surface, or NULL on failure.
 */
cairo_surface_t* load_svg_as_cairo_surface(const char *filepath, int size) {
    NSVGimage *image = nsvgParseFromFile(filepath, "px", 96.0f);
    if (!image) {
        return NULL;
    }

    float scale = (float)size / (image->width > image->height ? 
                                 image->width : image->height);

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    unsigned char *img_data = malloc(size * size * 4);

    nsvgRasterize(rast, image, 0, 0, scale, img_data, size, size, size * 4);

    cairo_surface_t *surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, size, size);
    unsigned char *cairo_data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int src_idx = (y * size + x) * 4;
            int dst_idx = (y * stride) + x * 4;

            unsigned char r = img_data[src_idx];
            unsigned char g = img_data[src_idx + 1];
            unsigned char b = img_data[src_idx + 2];
            unsigned char a = img_data[src_idx + 3];

            cairo_data[dst_idx + 0] = (b * a) / 255;
            cairo_data[dst_idx + 1] = (g * a) / 255;
            cairo_data[dst_idx + 2] = (r * a) / 255;
            cairo_data[dst_idx + 3] = a;
        }
    }

    cairo_surface_mark_dirty(surf);

    free(img_data);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    return surf;
}

/**
 * @brief Locates and loads an icon by name or absolute path from system and user directories.
 * 
 * @param name The icon name or file path.
 * @param size Target icon size.
 * @return cairo_surface_t* The loaded Cairo surface, or NULL if not found.
 */
cairo_surface_t* get_icon(const char *name, int size) {
    if (!name || name[0] == '\0') {
        return NULL;
    }
    
    if (name[0] == '/') {
        if (access(name, F_OK) == 0) {
            if (strstr(name, ".svg")) {
                return load_svg_as_cairo_surface(name, size);
            } else {
                cairo_surface_t *s = cairo_image_surface_create_from_png(name);
                if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) {
                    return s;
                }
                cairo_surface_destroy(s);
            }
        }
        char path_with_ext[1024];
        snprintf(path_with_ext, sizeof(path_with_ext), "%s.png", name);
        if (access(path_with_ext, F_OK) == 0) {
            cairo_surface_t *s = cairo_image_surface_create_from_png(path_with_ext);
            if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) {
                return s;
            }
            cairo_surface_destroy(s);
        }
        snprintf(path_with_ext, sizeof(path_with_ext), "%s.svg", name);
        if (access(path_with_ext, F_OK) == 0) {
            return load_svg_as_cairo_surface(path_with_ext, size);
        }
        return NULL;
    }

    const char *formats[] = { "svg", "png", NULL };
    const char *sizes[] = {
        "scalable", "512x512", "256x256", "192x192", "128x128", 
        "96x96", "72x72", "64x64", "48x48", "36x36", "32x32", 
        "24x24", "22x22", "16x16", "*", NULL
    };

    char pattern[1024];
    glob_t g;
    const char *home = getenv("HOME");

    for (int f = 0; formats[f] != NULL; f++) {
        for (int s = 0; sizes[s] != NULL; s++) {
            const char *sys_patterns[] = {
                "/usr/share/icons/hicolor/%s/apps/%s.%s",
                "/usr/share/icons/hicolor/%s/apps/*/%s.%s",
                "/usr/share/icons/Adwaita/%s/apps/%s.%s",
                "/usr/share/icons/Papirus/%s/apps/%s.%s",
                "/usr/share/icons/*/%s/apps/%s.%s",
                "/usr/share/icons/*/%s/*/%s.%s",
                NULL
            };

            for (int p = 0; sys_patterns[p] != NULL; p++) {
                snprintf(pattern, sizeof(pattern), sys_patterns[p], sizes[s], 
                         name, formats[f]);
                if (glob(pattern, GLOB_NOSORT, NULL, &g) == 0) {
                    for (size_t j = 0; j < g.gl_pathc; j++) {
                        cairo_surface_t *surf = NULL;
                        if (strcmp(formats[f], "svg") == 0) {
                            surf = load_svg_as_cairo_surface(
                                g.gl_pathv[j], size);
                        } else {
                            surf = cairo_image_surface_create_from_png(
                                g.gl_pathv[j]);
                        }
                        if (surf && cairo_surface_status(surf) == 
                            CAIRO_STATUS_SUCCESS) {
                            globfree(&g);
                            return surf;
                        }
                        if (surf) {
                            cairo_surface_destroy(surf);
                        }
                    }
                    globfree(&g);
                }
            }

            if (home) {
                const char *user_patterns[] = {
                    "%s/.local/share/icons/hicolor/%s/apps/%s.%s",
                    "%s/.local/share/icons/hicolor/%s/apps/*/%s.%s",
                    "%s/.local/share/icons/*/%s/apps/%s.%s",
                    "%s/.local/share/icons/*/%s/*/%s.%s",
                    NULL
                };

                for (int p = 0; user_patterns[p] != NULL; p++) {
                    snprintf(pattern, sizeof(pattern), user_patterns[p], home, 
                             sizes[s], name, formats[f]);
                    if (glob(pattern, GLOB_NOSORT, NULL, &g) == 0) {
                        for (size_t j = 0; j < g.gl_pathc; j++) {
                            cairo_surface_t *surf = NULL;
                            if (strcmp(formats[f], "svg") == 0) {
                                surf = load_svg_as_cairo_surface(
                                    g.gl_pathv[j], size);
                            } else {
                                surf = cairo_image_surface_create_from_png(
                                    g.gl_pathv[j]);
                            }
                            if (surf && cairo_surface_status(surf) == 
                                CAIRO_STATUS_SUCCESS) {
                                globfree(&g);
                                return surf;
                            }
                            if (surf) {
                                cairo_surface_destroy(surf);
                            }
                        }
                        globfree(&g);
                    }
                }
            }
        }

        snprintf(pattern, sizeof(pattern), "/usr/share/pixmaps/%s.%s", 
                 name, formats[f]);
        if (glob(pattern, GLOB_NOSORT, NULL, &g) == 0) {
            for (size_t j = 0; j < g.gl_pathc; j++) {
                cairo_surface_t *surf = NULL;
                if (strcmp(formats[f], "svg") == 0) {
                    surf = load_svg_as_cairo_surface(g.gl_pathv[j], size);
                } else {
                    surf = cairo_image_surface_create_from_png(g.gl_pathv[j]);
                }
                if (surf && cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
                    globfree(&g);
                    return surf;
                }
                if (surf) {
                    cairo_surface_destroy(surf);
                }
            }
            globfree(&g);
        }
    }

    char lower_name[128];
    snprintf(lower_name, sizeof(lower_name), "%s", name);
    bool changed = false;
    for(int i = 0; lower_name[i]; i++) {
        if (tolower(lower_name[i]) != lower_name[i]) {
            lower_name[i] = tolower(lower_name[i]);
            changed = true;
        }
    }
    
    if (changed) {
        return get_icon(lower_name, size);
    }

    return NULL;
}

/**
 * @brief Retrieves an existing category by name, or creates a new one.
 * 
 * @param name The category name.
 * @return Category* Pointer to the category.
 */
Category* get_or_create_category(const char *name) {
    for (int i = 0; i < category_count; i++) {
        if (strcmp(categories[i].name, name) == 0) {
            return &categories[i];
        }
    }
    strcpy(categories[category_count].name, name);
    categories[category_count].app_count = 0;
    return &categories[category_count++];
}

/**
 * @brief Comparator for sorting App pointers alphabetically.
 * 
 * @param a Pointer to first App pointer.
 * @param b Pointer to second App pointer.
 * @return int Comparison result.
 */
int compare_apps(const void *a, const void *b) {
    App **app_a = (App **)a;
    App **app_b = (App **)b;
    return strcasecmp((*app_a)->name, (*app_b)->name);
}

/**
 * @brief Comparator for sorting Categories alphabetically.
 * 
 * @param a Pointer to first Category.
 * @param b Pointer to second Category.
 * @return int Comparison result.
 */
int compare_categories(const void *a, const void *b) {
    Category *cat_a = (Category *)a;
    Category *cat_b = (Category *)b;
    if (strcmp(cat_a->name, "Misc") == 0) return 1;
    if (strcmp(cat_b->name, "Misc") == 0) return -1;
    return strcasecmp(cat_a->name, cat_b->name);
}

/**
 * @brief Scans a specific directory for .desktop files and populates app lists.
 * 
 * @param base_path Directory path to scan.
 */
void scan_app_dir(const char *base_path) {
    DIR *d = opendir(base_path);
    if (!d) {
        return;
    }
    
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL && app_count < MAX_APPS) {
        if (!strstr(dir->d_name, ".desktop")) {
            continue;
        }
        
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", base_path, dir->d_name);
        
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        
        char line[256];
        char name[128] = {0};
        char exec[256] = {0};
        char icon_name[128] = {0};
        char categories_str[256] = {0};
        bool nodisplay = false;
        
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "NoDisplay=true", 14) == 0) {
                nodisplay = true;
            } else if (strncmp(line, "Name=", 5) == 0 && name[0] == '\0') {
                snprintf(name, sizeof(name), "%.127s", line + 5);
                name[strcspn(name, "\n")] = 0;
            } else if (strncmp(line, "Exec=", 5) == 0 && exec[0] == '\0') {
                snprintf(exec, sizeof(exec), "%.255s", line + 5);
                exec[strcspn(exec, "\n")] = 0;
                char *pct = strchr(exec, '%'); 
                if (pct) {
                    *(pct - 1) = 0;
                }
            } else if (strncmp(line, "Icon=", 5) == 0 && icon_name[0] == '\0') {
                snprintf(icon_name, sizeof(icon_name), "%.127s", line + 5);
                icon_name[strcspn(icon_name, "\n")] = 0;
            } else if (strncmp(line, "Categories=", 11) == 0 && 
                       categories_str[0] == '\0') {
                snprintf(categories_str, sizeof(categories_str), "%.255s", 
                         line + 11);
                categories_str[strcspn(categories_str, "\n")] = 0;
            }
        }
        fclose(f);
       
        if (!nodisplay && name[0] && exec[0]) {
            strcpy(apps[app_count].name, name);
            strcpy(apps[app_count].desktop_path, path);
            apps[app_count].icon = get_icon(icon_name, ICON_SIZE);
            
            Category *app_cat = NULL; 
            if (categories_str[0]) {
                for (int i = 0; cat_map[i][0] != NULL; i++) {
                    if (strstr(categories_str, cat_map[i][0])) {
                        app_cat = get_or_create_category(cat_map[i][1]);
                        break;
                    }
                }
            }
            if (!app_cat) {
                app_cat = get_or_create_category("Misc");
            }
            
            app_cat->apps[app_cat->app_count++] = &apps[app_count];
            app_count++;
        }
    }
    closedir(d);
}

/**
 * @brief Scans the user's Desktop directory for .desktop files.
 */
void load_desktop_apps() {
    const char *home = getenv("HOME");
    if (!home) return;
    char base_path[1024];
    snprintf(base_path, sizeof(base_path), "%s/Desktop", home);
    
    DIR *d = opendir(base_path);
    if (!d) return;
    
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL && desktop_app_count < MAX_APPS) {
        if (!strstr(dir->d_name, ".desktop")) continue;
        
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", base_path, dir->d_name);
        
        FILE *f = fopen(path, "r");
        if (!f) continue;
        
        char line[256];
        char name[128] = {0};
        char exec[256] = {0};
        char icon_name[128] = {0};
        
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Name=", 5) == 0 && name[0] == '\0') {
                snprintf(name, sizeof(name), "%.127s", line + 5);
                name[strcspn(name, "\n")] = 0;
            } else if (strncmp(line, "Exec=", 5) == 0 && exec[0] == '\0') {
                snprintf(exec, sizeof(exec), "%.255s", line + 5);
                exec[strcspn(exec, "\n")] = 0;
                char *pct = strchr(exec, '%'); 
                if (pct) *(pct - 1) = 0;
            } else if (strncmp(line, "Icon=", 5) == 0 && icon_name[0] == '\0') {
                snprintf(icon_name, sizeof(icon_name), "%.127s", line + 5);
                icon_name[strcspn(icon_name, "\n")] = 0;
            }
        }
        fclose(f);
        if (name[0] && exec[0]) {
            strcpy(desktop_apps[desktop_app_count].name, name);
            strcpy(desktop_apps[desktop_app_count].desktop_path, path);
            desktop_apps[desktop_app_count].icon = get_icon(
                icon_name, DESKTOP_ICON_SIZE * 2);
            desktop_app_count++;
        }    
    }
    closedir(d);
}

/**
 * @brief Orchestrates application loading from all configured system and user directories.
 */
void load_apps() {
    scan_app_dir("/usr/share/applications");
    
    const char *home = getenv("HOME");
    if (home) {
        char user_path[1024];
        snprintf(user_path, sizeof(user_path), 
                 "%s/.local/share/applications", home);
        scan_app_dir(user_path);
    }

    for (int i = 0; i < category_count; i++) {
        qsort(categories[i].apps, categories[i].app_count, 
              sizeof(App*), compare_apps);
    }
    qsort(categories, category_count, sizeof(Category), compare_categories);
}

/**
 * @brief Calculates the geometry and scroll offsets for the application menus.
 */
void update_menu_heights() {
    cat_menu_height = category_count * 30 + 10;
    if (cat_menu_height > MAX_MENU_HEIGHT) {
        cat_menu_height = MAX_MENU_HEIGHT;
    }
    
    if (hovered_category >= 0 && hovered_category < category_count) {
        app_menu_height = categories[hovered_category].app_count * 30 + 10;
        if (app_menu_height > MAX_MENU_HEIGHT) {
            app_menu_height = MAX_MENU_HEIGHT;
        }

        double center_from_bottom = cat_menu_height - 5 - 
            (hovered_category * 30) + cat_scroll - 15;
            
        int desired_app_y_from_bottom = 
            (int)(center_from_bottom + (app_menu_height / 2.0));

        if (desired_app_y_from_bottom < app_menu_height) {
            desired_app_y_from_bottom = app_menu_height;
        }
        if (desired_app_y_from_bottom > MAX_MENU_HEIGHT) {
            desired_app_y_from_bottom = MAX_MENU_HEIGHT;
        }

        app_y_offset_from_bottom = desired_app_y_from_bottom;
    } else {
        app_menu_height = 0;
        app_y_offset_from_bottom = 0;
    }
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
    snprintf(tl->app_id, sizeof(tl->app_id), "%s", app_id);
    if (tl->icon) {
        cairo_surface_destroy(tl->icon);
    }
    tl->icon = get_icon(app_id, ICON_SIZE);
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
    
    if (toplevels_head == tl) {
        toplevels_head = tl->next;
    } else {
        Toplevel *curr = toplevels_head;
        while (curr && curr->next != tl) {
            curr = curr->next;
        }
        if (curr) {
            curr->next = tl->next;
        }
    }
    
    if (tl->icon) {
        cairo_surface_destroy(tl->icon);
    }
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
    free(tl);
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
    Toplevel *tl = calloc(1, sizeof(Toplevel));
    tl->handle = handle;
    tl->next = toplevels_head;
    toplevels_head = tl;
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
 * @brief Renders a scaled icon to a Cairo context.
 * 
 * @param cr The Cairo context.
 * @param icon The Cairo surface containing the icon.
 * @param x X coordinate for rendering.
 * @param y Y coordinate for rendering.
 * @param size The target rendering size.
 */
void draw_icon(
    cairo_t *cr, cairo_surface_t *icon, double x, double y, double size) {
    if (!icon) {
        return;
    }
    
    double w = cairo_image_surface_get_width(icon);
    double h = cairo_image_surface_get_height(icon);
    if (w == 0 || h == 0) {
        return;
    }
    
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, size / w, size / h);
    cairo_set_source_surface(cr, icon, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

/**
 * @brief Defines a rounded rectangle path in a Cairo context.
 * 
 * @param cr The Cairo context.
 * @param x Top-left X coordinate.
 * @param y Top-left Y coordinate.
 * @param w Width of the rectangle.
 * @param h Height of the rectangle.
 * @param r Border radius.
 */
void rounded_rect(
    cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI/2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI/2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI/2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3*M_PI/2);
    cairo_close_path(cr);
}

/**
 * @brief Configures and renders the background wallpaper layer surface.
 */
void draw_bg() {
    if (bg_width == 0 || bg_height == 0) return;

    ensure_buffer(&bg_buffer, bg_width, bg_height, output_scale);

    int buffer_w = bg_width * output_scale;
    int buffer_h = bg_height * output_scale;
    int stride = buffer_w * 4;

    cairo_surface_t *cairo_surf = cairo_image_surface_create_for_data(
        (unsigned char*)bg_buffer.data, CAIRO_FORMAT_ARGB32, 
        buffer_w, buffer_h, stride);
    cairo_t *cr = cairo_create(cairo_surf);
    
    cairo_scale(cr, output_scale, output_scale);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (start_icon && cairo_surface_status(start_icon) == CAIRO_STATUS_SUCCESS) {
        cairo_save(cr);
        cairo_identity_matrix(cr);
        double icon_w = cairo_image_surface_get_width(start_icon);
        double icon_h = cairo_image_surface_get_height(start_icon);
        cairo_set_source_surface(
            cr, start_icon, (buffer_w - icon_w) / 2.0, 
            (buffer_h - icon_h) / 2.0);
            
        cairo_paint(cr);
        cairo_restore(cr);
    }

    cairo_select_font_face(
        cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    
    int max_rows = (bg_height - PANEL_HEIGHT) / DESKTOP_CELL_HEIGHT;
    if (max_rows < 1) max_rows = 1;
    
    for (int i = 0; i < desktop_app_count; i++) {
        int col = i / max_rows;
        int row = i % max_rows;
        
        double cell_x = col * DESKTOP_CELL_WIDTH + 10;
        double cell_y = row * DESKTOP_CELL_HEIGHT + 10;

        if (i == last_clicked_desktop_index) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.2);
            rounded_rect(
                cr, cell_x, cell_y, DESKTOP_CELL_WIDTH, DESKTOP_CELL_HEIGHT, 8.0);
            cairo_fill(cr);
        }
        
        if (desktop_apps[i].icon) {
            double icon_w = cairo_image_surface_get_width(desktop_apps[i].icon);
            double icon_h = cairo_image_surface_get_height(desktop_apps[i].icon);
            double scale = (double)DESKTOP_ICON_SIZE / 
                           (icon_w > icon_h ? icon_w : icon_h);
            
            double draw_x = cell_x + (DESKTOP_CELL_WIDTH - (icon_w * scale)) / 2.0;
            double draw_y = cell_y + 5;
            
            cairo_save(cr);
            cairo_translate(cr, draw_x, draw_y);
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, desktop_apps[i].icon, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        }
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        
        char line1[256] = {0};
        char line2[256] = {0};
        char temp[256] = {0};
        const char *name = desktop_apps[i].name;
        cairo_text_extents_t extents;
        cairo_text_extents(cr, name, &extents);
        
        double max_w = DESKTOP_CELL_WIDTH - 4.0;
        
        if (extents.width <= max_w) {
            strcpy(line1, name);
        } else {
            int len = strlen(name);
            int split_idx = -1;

            for (int j = 0; j < len; j++) {
                if (name[j] == ' ') {
                    snprintf(temp, sizeof(temp), "%.*s", j, name);
                    cairo_text_extents(cr, temp, &extents); 
                    if (extents.width <= max_w) {
                        split_idx = j;
                    } else {
                        break;
                    }
                }
            }
            
            if (split_idx != -1) {
                snprintf(line1, sizeof(line1), "%.*s", split_idx, name);
                line1[split_idx] = '\0';
                
                const char *rem = name + split_idx + 1;
                cairo_text_extents(cr, rem, &extents);
                
                if (extents.width <= max_w) {
                    strcpy(line2, rem);
                } else {
                    for (int j = strlen(rem); j >= 0; j--) {
                        snprintf(temp, sizeof(temp), "%.*s...", j, rem);
                        cairo_text_extents(cr, temp, &extents);
                        if (extents.width <= max_w || j == 0) {
                            strcpy(line2, temp);
                            break;
                        }
                    }
                }
            } else {
                for (int j = len; j >= 0; j--) {
                    snprintf(temp, sizeof(temp), "%.*s...", j, name);
                    cairo_text_extents(cr, temp, &extents);
                    if (extents.width <= max_w || j == 0) {
                        strcpy(line1, temp);
                        break;
                    }
                }
            }
        }
        
        cairo_save(cr);
        cairo_rectangle(cr, cell_x, cell_y + DESKTOP_ICON_SIZE + 10, 
                        DESKTOP_CELL_WIDTH, 40);
        cairo_clip(cr);
        
        cairo_text_extents(cr, line1, &extents);
        double text_x = cell_x + (DESKTOP_CELL_WIDTH - extents.width) / 2.0;
        cairo_move_to(cr, text_x, cell_y + DESKTOP_ICON_SIZE + 22);
        cairo_show_text(cr, line1);
        
        if (line2[0] != '\0') {
            cairo_text_extents(cr, line2, &extents);
            text_x = cell_x + (DESKTOP_CELL_WIDTH - extents.width) / 2.0;
            cairo_move_to(cr, text_x, cell_y + DESKTOP_ICON_SIZE + 36);
            cairo_show_text(cr, line2);
        }
        
        cairo_restore(cr);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(cairo_surf);

    struct wl_region *region = wl_compositor_create_region(compositor);
    for (int i = 0; i < desktop_app_count; i++) {
        int col = i / max_rows;
        int row = i % max_rows;
        int cell_x = col * DESKTOP_CELL_WIDTH + 10;
        int cell_y = row * DESKTOP_CELL_HEIGHT + 10;
        wl_region_add(
            region, cell_x, cell_y, DESKTOP_CELL_WIDTH, DESKTOP_CELL_HEIGHT);
    }
    wl_surface_set_input_region(bg_surface, region);
    wl_region_destroy(region);

    wl_surface_set_buffer_scale(bg_surface, output_scale);
    wl_surface_attach(bg_surface, bg_buffer.buffer, 0, 0);
    wl_surface_damage(bg_surface, 0, 0, bg_width, bg_height);
    wl_surface_commit(bg_surface);
}

/**
 * @brief Handles configuration events for the background layer surface.
 */
static void bg_layer_surface_configure(
    void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
    uint32_t w, uint32_t h) {
    zwlr_layer_surface_v1_ack_configure(layer, serial);

    if (w == 0 || h == 0) {
        return;
    }

    bg_width = w;
    bg_height = h;
    draw_bg();
}

static const struct zwlr_layer_surface_v1_listener bg_layer_listener = { 
    .configure = bg_layer_surface_configure 
};

/**
 * @brief Renders the main panel UI, including the start menu and window list.
 */
void draw_frame() {
    ensure_buffer(&ui_buffer, width, current_height, output_scale);

    int buffer_w = width * output_scale;
    int buffer_h = current_height * output_scale;
    int stride = buffer_w * 4;

    cairo_surface_t *cairo_surf = cairo_image_surface_create_for_data(
        (unsigned char*)ui_buffer.data, CAIRO_FORMAT_ARGB32, 
        buffer_w, buffer_h, stride);
    cairo_t *cr = cairo_create(cairo_surf);
    
    cairo_scale(cr, output_scale, output_scale);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 1.0);
    cairo_rectangle(cr, 0, current_height - PANEL_HEIGHT, width, PANEL_HEIGHT);
    cairo_fill(cr);

    int num_windows = 0;
    Toplevel *tl_count = toplevels_head;
    while (tl_count) {
        if (!tl_count->closed) num_windows++;
        tl_count = tl_count->next;
    }

    int padding = 5;
    int max_btn_width = 150;
    int min_btn_width = ICON_SIZE + 10;
    int btn_width = max_btn_width;

    if (num_windows > 0) {
        int available_space = width - START_BTN_WIDTH - padding - 
                              (num_windows * padding);
        btn_width = available_space / num_windows;
        if (btn_width > max_btn_width) btn_width = max_btn_width;
        if (btn_width < min_btn_width) btn_width = min_btn_width;
    }

    int x_offset = START_BTN_WIDTH + padding;

    Toplevel *tl = toplevels_head;
    while (tl) {
        if (tl->closed) { 
            tl = tl->next; 
            continue; 
        }

        if (tl->active) {
            cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 1.0);
        } else if (tl->minimized) {
            cairo_set_source_rgba(cr, 0.05, 0.05, 0.05, 1.0);
        } else {
            cairo_set_source_rgba(cr, 0.15, 0.15, 0.15, 1.0);
        }
        
        rounded_rect(cr, x_offset, current_height - PANEL_HEIGHT + 3, 
                     btn_width, PANEL_HEIGHT - 6, 4.0);
        cairo_fill(cr);
        
        cairo_set_source_rgba(cr, 0.4, 0.4, 0.4, 1.0);
        rounded_rect(cr, x_offset, current_height - PANEL_HEIGHT + 3, 
                     btn_width, PANEL_HEIGHT - 6, 4.0);
        cairo_stroke(cr);

        int text_x_offset = x_offset + 5;
        if (tl->icon) {
            draw_icon(cr, tl->icon, x_offset + 5, 
                      current_height - PANEL_HEIGHT + 7, ICON_SIZE);
            text_x_offset += ICON_SIZE + 5;
        }

        double clip_width = btn_width - (text_x_offset - x_offset) - 5;
        if (clip_width > 0) {
            cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 1.0);
            cairo_select_font_face(
                cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, 
                CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 12);
            
            cairo_save(cr);
            cairo_rectangle(cr, text_x_offset, current_height - PANEL_HEIGHT, 
                            clip_width, PANEL_HEIGHT);
            cairo_clip(cr);
            cairo_move_to(cr, text_x_offset, current_height - 10);
            
            const char *display_text = "Unknown";
            if (tl->title[0] != '\0') {
                display_text = tl->title;
            } else if (tl->app_id[0] != '\0') {
                display_text = tl->app_id;
            }
            
            cairo_show_text(cr, display_text);
            cairo_restore(cr);
        }

        x_offset += btn_width + padding;
        tl = tl->next;
    }

    cairo_set_source_rgba(cr, 0.25, 0.25, 0.25, 1.0);
    rounded_rect(cr, 4, current_height - PANEL_HEIGHT + 4, 
                 START_BTN_WIDTH - 8, PANEL_HEIGHT - 8, 4.0);
    cairo_fill(cr);
    
    if (start_icon && cairo_surface_status(start_icon) == CAIRO_STATUS_SUCCESS) {
        double icon_w = cairo_image_surface_get_width(start_icon);
        double icon_h = cairo_image_surface_get_height(start_icon);
        double scale = (double)ICON_SIZE / (icon_w > icon_h ? icon_w : icon_h);
        double draw_x = (START_BTN_WIDTH - (icon_w * scale)) / 2.0;
        double draw_y = current_height - PANEL_HEIGHT + 
                        (PANEL_HEIGHT - (icon_h * scale)) / 2.0;
        
        cairo_save(cr);
        cairo_translate(cr, draw_x, draw_y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, start_icon, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_select_font_face(
            cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, 20, current_height - 10);
        cairo_show_text(cr, "Start");
    }

    if (menu_open) {
        int cat_x = 2;
        int cat_y = current_height - PANEL_HEIGHT - cat_menu_height - 4;
        
        cairo_save(cr);
        rounded_rect(cr, cat_x, cat_y, 180, cat_menu_height, 8.0);
        cairo_clip(cr);
        
        cairo_set_source_rgba(cr, 0.15, 0.15, 0.15, 1.0);
        cairo_paint(cr);
        
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_select_font_face(
            cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 13);
        
        for (int i = 0; i < category_count; i++) {
            int item_y = cat_y + 5 + (i * 30) - cat_scroll;
            if (item_y > cat_y + cat_menu_height || item_y + 30 < cat_y) {
                continue;
            }

            if (i == hovered_category) {
                cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 1.0);
                cairo_rectangle(cr, cat_x, item_y, 180, 30);
                cairo_fill(cr);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            }
            
            cairo_save(cr);
            cairo_rectangle(cr, cat_x + 10, item_y, 150, 30);
            cairo_clip(cr);
            cairo_move_to(cr, cat_x + 10, item_y + 20);
            cairo_show_text(cr, categories[i].name);
            cairo_restore(cr);
            
            cairo_move_to(cr, cat_x + 160, item_y + 10);
            cairo_line_to(cr, cat_x + 165, item_y + 15);
            cairo_line_to(cr, cat_x + 160, item_y + 20); 
            cairo_set_line_width(cr, 1.5);
            cairo_stroke(cr);
        }
        cairo_restore(cr);

        if (hovered_category >= 0) {
            int app_x = cat_x + 180 + 4;
            int app_y = current_height - PANEL_HEIGHT - 
                        app_y_offset_from_bottom - 4;
            
            cairo_save(cr);
            rounded_rect(cr, app_x, app_y, 250, app_menu_height, 8.0);
            cairo_clip(cr);
            
            cairo_set_source_rgba(cr, 0.18, 0.18, 0.18, 1.0);
            cairo_paint(cr);
            
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            cairo_select_font_face(
                cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, 
                CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 13);
            
            Category *cat = &categories[hovered_category];
            for (int i = 0; i < cat->app_count; i++) {
                int item_y = app_y + 5 + (i * 30) - app_scroll;
                if (item_y > app_y + app_menu_height || item_y + 30 < app_y) {
                    continue;
                }
                
                if (i == hovered_app) {
                    cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 1.0);
                    cairo_rectangle(cr, app_x, item_y, 250, 30);
                    cairo_fill(cr);
                    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
                }

                int text_x = app_x + 10;
                if (cat->apps[i]->icon) {
                    draw_icon(cr, cat->apps[i]->icon, text_x, item_y + 7, 
                              ICON_SIZE);
                    text_x += ICON_SIZE + 10;
                }
                
                cairo_save(cr);
                cairo_rectangle(cr, text_x, item_y, 250 - (text_x - app_x), 30);
                cairo_clip(cr);
                cairo_move_to(cr, text_x, item_y + 20);
                cairo_show_text(cr, cat->apps[i]->name);
                cairo_restore(cr);
            }
            cairo_restore(cr);
        }
    }

    struct wl_region *region = wl_compositor_create_region(compositor);
    if (menu_open) {
        wl_region_add(region, 0, 0, width, current_height);
    } else {
        wl_region_add(
            region, 0, current_height - PANEL_HEIGHT, width, PANEL_HEIGHT);
    }
    wl_surface_set_input_region(surface, region);
    wl_region_destroy(region);

    cairo_destroy(cr);
    cairo_surface_destroy(cairo_surf);

    wl_surface_set_buffer_scale(surface, output_scale);
    wl_surface_attach(surface, ui_buffer.buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, width, current_height);
    wl_surface_commit(surface);
}

/**
 * @brief Updates the panel's layer dynamically based on menu state.
 */
void update_panel_layer() {
    if (layer_shell_version >= 2 && layer_surface) {
        zwlr_layer_surface_v1_set_layer(layer_surface, 
            menu_open ? ZWLR_LAYER_SHELL_V1_LAYER_TOP : ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
    }
}

/**
 * @brief Handles pointer button press events to manage menu interactions and 
 * window state toggling. Uses fallback execution strategies for desktop files.
 */
static void pointer_button(
    void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
    uint32_t button, uint32_t state) {
    if (state != 1) {
        return;
    }

    double *pos = data;
    double x = pos[0];
    double y = pos[1];

    if (current_pointer_surface == bg_surface) {
        int max_rows = (bg_height - PANEL_HEIGHT) / DESKTOP_CELL_HEIGHT;
        if (max_rows < 1) max_rows = 1;
        
        int clicked_idx = -1;
        for (int i = 0; i < desktop_app_count; i++) {
            int col = i / max_rows;
            int row = i % max_rows;
            int cell_x = col * DESKTOP_CELL_WIDTH + 10;
            int cell_y = row * DESKTOP_CELL_HEIGHT + 10;
            if (x >= cell_x && x < cell_x + DESKTOP_CELL_WIDTH &&
                y >= cell_y && y < cell_y + DESKTOP_CELL_HEIGHT) {
                clicked_idx = i;
                break;
            }
        }
       
        if (clicked_idx != -1) {
            if (time - last_click_time < 300 && 
                last_clicked_desktop_index == clicked_idx) {
                if (fork() == 0) { 
                    setsid(); 
                    if (fork() == 0) {
                        int fd = open("/dev/null", O_RDWR);
                        if (fd >= 0) {
                            dup2(fd, STDIN_FILENO);
                            dup2(fd, STDOUT_FILENO);
                            dup2(fd, STDERR_FILENO);
                            if (fd > 2) close(fd);
                        } 
                        char *d_path = desktop_apps[clicked_idx].desktop_path;
                        execlp("dex", "dex", d_path, NULL);
                        execlp("gio", "gio", "launch", d_path, NULL);
                        execlp("gtk-launch", "gtk-launch", d_path, NULL);
                        exit(0);
                    }
                    exit(0);
                }
                last_click_time = 0;
                last_clicked_desktop_index = -1;
                draw_bg();
            } else {
                last_click_time = time;
                if (last_clicked_desktop_index != clicked_idx) {
                    last_clicked_desktop_index = clicked_idx;
                    draw_bg();
                }
            }
        } else {
            last_click_time = time;
            if (last_clicked_desktop_index != -1) {
                last_clicked_desktop_index = -1;
                draw_bg();
            }
        }

        if (menu_open) {
            menu_open = false;
            update_panel_layer();
            hovered_category = -1;
            hovered_app = -1;
            update_menu_heights();
            if (configured) {
                draw_frame();
            }
        }
        return;
    }

    if (last_clicked_desktop_index != -1) {
        last_clicked_desktop_index = -1;
        draw_bg();
    }

    if (x < START_BTN_WIDTH && y > current_height - PANEL_HEIGHT) { 
        menu_open = !menu_open;
        update_panel_layer();
        if (!menu_open) { 
            hovered_category = -1; 
            hovered_app = -1; 
        }
        update_menu_heights();
        if (configured) {
            draw_frame();
        }
    } else if (y > current_height - PANEL_HEIGHT && x > START_BTN_WIDTH) {
        if (menu_open) {
            menu_open = false;
            hovered_category = -1;
            hovered_app = -1;
            update_menu_heights();
        }
        
        int num_windows = 0;
        Toplevel *tl_count = toplevels_head;
        while (tl_count) {
            if (!tl_count->closed) num_windows++;
            tl_count = tl_count->next;
        }

        int padding = 5;
        int max_btn_width = 150;
        int min_btn_width = ICON_SIZE + 10;
        int btn_width = max_btn_width;

        if (num_windows > 0) {
            int available_space = width - START_BTN_WIDTH - padding - 
                                  (num_windows * padding);
            btn_width = available_space / num_windows;
            if (btn_width > max_btn_width) btn_width = max_btn_width;
            if (btn_width < min_btn_width) btn_width = min_btn_width;
        }

        int click_x = x - START_BTN_WIDTH - padding;
        
        if (click_x >= 0) {
            int step = btn_width + padding;
            int index = click_x / step;
            int rem = click_x % step;
            
            if (rem <= btn_width) {
                Toplevel *tl = toplevels_head;
                int i = 0;
                while (tl) {
                    if (!tl->closed) {
                        if (i == index) break;
                        i++;
                    }
                    tl = tl->next;
                }
                
                if (tl) {
                    if (tl->active) {
                        zwlr_foreign_toplevel_handle_v1_set_minimized(
                            tl->handle);
                    } else if (tl->minimized) {
                        zwlr_foreign_toplevel_handle_v1_unset_minimized(
                            tl->handle);
                        if (default_seat) {
                            zwlr_foreign_toplevel_handle_v1_activate(
                                tl->handle, default_seat);
                        }
                    } else {
                        if (default_seat) {
                            zwlr_foreign_toplevel_handle_v1_activate(
                                tl->handle, default_seat);
                        }
                    }
                }
            }
        }
        if (configured) {
            draw_frame();
        }
    } else if (menu_open) {
        int cat_x = 4;
        int cat_y = current_height - PANEL_HEIGHT - cat_menu_height - 4;
        int app_x = cat_x + 180 + 4;
        int app_y = current_height - PANEL_HEIGHT - 
                    app_y_offset_from_bottom - 4;
            
        bool in_cat = (x >= cat_x && x < cat_x + 180 && 
                       y >= cat_y && y < cat_y + cat_menu_height);
        bool in_app = (hovered_category >= 0 && x >= app_x && 
                       x < app_x + 250 && y >= app_y && 
                       y < app_y + app_menu_height);
                
        if (in_app) {
            int idx = (y - app_y - 5 + app_scroll) / 30;
            if (idx >= 0 && idx < categories[hovered_category].app_count) {
                if (fork() == 0) { 
                    setsid(); 
                    if (fork() == 0) {
                        int fd = open("/dev/null", O_RDWR);
                        if (fd >= 0) {
                            dup2(fd, STDIN_FILENO);
                            dup2(fd, STDOUT_FILENO);
                            dup2(fd, STDERR_FILENO);
                            if (fd > 2) close(fd);
                        } 
                        
                        char *d_path = 
                            categories[hovered_category].apps[idx]->desktop_path;
                        
                        execlp("dex", "dex", d_path, NULL);
                        execlp("gio", "gio", "launch", d_path, NULL);
                        execlp("gtk-launch", "gtk-launch", d_path, NULL);
                        
                        exit(0);
                    }
                    exit(0);
                }
                menu_open = false;
                update_panel_layer(); 
                hovered_category = -1;
                hovered_app = -1;
                update_menu_heights();
                if (configured) {
                    draw_frame();
                }
            }
        } else if (!in_cat) {
            menu_open = false;
            update_panel_layer();
            hovered_category = -1;
            hovered_app = -1;
            update_menu_heights();
            if (configured) {
                draw_frame();
            }
        }
    }
}

/**
 * @brief Tracks pointer motion to update hover states in the menu.
 */
static void pointer_motion(
    void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x,
    wl_fixed_t y) {
    double *pos = data;
    pos[0] = wl_fixed_to_double(x);
    pos[1] = wl_fixed_to_double(y);
    
    bool needs_redraw = false;

    if (menu_open && current_pointer_surface == surface) {
        double mx = pos[0];
        double my = pos[1];
        
        int cat_x = 8;
        int cat_y = current_height - PANEL_HEIGHT - cat_menu_height - 8;
        int app_x = cat_x + 180 + 8;
        int app_y = current_height - PANEL_HEIGHT - 
                    app_y_offset_from_bottom - 8;

        if (mx >= cat_x && mx < cat_x + 180 && 
            my >= cat_y && my < cat_y + cat_menu_height) {
            int idx = (my - cat_y - 5 + cat_scroll) / 30;
            if (idx >= 0 && idx < category_count) {
                if (hovered_category != idx) {
                    hovered_category = idx;
                    app_scroll = 0;
                    hovered_app = -1;
                    update_menu_heights();
                    needs_redraw = true;
                }
            }
            if (hovered_app != -1) {
                hovered_app = -1;
                needs_redraw = true;
            }
        } else if (hovered_category >= 0 && mx >= app_x && mx < app_x + 250 && 
                   my >= app_y && my < app_y + app_menu_height) {
            int idx = (my - app_y - 5 + app_scroll) / 30;
            if (idx >= 0 && idx < categories[hovered_category].app_count) {
                if (hovered_app != idx) {
                    hovered_app = idx;
                    needs_redraw = true;
                }
            } else if (hovered_app != -1) {
                hovered_app = -1;
                needs_redraw = true;
            }
        } else {
            if (hovered_app != -1) {
                hovered_app = -1;
                needs_redraw = true;
            }
        }
    }

    if (needs_redraw && configured) {
        draw_frame();
    }
}

/**
 * @brief Handles scroll wheel events to navigate menus.
 */
static void pointer_axis(
    void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
    wl_fixed_t value) {
    if (!menu_open || axis != 0 || current_pointer_surface != surface) {
        return;
    }
    
    double *pos = data;
    double mx = pos[0];
    double my = pos[1];
    double scroll_amt = wl_fixed_to_double(value);
   
    int cat_x = 8;
    int cat_y = current_height - PANEL_HEIGHT - cat_menu_height - 8;
    int app_x = cat_x + 180 + 8;
    int app_y = current_height - PANEL_HEIGHT - app_y_offset_from_bottom - 8;
    
    if (mx >= cat_x && mx < cat_x + 180 && 
        my >= cat_y && my < cat_y + cat_menu_height) { 
        cat_scroll += scroll_amt;
        int max_scroll = category_count * 30 + 10 - cat_menu_height;
        if (max_scroll < 0) max_scroll = 0;
        if (cat_scroll < 0) cat_scroll = 0;
        if (cat_scroll > max_scroll) cat_scroll = max_scroll;
        
        update_menu_heights();
        if (configured) {
            draw_frame();
        }
    }
    
    if (hovered_category >= 0 && mx >= app_x && mx < app_x + 250 && 
        my >= app_y && my < app_y + app_menu_height) {
        app_scroll += scroll_amt;
        int max_scroll = categories[hovered_category].app_count * 30 + 10 - 
                         app_menu_height;
        if (max_scroll < 0) max_scroll = 0;
        if (app_scroll < 0) app_scroll = 0;
        if (app_scroll > max_scroll) app_scroll = max_scroll;
        if (configured) {
            draw_frame();
        }
    }
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
    if (surf == bg_surface && last_clicked_desktop_index != -1) {
        last_clicked_desktop_index = -1;
        draw_bg();
    }
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
    if (w > 0) width = w;
    if (h > 0) current_height = h;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    configured = true;
    draw_frame();
}

static const struct zwlr_layer_surface_v1_listener layer_listener = { 
    .configure = layer_surface_configure 
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
 * @brief Application entry point. Daemonizes the process, sets up Wayland 
 * surfaces, initializes the event loop, and handles display dispatching.
 */
int main() {
    signal(SIGCHLD, SIG_IGN);
   
    if (access("/usr/share/selkies/www/icon.png", F_OK) == 0) {
        start_icon = cairo_image_surface_create_from_png(
            "/usr/share/selkies/www/icon.png");
    }

    load_apps();
    load_desktop_apps();

    display = wl_display_connect(NULL); 
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

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

    int inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    char watch_file[1024];
    const char *home_dir = getenv("HOME");
    if (home_dir) {
        snprintf(watch_file, sizeof(watch_file), "%s/.config/panel-reload", 
                 home_dir);
        int fd = open(watch_file, O_CREAT | O_RDWR, 0644);
        if (fd >= 0) close(fd);
        inotify_add_watch(inotify_fd, watch_file, IN_ATTRIB | IN_MODIFY);
    }

    int wl_fd = wl_display_get_fd(display);
    struct pollfd fds[2] = {
        { .fd = wl_fd, .events = POLLIN },
        { .fd = inotify_fd, .events = POLLIN }
    };

    while (1) {
        while (wl_display_prepare_read(display) != 0) {
            wl_display_dispatch_pending(display);
        }
        wl_display_flush(display);
        
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) {
                wl_display_cancel_read(display);
                continue;
            }
            wl_display_cancel_read(display);
            break;
        }
        
        if (fds[0].revents & POLLIN) {
            wl_display_read_events(display);
            wl_display_dispatch_pending(display);
        } else {
            wl_display_cancel_read(display);
        }
        
        if (fds[1].revents & POLLIN) {
            char buf[4096] __attribute__ ((aligned(
                __alignof__(struct inotify_event))));
            while (read(inotify_fd, buf, sizeof(buf)) > 0) {}
            
            for (int i = 0; i < app_count; i++) {
                if (apps[i].icon) cairo_surface_destroy(apps[i].icon);
            }
            for (int i = 0; i < desktop_app_count; i++) {
                if (desktop_apps[i].icon) {
                    cairo_surface_destroy(desktop_apps[i].icon);
                }
            }
            
            app_count = 0;
            desktop_app_count = 0;
            category_count = 0;
            
            load_apps();
            load_desktop_apps();
            
            if (configured) {
                draw_frame();
                draw_bg();
            }
        }
    }

    if (cursor_theme) {
        wl_cursor_theme_destroy(cursor_theme);
    }
    if (cursor_surface) {
        wl_surface_destroy(cursor_surface);
    }

    wl_display_disconnect(display);
    return 0;
}
