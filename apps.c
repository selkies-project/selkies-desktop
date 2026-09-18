#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
 * @brief One XPM colour table entry, the pixel characters packed into an
 * integer so the table can be sorted and binary searched per pixel.
 */
typedef struct {
    uint32_t key;
    uint32_t argb;
} XpmColor;

/**
 * @brief Orders XPM colour table entries by their packed pixel characters.
 *
 * @param a First entry.
 * @param b Second entry.
 * @return int Negative, zero or positive as qsort and bsearch expect.
 */
static int xpm_color_cmp(const void *a, const void *b) {
    uint32_t ka = ((const XpmColor *)a)->key;
    uint32_t kb = ((const XpmColor *)b)->key;
    return (ka > kb) - (ka < kb);
}

/**
 * @brief Packs the characters of one XPM pixel into a single integer.
 *
 * @param chars Pointer to the pixel characters.
 * @param cpp Characters per pixel, at most four.
 * @return uint32_t The packed key.
 */
static uint32_t xpm_pack_key(const char *chars, int cpp) {
    uint32_t key = 0;
    for (int i = 0; i < cpp; i++) {
        key = (key << 8) | (unsigned char)chars[i];
    }
    return key;
}

/**
 * @brief Returns the next C string literal in an XPM buffer, skipping block
 * comments, and terminates it in place so no copy is needed.
 *
 * @param cursor Read position, advanced past the returned string.
 * @return char* Start of the string contents, or NULL when none remain.
 */
static char* xpm_next_string(char **cursor) {
    char *p = *cursor;
    while (*p) {
        if (p[0] == '/' && p[1] == '*') {
            char *end = strstr(p + 2, "*/");
            if (!end) {
                break;
            }
            p = end + 2;
        } else if (*p == '"') {
            char *start = ++p;
            while (*p && *p != '"') {
                p++;
            }
            if (!*p) {
                break;
            }
            *p = '\0';
            *cursor = p + 1;
            return start;
        } else {
            p++;
        }
    }
    *cursor = p + strlen(p);
    return NULL;
}

/**
 * @brief Ranks an XPM colour key so the colour visual wins over the grey and
 * mono ones when a table line carries several.
 *
 * @param token A whitespace separated token from a colour line.
 * @return int The rank of the key, or -1 if the token is not a key.
 */
static int xpm_key_rank(const char *token) {
    if (strcmp(token, "c") == 0) return 4;
    if (strcmp(token, "g") == 0) return 3;
    if (strcmp(token, "g4") == 0) return 2;
    if (strcmp(token, "m") == 0) return 1;
    if (strcmp(token, "s") == 0) return 0;
    return -1;
}

/**
 * @brief Resolves an XPM colour value to a premultiplied ARGB pixel. Handles
 * None, hex values of one to four digits per channel, a few common names and
 * then the X11 rgb.txt database when it is installed. Unknown names fall back
 * to opaque black.
 *
 * @param value The colour value text.
 * @return uint32_t The ARGB pixel.
 */
static uint32_t xpm_resolve_color(const char *value) {
    if (strcasecmp(value, "none") == 0) {
        return 0;
    }

    if (value[0] == '#') {
        size_t digits = strlen(value + 1) / 3;
        if (digits >= 1 && digits <= 4 && strlen(value + 1) == digits * 3) {
            uint32_t channels[3];
            for (int i = 0; i < 3; i++) {
                char part[5] = {0};
                memcpy(part, value + 1 + i * digits, digits);
                uint32_t v = (uint32_t)strtoul(part, NULL, 16);
                channels[i] = digits == 1 ? v * 17 : v >> (4 * (digits - 2));
            }
            return 0xFF000000u | (channels[0] << 16) | (channels[1] << 8) |
                   channels[2];
        }
        return 0xFF000000u;
    }

    static const struct { const char *name; uint32_t rgb; } named[] = {
        {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000},
        {"green", 0x00FF00}, {"blue", 0x0000FF}, {"yellow", 0xFFFF00},
        {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF}, {"gray", 0xBEBEBE},
        {"grey", 0xBEBEBE}, {NULL, 0}
    };
    for (int i = 0; named[i].name != NULL; i++) {
        if (strcasecmp(value, named[i].name) == 0) {
            return 0xFF000000u | named[i].rgb;
        }
    }

    uint32_t argb = 0xFF000000u;
    FILE *fp = fopen("/usr/share/X11/rgb.txt", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            unsigned int r, g, b;
            char rgb_name[128];
            if (sscanf(line, "%u %u %u %127[^\n]", &r, &g, &b,
                       rgb_name) == 4 && strcasecmp(rgb_name, value) == 0) {
                argb |= (r & 0xFF) << 16 | (g & 0xFF) << 8 | (b & 0xFF);
                break;
            }
        }
        fclose(fp);
    }
    return argb;
}

/**
 * @brief Parses the part of an XPM colour line that follows the pixel
 * characters. A line is a run of key and value pairs where a value may span
 * several words, so tokens are gathered until the next key and the value of
 * the highest ranked key is the one resolved.
 *
 * @param spec The mutable colour line text after the pixel characters.
 * @return uint32_t The ARGB pixel.
 */
static uint32_t xpm_parse_color(char *spec) {
    char value[64] = {0};
    char best[64] = {0};
    int rank = -1;
    int best_rank = -1;
    char *save = NULL;
    char *token = strtok_r(spec, " \t", &save);

    for (;;) {
        int token_rank = token ? xpm_key_rank(token) : 0;
        if (!token || token_rank >= 0) {
            if (rank > best_rank && value[0] != '\0') {
                snprintf(best, sizeof(best), "%s", value);
                best_rank = rank;
            }
            if (!token) {
                break;
            }
            rank = token_rank;
            value[0] = '\0';
        } else {
            size_t used = strlen(value);
            snprintf(value + used, sizeof(value) - used, "%s%s",
                     used ? " " : "", token);
        }
        token = strtok_r(NULL, " \t", &save);
    }

    return best_rank > 0 ? xpm_resolve_color(best) : 0xFF000000u;
}

/**
 * @brief Loads an XPM file into a Cairo surface at its native size. The file
 * is read whole and walked as a sequence of string literals: the header, the
 * colour table, then one string per pixel row. Pixels whose characters are
 * missing from the table are left transparent.
 *
 * @param filepath Path to the XPM file.
 * @return cairo_surface_t* The decoded image surface, or NULL on failure.
 */
static cairo_surface_t* load_xpm_as_cairo_surface(const char *filepath) {
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        return NULL;
    }

    char *buf = NULL;
    XpmColor *colors = NULL;
    cairo_surface_t *surf = NULL;
    bool ok = false;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    rewind(fp);
    if (len <= 0 || len > 16 * 1024 * 1024) {
        goto done;
    }
    buf = malloc((size_t)len + 1);
    if (!buf || fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        goto done;
    }
    buf[len] = '\0';

    char *cursor = buf;
    char *header = xpm_next_string(&cursor);
    int w, h, ncolors, cpp;
    if (!header || sscanf(header, "%d %d %d %d", &w, &h, &ncolors,
                          &cpp) != 4 ||
        w < 1 || w > 2048 || h < 1 || h > 2048 ||
        ncolors < 1 || ncolors > 65536 || cpp < 1 || cpp > 4) {
        goto done;
    }

    colors = malloc((size_t)ncolors * sizeof(XpmColor));
    if (!colors) {
        goto done;
    }
    for (int i = 0; i < ncolors; i++) {
        char *line = xpm_next_string(&cursor);
        if (!line || strlen(line) < (size_t)cpp) {
            goto done;
        }
        colors[i].key = xpm_pack_key(line, cpp);
        colors[i].argb = xpm_parse_color(line + cpp);
    }
    qsort(colors, (size_t)ncolors, sizeof(XpmColor), xpm_color_cmp);

    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        goto done;
    }
    cairo_surface_flush(surf);
    unsigned char *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);

    for (int y = 0; y < h; y++) {
        char *row = xpm_next_string(&cursor);
        if (!row || strlen(row) < (size_t)w * cpp) {
            goto done;
        }
        uint32_t *dst = (uint32_t *)(void *)(data + (size_t)y * stride);
        for (int x = 0; x < w; x++) {
            XpmColor probe = { xpm_pack_key(row + (size_t)x * cpp, cpp), 0 };
            XpmColor *hit = bsearch(&probe, colors, (size_t)ncolors,
                                    sizeof(XpmColor), xpm_color_cmp);
            dst[x] = hit ? hit->argb : 0;
        }
    }
    cairo_surface_mark_dirty(surf);
    ok = true;

done:
    fclose(fp);
    free(buf);
    free(colors);
    if (!ok && surf) {
        cairo_surface_destroy(surf);
        surf = NULL;
    }
    return surf;
}

/**
 * @brief Loads an icon file with the decoder its extension calls for, PNG
 * being the default.
 *
 * @param filepath Path to the icon file.
 * @param size Target size, used by the SVG rasterizer only.
 * @return cairo_surface_t* The loaded surface, or NULL on failure.
 */
static cairo_surface_t* load_icon_file(const char *filepath, int size) {
    const char *ext = strrchr(filepath, '.');
    if (ext && strcasecmp(ext, ".svg") == 0) {
        return load_svg_as_cairo_surface(filepath, size);
    }
    if (ext && strcasecmp(ext, ".xpm") == 0) {
        return load_xpm_as_cairo_surface(filepath);
    }
    cairo_surface_t *s = cairo_image_surface_create_from_png(filepath);
    if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) {
        return s;
    }
    cairo_surface_destroy(s);
    return NULL;
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
            cairo_surface_t *s = load_icon_file(name, size);
            if (s) {
                return s;
            }
        }
        const char *exts[] = { "png", "svg", "xpm", NULL };
        char path_with_ext[1024];
        for (int e = 0; exts[e] != NULL; e++) {
            snprintf(path_with_ext, sizeof(path_with_ext), "%s.%s", name,
                     exts[e]);
            if (access(path_with_ext, F_OK) == 0) {
                cairo_surface_t *s = load_icon_file(path_with_ext, size);
                if (s) {
                    return s;
                }
            }
        }
        return NULL;
    }

    const char *name_ext = strrchr(name, '.');
    if (name_ext && (strcasecmp(name_ext, ".png") == 0 ||
                     strcasecmp(name_ext, ".svg") == 0 ||
                     strcasecmp(name_ext, ".xpm") == 0)) {
        char base[128];
        snprintf(base, sizeof(base), "%.*s", (int)(name_ext - name), name);
        return get_icon(base, size);
    }

    const char *formats[] = { "svg", "png", "xpm", NULL };
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
                        cairo_surface_t *surf = load_icon_file(
                            g.gl_pathv[j], size);
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
                            cairo_surface_t *surf = load_icon_file(
                                g.gl_pathv[j], size);
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
                cairo_surface_t *surf = load_icon_file(g.gl_pathv[j], size);
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
