// -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
// (c) 2016 Henner Zeller <h.zeller@acm.org>
//
// timg - a terminal image viewer.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://gnu.org/licenses/gpl-2.0.txt>
//
// To compile this image viewer, first get image-magick development files
// $ sudo apt-get install libgraphicsmagick++-dev
#include "timg-version.h"
#include "terminal-canvas.h"
#include "timg-time.h"

#include "image-display.h"
#ifdef WITH_TIMG_VIDEO
#  include "video-display.h"
#endif

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <Magick++.h>

#ifndef TIMG_VERSION
#  define TIMG_VERSION "(unknown)"
#endif

using timg::Duration;
using timg::Time;

volatile sig_atomic_t interrupt_received = 0;
static void InterruptHandler(int signo) {
  interrupt_received = 1;
}

static int usage(const char *progname, int w, int h) {
#ifdef WITH_TIMG_VIDEO
    static constexpr char kFileType[] = "image/video";
#else
    static constexpr char kFileType[] = "image";
#endif
    fprintf(stderr, "usage: %s [options] <%s> [<%s>...]\n", progname,
            kFileType, kFileType);
    fprintf(stderr, "Options:\n"
            "\t-g<w>x<h>  : Output pixel geometry. Default from terminal %dx%d\n"
            "\t-w<seconds>: If multiple images given: Wait time between (default: 0.0).\n"
            "\t-a         : Switch off antialiasing (default: on)\n"
            "\t-T[<pre-crop>] : Trim: auto-crop away all same-color pixels around image.\n"
            "\t             The optional pre-crop is pixels to remove beforehand\n"
            "\t             to get rid of an uneven border.\n"
            "\t-W         : Scale to fit width of terminal (default: "
            "fit terminal width and height)\n"
            "\t-U         : Toggle Upscale. If an image is smaller than\n"
            "\t             the terminal size, scale it up to full size.\n"
            "\t-V         : This is a video, don't attempt to probe image deocding first\n"
            "\t             (useful, if you stream from stdin).\n"
            "\t-b<str>    : Background color to use on transparent images (default '').\n"
            "\t-B<str>    : Checkerboard pattern color to use on transparent images (default '').\n"
            // used -C before to clear screen, but that is really a small
            // feature that should not request a toplevel option. Let's make
            // this --clear once we use long options, and re-use -C for
            // centering.
            //"\t-C         : Clear screen before showing images.\n"
            "\t-C         : Center image horizontally.\n"
            "\t-F         : Print filename before showing images.\n"
            "\t-E         : Don't hide the cursor while showing images.\n"
            "\t-v         : Print version and exit.\n"

            "\n  Scrolling\n"
            "\t-s[<ms>]   : Scroll horizontally (optionally: delay ms (60)).\n"
            "\t-d<dx:dy>  : delta x and delta y when scrolling (default: 1:0).\n"

            "\n  For Animations and Scrolling\n"
            "\t-t<seconds>: Stop after this time.\n"
            "\t-c<num>    : Number of runs through a full cycle.\n"
            "\t-f<num>    : Only animation: number of frames to render.\n"

            "\nIf both -c and -t are given, whatever comes first stops.\n"
            "If both -w and -t are given for some animation/scroll, -t "
            "takes precedence\n",
            w, h);
    return 1;
}

static bool GetBoolenEnv(const char *env_name) {
    const char *const value = getenv(env_name);
    return value && atoi(value) != 0;
}

int main(int argc, char *argv[]) {
    Magick::InitializeMagick(*argv);

    struct winsize w = {};
    const bool winsize_success = (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0);
    const int term_width = w.ws_col;
    const int term_height = 2 * (w.ws_row-1);  // double number of pixels high.
    bool terminal_use_upper_block = GetBoolenEnv("TIMG_USE_UPPER_BLOCK");

    timg::DisplayOptions display_opts;
    bool do_scroll = false;
    bool do_clear = false;

    bool show_filename = false;
    bool hide_cursor = true;
    int width = term_width;
    int height = term_height;
    int max_frames = -1;
    const char *bg_color = nullptr;
    const char *pattern_color = nullptr;
    Duration duration = Duration::InfiniteFuture();
    Duration between_images_duration = Duration::Millis(0);
    Duration scroll_delay = Duration::Millis(50);
    int loops  = -1;
    int dx = 1;
    int dy = 0;
    bool fit_width = false;
    bool do_image_loading = true;

    int opt;
    while ((opt = getopt(argc, argv, "vg:s::w:t:c:f:b:B:T::hCFEd:UWaV"))!=-1) {
        switch (opt) {
        case 'g':
            if (sscanf(optarg, "%dx%d", &width, &height) < 2) {
                fprintf(stderr, "Invalid size spec '%s'", optarg);
                return usage(argv[0], term_width, term_height);
            }
            break;
        case 'w':
            between_images_duration
                = Duration::Millis(roundf(atof(optarg) * 1000.0f));
            break;
        case 't':
            duration = Duration::Millis(roundf(atof(optarg) * 1000.0f));
            break;
        case 'c':
            loops = atoi(optarg);
            break;
        case 'f':
            max_frames = atoi(optarg);
            break;
        case 'a':
            display_opts.antialias = false;
            break;
        case 'b':
            bg_color = strdup(optarg);
            break;
        case 'B':
            pattern_color = strdup(optarg);
            break;
        case 's':
            do_scroll = true;
            if (optarg != NULL) {
                scroll_delay = Duration::Millis(atoi(optarg));
            }
            break;
        case 'V':
#ifdef WITH_TIMG_VIDEO
            do_image_loading = false;
#else
            fprintf(stderr, "-V: Video support not compiled in\n");
#endif
            break;
        case 'd':
            if (sscanf(optarg, "%d:%d", &dx, &dy) < 1) {
                fprintf(stderr, "-d%s: At least dx paramter needed e.g. -d1."
                        "Or you can give dx, dy like so: -d1:-1", optarg);
                return usage(argv[0], term_width, term_height);
            }
            break;
        case 'C':
            // used to be do_clear. Needs to find a new home in --clear
            display_opts.center_horizontally = true;
            break;
        case 'U':
            display_opts.upscale = !display_opts.upscale;
            break;
        case 'T':
            display_opts.auto_trim_image = true;
            if (optarg) {
                display_opts.crop_border = atoi(optarg);
            }
            break;
        case 'F':
            show_filename = !show_filename;
            break;
        case 'E':
            hide_cursor = false;
            break;
        case 'W':
            fit_width = true;
            break;
        case 'v':
            fprintf(stderr, "timg " TIMG_VERSION
                    " <https://github.com/hzeller/timg>\n"
                    "Copyright (c) 2016.. Henner Zeller. "
                    "This program is free software; license GPL 2.0.\n\n");
            fprintf(stderr, "Image decoding %s\n",
                    timg::ImageLoader::VersionInfo());
#ifdef WITH_TIMG_VIDEO
            fprintf(stderr, "Video decoding %s\n",
                    timg::VideoLoader::VersionInfo());
#endif
            return 0;
        case 'h':
        default:
            return usage(argv[0], term_width, term_height);
        }
    }

    if (width < 1 || height < 1) {
        if (!winsize_success || term_height < 0 || term_width < 0) {
            fprintf(stderr, "Failed to read size from terminal; "
                    "Please supply -g<width>x<height> directly.\n");
        } else {
            fprintf(stderr, "%dx%d is a rather unusual size\n", width, height);
        }
        return usage(argv[0], term_width, term_height);
    }

    if (optind >= argc) {
        fprintf(stderr, "Expected image filename.\n");
        return usage(argv[0], term_width, term_height);
    }

    // There is no scroll if there is no movement.
    if (dx == 0 && dy == 0) {
        fprintf(stderr, "Scrolling chosen, but dx:dy = 0:0. "
                "Just showing image, no scroll.\n");
        do_scroll = false;
    }

    // If we scroll in one direction (so have 'infinite' space) we want fill
    // the available screen space fully in the other direction.
    display_opts.fill_width  = fit_width || (do_scroll && dy != 0);
    display_opts.fill_height = do_scroll && dx != 0; // scroll hor, fill vert
    int exit_code = 0;

    signal(SIGTERM, InterruptHandler);
    signal(SIGINT, InterruptHandler);

    timg::TerminalCanvas canvas(STDOUT_FILENO, terminal_use_upper_block);
    if (hide_cursor) {
        canvas.CursorOff();
    }

    for (int imgarg = optind; imgarg < argc && !interrupt_received; ++imgarg) {
        const char *filename = argv[imgarg];
        if (do_clear) canvas.ClearScreen();
        if (show_filename) {
            printf("%s\n", filename);
        }

        if (do_image_loading) {
            timg::ImageLoader image_loader;
            if (image_loader.LoadAndScale(filename, width, height,
                                          display_opts,
                                          bg_color, pattern_color)) {
                if (do_scroll) {
                    image_loader.Scroll(duration, loops, interrupt_received,
                                        dx, dy, scroll_delay, &canvas);
                } else {
                    image_loader.Display(duration, max_frames, loops,
                                         interrupt_received, &canvas);
                }
                if (!image_loader.is_animation()) {
                    (Time::Now() + between_images_duration).WaitUntil();
                }
                continue;
            }
        }

#ifdef WITH_TIMG_VIDEO
        timg::VideoLoader video_loader;
        if (video_loader.LoadAndScale(filename, width, height, display_opts)) {
            video_loader.Play(duration, interrupt_received, &canvas);
            continue;
        }
#endif

        // We either loaded, played and continue'ed, or we end up here.
        fprintf(stderr, "%s: couldn't load\n", filename);
        exit_code = 1;
#ifdef WITH_TIMG_VIDEO
        if (strcmp(filename, "-") == 0 || strcmp(filename, "/dev/stdin") == 0) {
            fprintf(stderr, "If this is a video on stdin, use '-V' to "
                    "skip image probing\n");
        }
#endif
    }

    if (hide_cursor) {
        canvas.CursorOn();
    }
    if (interrupt_received)   // Make 'Ctrl-C' appear on new line.
        printf("\n");

    return exit_code;
}
