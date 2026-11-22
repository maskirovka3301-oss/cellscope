#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "camera_platform.h"
#include "runtime.h"
#include "hfmodel.h"
#include "train.h"

#include <curl/curl.h>

static void console_runtime_progress(void *ctx, const PythonRuntime *rt)
{
    (void)ctx;
    static int last_stage = -1;
    static int last_pct = -1;
    if ((int)rt->stage != last_stage || rt->percent >= last_pct + 5 || rt->stage == RT_STAGE_ERROR) {
        last_stage = (int)rt->stage;
        last_pct = rt->percent;
        printf("[%3d%%] %s\n", rt->percent, rt->message);
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    App *app = calloc(1, sizeof(App));
    if (!app) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    char err[1024] = {0};
    /* A macOS LaunchServices relaunch drops argv; recover it before parsing. */
    camera_platform_take_relaunch_args(&argc, &argv);
    AppArgsResult ar = app_parse_args(app, argc, argv, err, sizeof(err));
    if (ar == APP_ARGS_HELP) {
        app_print_usage();
        free(app);
        return 0;
    }
    if (ar == APP_ARGS_VERSION) {
        free(app);
        return 0;
    }
    if (ar == APP_ARGS_ERROR) {
        fprintf(stderr, "CellScope: %s\n\n", err);
        app_print_usage();
        free(app);
        return 2;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (app->setup_runtime_mode) {
        PythonRuntime rt;
        char rerr[1024] = {0};
        if (!runtime_init(&rt, cs_project_root(), rerr, sizeof(rerr))) {
            fprintf(stderr, "Runtime setup: %s\n", rerr);
            free(app);
            return 2;
        }
        printf("CellScope local runtime\n  runtime:  %s\n  minimind: %s\n  training: %s\n",
               rt.dir, rt.minimind, rt.training);
        bool skip_models = getenv("CELLSCOPE_SKIP_MODELS") != NULL;
        bool ok = runtime_ensure(&rt, console_runtime_progress, NULL, !skip_models);
        printf("\n%s\n", ok ? "Runtime ready." : rt.error);
        free(app);
        curl_global_cleanup();
        return ok ? 0 : 3;
    }

    if (app->train_mode) {
        int rc = train_run(&app->train);
        free(app);
        curl_global_cleanup();
        return rc;
    }

    if (app->download_model[0]) {
        char derr[1024] = {0};
        int rc = hf_model_download(app->download_model, console_runtime_progress, NULL, derr,
                                   sizeof(derr));
        if (rc != 0) fprintf(stderr, "CellScope model download: %s\n", derr);
        free(app);
        curl_global_cleanup();
        return rc;
    }

    /* macOS TCC attributes a camera prompt to the responsible application.
     * A binary launched from a terminal, IDE, or another agent can silently
     * lose that prompt. Before the first decision, re-open ourselves through
     * LaunchServices so the prompt belongs to CellScope. The --relaunched
     * argument and environment guard prevent a loop. */
    if (!app->no_camera && !app->demo && !app->image_mode && !app->relaunched) {
        if (camera_platform_auto_relaunch_if_needed()) {
            fprintf(stderr, "CellScope: reopening through LaunchServices for camera permission\n");
            free(app);
            curl_global_cleanup();
            return 0;
        }
    }

    SDL_SetAppMetadata("CellScope", CELLSCOPE_VERSION, "com.cellscope.app");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_CAMERA | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        free(app);
        return 1;
    }

    /* Use a maximized normal window rather than native fullscreen so the
     * macOS/Windows/Linux title bar remains usable. */
    SDL_Window *win = SDL_CreateWindow("CellScope — Phone Cell + Meter Reader", 1440, 900,
                                       SDL_WINDOW_MAXIMIZED | SDL_WINDOW_RESIZABLE |
                                           SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) {
        fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        free(app);
        return 1;
    }
    SDL_SetWindowMinimumSize(win, 1180, 760);

    GpuRenderer *gpu = gpu_renderer_create(win, err, sizeof(err));
    if (!gpu) {
        fprintf(stderr, "Renderer creation failed: %s\n", err);
        SDL_DestroyWindow(win);
        SDL_Quit();
        free(app);
        return 1;
    }

    if (!app_init(app, win, gpu, err, sizeof(err))) {
        fprintf(stderr, "CellScope init failed: %s\n", err);
        app_shutdown(app);
        gpu_renderer_destroy(gpu);
        SDL_DestroyWindow(win);
        SDL_Quit();
        free(app);
        return 1;
    }

    int logical_w = 1440, logical_h = 900;

    bool running = true;
    bool screenshot_taken = false;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                running = false;
                break;
            }
            SDL_Event converted = e;
            int ww = logical_w, wh = logical_h;
            SDL_GetWindowSize(win, &ww, &wh);
            if (ww > 0 && wh > 0) {
                float sx = (float)logical_w / (float)ww;
                float sy = (float)logical_h / (float)wh;
                if (converted.type == SDL_EVENT_MOUSE_MOTION) {
                    converted.motion.x *= sx;
                    converted.motion.y *= sy;
                } else if (converted.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                           converted.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    converted.button.x *= sx;
                    converted.button.y *= sy;
                } else if (converted.type == SDL_EVENT_MOUSE_WHEEL) {
                    converted.wheel.mouse_x *= sx;
                    converted.wheel.mouse_y *= sy;
                }
            }
            app_handle_event(app, &converted);
        }
        if (!running) break;

        app_update(app);
        if (SDL_GetAtomicInt(&app->quit)) {
            running = false;
            break;
        }

        ui_begin_frame(&app->ui, logical_w, logical_h);
        app_draw(app);

        int64_t elapsed = cs_now_ms() - app->started_ms;
        if (app->screenshot_path[0] && !screenshot_taken &&
            elapsed >= app->screenshot_delay_ms) {
            app_save_window_png(app, app->screenshot_path);
            screenshot_taken = true;
            if (app->quit_after_screenshot) running = false;
        }
        if (app->run_ms > 0 && elapsed >= app->run_ms) running = false;

        ui_end_frame(&app->ui);

        if (running) SDL_Delay(8);
    }

    app_shutdown(app);
    gpu_renderer_destroy(gpu);
    SDL_DestroyWindow(win);
    SDL_Quit();
    curl_global_cleanup();
    free(app);
    return 0;
}
