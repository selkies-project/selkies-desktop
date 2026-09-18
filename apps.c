#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <glob.h>
#include <cairo/cairo.h>
#include "desktop.h"
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "vendor/nanosvg.h"
#include "vendor/nanosvgrast.h"

App apps[MAX_APPS];
int app_count = 0;

App desktop_apps[MAX_APPS];
int desktop_app_count = 0;

Category categories[20];
int category_count = 0;

cairo_surface_t *start_icon = NULL;

#define MAX_SEEN_IDS (MAX_APPS * 2)

static char *seen_ids[MAX_SEEN_IDS];
static int seen_id_count = 0;

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
    unsigned char *img_data = malloc((size_t)size * size * 4);
    if (!rast || !img_data) {
        free(img_data);
        if (rast) nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return NULL;
    }

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
 * @brief Checks whether a desktop file ID was already claimed by a directory
 * of higher precedence, and claims it otherwise. This lets a file in the
 * user's data directory replace or hide the system file of the same name.
 *
 * @param id The desktop file ID, its file name within the applications directory.
 * @return bool True when the ID was already claimed and the file must be skipped.
 */
static bool claim_desktop_id(const char *id) {
    for (int i = 0; i < seen_id_count; i++) {
        if (strcmp(seen_ids[i], id) == 0) {
            return true;
        }
    }
    if (seen_id_count < MAX_SEEN_IDS) {
        char *copy = strdup(id);
        if (copy) {
            seen_ids[seen_id_count++] = copy;
        }
    }
    return false;
}

/**
 * @brief Checks whether the program named by a TryExec key is installed,
 * either as an absolute path or by searching PATH.
 *
 * @param program The value of the TryExec key.
 * @return bool True when an executable file was found.
 */
static bool program_exists(const char *program) {
    if (program[0] == '/') {
        return access(program, X_OK) == 0;
    }

    const char *path = getenv("PATH");
    if (!path || path[0] == '\0') {
        path = "/usr/local/bin:/usr/bin:/bin";
    }

    while (*path) {
        size_t dir_len = strcspn(path, ":");
        char candidate[1024];
        if (dir_len > 0) {
            snprintf(candidate, sizeof(candidate), "%.*s/%s",
                     (int)dir_len, path, program);
            if (access(candidate, X_OK) == 0) {
                return true;
            }
        }
        path += dir_len;
        if (*path == ':') path++;
    }
    return false;
}

/**
 * @brief Scans a specific directory for .desktop files and populates app lists.
 * Only keys of the [Desktop Entry] group are read, and an entry is left out
 * when it is not an application, sets NoDisplay or Hidden, or names a TryExec
 * program that is not installed. OnlyShowIn and NotShowIn are deliberately
 * ignored, since the panel is not a desktop environment of its own and images
 * remove the desktop files they do not want listed. Lines longer than the read
 * buffer arrive in pieces, and only the first piece of a line is parsed.
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
        if (claim_desktop_id(dir->d_name)) {
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
        bool in_entry_group = false;
        bool line_start = true;
        bool excluded = false;

        while (fgets(line, sizeof(line), f)) {
            bool first_piece = line_start;
            line_start = (strchr(line, '\n') != NULL);
            if (!first_piece) {
                continue;
            }
            line[strcspn(line, "\r\n")] = 0;

            if (line[0] == '[') {
                in_entry_group = (strcmp(line, "[Desktop Entry]") == 0);
                continue;
            }
            if (!in_entry_group) {
                continue;
            }

            if (strncmp(line, "NoDisplay=true", 14) == 0 ||
                strncmp(line, "Hidden=true", 11) == 0) {
                excluded = true;
            } else if (strncmp(line, "Type=", 5) == 0) {
                if (strcmp(line + 5, "Application") != 0) {
                    excluded = true;
                }
            } else if (strncmp(line, "TryExec=", 8) == 0) {
                if (line[8] != '\0' && !program_exists(line + 8)) {
                    excluded = true;
                }
            } else if (strncmp(line, "Name=", 5) == 0 && name[0] == '\0') {
                snprintf(name, sizeof(name), "%.127s", line + 5);
            } else if (strncmp(line, "Exec=", 5) == 0 && exec[0] == '\0') {
                snprintf(exec, sizeof(exec), "%.255s", line + 5);
            } else if (strncmp(line, "Icon=", 5) == 0 && icon_name[0] == '\0') {
                snprintf(icon_name, sizeof(icon_name), "%.127s", line + 5);
            } else if (strncmp(line, "Categories=", 11) == 0 &&
                       categories_str[0] == '\0') {
                snprintf(categories_str, sizeof(categories_str), "%.255s",
                         line + 11);
            }
        }
        fclose(f);

        if (!excluded && name[0] && exec[0]) {
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
 * @brief Orchestrates application loading from the XDG data directories in
 * order of precedence, the user's data home first and then each directory of
 * XDG_DATA_DIRS, so that the first file found for a desktop file ID wins.
 */
void load_apps() {
    char apps_path[1024];
    const char *data_home = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");

    if (data_home && data_home[0]) {
        snprintf(apps_path, sizeof(apps_path), "%s/applications", data_home);
        scan_app_dir(apps_path);
    } else if (home) {
        snprintf(apps_path, sizeof(apps_path),
                 "%s/.local/share/applications", home);
        scan_app_dir(apps_path);
    }

    const char *data_dirs = getenv("XDG_DATA_DIRS");
    if (!data_dirs || data_dirs[0] == '\0') {
        data_dirs = "/usr/local/share:/usr/share";
    }

    while (*data_dirs) {
        size_t dir_len = strcspn(data_dirs, ":");
        if (dir_len > 0) {
            snprintf(apps_path, sizeof(apps_path), "%.*s/applications",
                     (int)dir_len, data_dirs);
            scan_app_dir(apps_path);
        }
        data_dirs += dir_len;
        if (*data_dirs == ':') data_dirs++;
    }

    for (int i = 0; i < category_count; i++) {
        qsort(categories[i].apps, categories[i].app_count,
              sizeof(App*), compare_apps);
    }
    qsort(categories, category_count, sizeof(Category), compare_categories);
}

/**
 * @brief Releases every loaded application icon and clears the application,
 * desktop, category and claimed desktop file ID lists so they can be loaded
 * again.
 */
void unload_apps() {
    for (int i = 0; i < app_count; i++) {
        if (apps[i].icon) cairo_surface_destroy(apps[i].icon);
    }
    for (int i = 0; i < desktop_app_count; i++) {
        if (desktop_apps[i].icon) {
            cairo_surface_destroy(desktop_apps[i].icon);
        }
    }

    for (int i = 0; i < seen_id_count; i++) {
        free(seen_ids[i]);
    }

    app_count = 0;
    desktop_app_count = 0;
    category_count = 0;
    seen_id_count = 0;
}
