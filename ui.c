#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <cairo/cairo.h>
#include "desktop.h"

const Backend *backend = NULL;

Toplevel *toplevels_head = NULL;

int width = 1920;
int current_height = 4000;
int output_scale = 1;

bool menu_open = false;
bool configured = false;

int bg_width = 0;
int bg_height = 0;

int cat_menu_height = 0;
int app_menu_height = 0;
int app_y_offset_from_bottom = 0;

int hovered_category = -1;
int hovered_app = -1;

double cat_scroll = 0;
double app_scroll = 0;

uint32_t last_click_time = 0;
int last_clicked_desktop_index = -1;

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
 * @brief Reports the rectangles currently covered by the open start menu, the
 * category list first and the application list second when one is showing.
 *
 * @param rects Output array with room for two rectangles.
 * @return int Number of rectangles written, zero when the menu is closed.
 */
int get_menu_rects(Rect *rects) {
    if (!menu_open) {
        return 0;
    }

    rects[0].x = 2;
    rects[0].y = current_height - PANEL_HEIGHT - cat_menu_height - 4;
    rects[0].w = 180;
    rects[0].h = cat_menu_height;

    if (hovered_category < 0) {
        return 1;
    }

    rects[1].x = rects[0].x + 180 + 4;
    rects[1].y = current_height - PANEL_HEIGHT - app_y_offset_from_bottom - 4;
    rects[1].w = 250;
    rects[1].h = app_menu_height;
    return 2;
}

/**
 * @brief Computes the top-left corner of a desktop icon cell. Icons fill
 * columns top to bottom and wrap to the next column above the panel.
 *
 * @param index Index into the desktop application list.
 * @param x Output X coordinate of the cell.
 * @param y Output Y coordinate of the cell.
 */
void get_desktop_cell(int index, int *x, int *y) {
    int max_rows = (bg_height - PANEL_HEIGHT) / DESKTOP_CELL_HEIGHT;
    if (max_rows < 1) max_rows = 1;

    *x = (index / max_rows) * DESKTOP_CELL_WIDTH + 10;
    *y = (index % max_rows) * DESKTOP_CELL_HEIGHT + 10;
}

/**
 * @brief Allocates a toplevel and links it at the head of the window list.
 *
 * @return Toplevel* The new zeroed toplevel, or NULL on allocation failure.
 */
Toplevel* toplevel_create() {
    Toplevel *tl = calloc(1, sizeof(Toplevel));
    if (!tl) {
        return NULL;
    }
    tl->next = toplevels_head;
    toplevels_head = tl;
    return tl;
}

/**
 * @brief Unlinks a toplevel from the window list and frees it and its icon.
 *
 * @param tl The toplevel to remove.
 */
void toplevel_destroy(Toplevel *tl) {
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
    free(tl);
}

/**
 * @brief Stores the application ID of a toplevel and looks up its themed icon.
 *
 * @param tl The toplevel to update.
 * @param app_id The application ID or window class reported by the backend.
 */
void toplevel_set_app_id(Toplevel *tl, const char *app_id) {
    snprintf(tl->app_id, sizeof(tl->app_id), "%s", app_id);
    if (tl->icon) {
        cairo_surface_destroy(tl->icon);
    }
    tl->icon = get_icon(app_id, ICON_SIZE);
}

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
 * @brief Renders the background wallpaper and desktop icons, then hands the
 * result to the backend to present.
 */
void draw_bg() {
    if (bg_width == 0 || bg_height == 0) return;

    cairo_surface_t *cairo_surf = backend->begin_draw(
        SURFACE_BG, bg_width, bg_height, output_scale);
    if (!cairo_surf) return;

    int buffer_w = bg_width * output_scale;
    int buffer_h = bg_height * output_scale;

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
            snprintf(line1, sizeof(line1), "%s", name);
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
                    snprintf(line2, sizeof(line2), "%s", rem);
                } else {
                    for (int j = strlen(rem); j >= 0; j--) {
                        snprintf(temp, sizeof(temp), "%.*s...", j, rem);
                        cairo_text_extents(cr, temp, &extents);
                        if (extents.width <= max_w || j == 0) {
                            snprintf(line2, sizeof(line2), "%s", temp);
                            break;
                        }
                    }
                }
            } else {
                for (int j = len; j >= 0; j--) {
                    snprintf(temp, sizeof(temp), "%.*s...", j, name);
                    cairo_text_extents(cr, temp, &extents);
                    if (extents.width <= max_w || j == 0) {
                        snprintf(line1, sizeof(line1), "%s", temp);
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

    backend->commit(SURFACE_BG);
}

/**
 * @brief Records the size granted to the background surface and renders it.
 *
 * @param w Width in logical pixels.
 * @param h Height in logical pixels.
 */
void bg_configure(int w, int h) {
    if (w <= 0 || h <= 0) {
        return;
    }

    bg_width = w;
    bg_height = h;
    draw_bg();
}

/**
 * @brief Renders the main panel UI, including the start menu and window list,
 * then hands the result to the backend to present.
 */
void draw_frame() {
    cairo_surface_t *cairo_surf = backend->begin_draw(
        SURFACE_PANEL, width, current_height, output_scale);
    if (!cairo_surf) return;

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

    Rect menu_rects[2];
    int menu_rect_count = get_menu_rects(menu_rects);

    if (menu_rect_count > 0) {
        int cat_x = menu_rects[0].x;
        int cat_y = menu_rects[0].y;

        cairo_save(cr);
        rounded_rect(cr, cat_x, cat_y, 180, cat_menu_height, MENU_RADIUS);
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

        if (menu_rect_count > 1) {
            int app_x = menu_rects[1].x;
            int app_y = menu_rects[1].y;

            cairo_save(cr);
            rounded_rect(cr, app_x, app_y, 250, app_menu_height, MENU_RADIUS);
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

    cairo_destroy(cr);
    cairo_surface_destroy(cairo_surf);

    backend->commit(SURFACE_PANEL);
}

/**
 * @brief Records the size granted to the panel surface and renders it.
 *
 * @param w Width in logical pixels, ignored when not positive.
 * @param h Height in logical pixels, ignored when not positive.
 */
void panel_configure(int w, int h) {
    if (w > 0) width = w;
    if (h > 0) current_height = h;
    configured = true;
    draw_frame();
}

/**
 * @brief Launches a desktop file fully detached from the panel process. Uses
 * fallback execution strategies across the available launchers.
 *
 * @param d_path Path to the .desktop file to launch.
 */
static void launch_desktop_file(const char *d_path) {
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
            execlp("dex", "dex", d_path, NULL);
            execlp("gio", "gio", "launch", d_path, NULL);
            execlp("gtk-launch", "gtk-launch", d_path, NULL);
            exit(0);
        }
        exit(0);
    }
}

/**
 * @brief Closes the start menu, resets its hover state, and redraws the panel.
 */
static void close_menu() {
    menu_open = false;
    backend->update_panel_layer();
    hovered_category = -1;
    hovered_app = -1;
    update_menu_heights();
    if (configured) {
        draw_frame();
    }
}

/**
 * @brief Handles pointer button press events to manage desktop icon
 * selection and launching, menu interactions, and window state toggling.
 *
 * @param id The surface the pointer is over.
 * @param x Pointer X position in logical surface coordinates.
 * @param y Pointer Y position in logical surface coordinates.
 * @param time Event timestamp in milliseconds.
 */
void handle_pointer_button(SurfaceId id, double x, double y, uint32_t time) {
    if (id == SURFACE_BG) {
        int clicked_idx = -1;
        for (int i = 0; i < desktop_app_count; i++) {
            int cell_x, cell_y;
            get_desktop_cell(i, &cell_x, &cell_y);
            if (x >= cell_x && x < cell_x + DESKTOP_CELL_WIDTH &&
                y >= cell_y && y < cell_y + DESKTOP_CELL_HEIGHT) {
                clicked_idx = i;
                break;
            }
        }

        if (clicked_idx != -1) {
            if (time - last_click_time < 300 &&
                last_clicked_desktop_index == clicked_idx) {
                launch_desktop_file(desktop_apps[clicked_idx].desktop_path);
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
            close_menu();
        }
        return;
    }

    if (last_clicked_desktop_index != -1) {
        last_clicked_desktop_index = -1;
        draw_bg();
    }

    if (x < START_BTN_WIDTH && y > current_height - PANEL_HEIGHT) {
        menu_open = !menu_open;
        backend->update_panel_layer();
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
                        backend->toplevel_set_minimized(tl, true);
                    } else if (tl->minimized) {
                        backend->toplevel_set_minimized(tl, false);
                        backend->toplevel_activate(tl);
                    } else {
                        backend->toplevel_activate(tl);
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
                launch_desktop_file(
                    categories[hovered_category].apps[idx]->desktop_path);
                close_menu();
            }
        } else if (!in_cat) {
            close_menu();
        }
    }
}

/**
 * @brief Tracks pointer motion to update hover states in the menu.
 *
 * @param id The surface the pointer is over.
 * @param mx Pointer X position in logical surface coordinates.
 * @param my Pointer Y position in logical surface coordinates.
 */
void handle_pointer_motion(SurfaceId id, double mx, double my) {
    bool needs_redraw = false;

    if (menu_open && id == SURFACE_PANEL) {
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
 * @brief Handles vertical scroll events to navigate menus.
 *
 * @param id The surface the pointer is over.
 * @param mx Pointer X position in logical surface coordinates.
 * @param my Pointer Y position in logical surface coordinates.
 * @param scroll_amt Scroll distance, positive values scroll down.
 */
void handle_pointer_axis(
    SurfaceId id, double mx, double my, double scroll_amt) {
    if (!menu_open || id != SURFACE_PANEL) {
        return;
    }

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
 * @brief Handles the pointer leaving a surface, clearing the desktop icon
 * selection when it leaves the background.
 *
 * @param id The surface the pointer left.
 */
void handle_pointer_leave(SurfaceId id) {
    if (id == SURFACE_BG && last_clicked_desktop_index != -1) {
        last_clicked_desktop_index = -1;
        draw_bg();
    }
}
