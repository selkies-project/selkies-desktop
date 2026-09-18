#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/inotify.h>
#include <errno.h>
#include <cairo/cairo.h>
#include "desktop.h"

/**
 * @brief Picks the display backend. Wayland is tried first because a labwc
 * session also exports DISPLAY for XWayland, where the X11 backend would have
 * no window manager to cooperate with. X11 is used when no Wayland compositor
 * with layer shell support can be reached.
 *
 * @return bool True when a backend initialized successfully.
 */
static bool select_backend() {
    const Backend *candidates[] = { &wayland_backend, &x11_backend, NULL };

    for (int i = 0; candidates[i] != NULL; i++) {
        backend = candidates[i];
        if (backend->init()) {
            return true;
        }
    }

    backend = NULL;
    return false;
}

/**
 * @brief Application entry point. Loads the application lists, brings up the
 * display backend, and runs the event loop over the display connection and
 * the reload trigger file.
 */
int main() {
    signal(SIGCHLD, SIG_IGN);

    if (access("/usr/share/selkies/www/icon.png", F_OK) == 0) {
        start_icon = cairo_image_surface_create_from_png(
            "/usr/share/selkies/www/icon.png");
    }

    load_apps();
    load_desktop_apps();

    if (!select_backend()) {
        fprintf(stderr, "selkies-desktop: no Wayland layer shell compositor "
                        "or X11 display available\n");
        return 1;
    }

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

    struct pollfd fds[2] = {
        { .fd = backend->get_fd(), .events = POLLIN },
        { .fd = inotify_fd, .events = POLLIN }
    };

    while (1) {
        backend->before_poll();

        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            bool interrupted = (errno == EINTR);
            backend->after_poll(false);
            if (interrupted) {
                continue;
            }
            break;
        }

        backend->after_poll(fds[0].revents & POLLIN);

        if (fds[0].revents & (POLLERR | POLLHUP)) {
            break;
        }

        if (fds[1].revents & POLLIN) {
            char buf[4096] __attribute__ ((aligned(
                __alignof__(struct inotify_event))));
            while (read(inotify_fd, buf, sizeof(buf)) > 0) {}

            unload_apps();
            load_apps();
            load_desktop_apps();

            if (configured) {
                draw_frame();
                draw_bg();
            }
        }
    }

    backend->shutdown();
    return 0;
}
