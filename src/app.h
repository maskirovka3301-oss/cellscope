#ifndef CELLSCOPE_APP_H
#define CELLSCOPE_APP_H

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>

#include "adb.h"
#include "camera.h"
#include "anatomy_3d.h"
#include "meter.h"
#include "quality.h"
#include "runtime.h"
#include "server.h"
#include "settings.h"
#include "telephony.h"
#include "train.h"
#include "ui.h"
#include "vlm.h"

#define CELL_MAX 2048
#define METER_MAX 1024
#define BURST_MAX 12

typedef struct {
    uint8_t *rgb;
    int w, h;
    int64_t ts_ms;
    uint64_t seq;
    float score;
    float motion;
    FrameQuality quality;
    bool used;
} BurstFrame;

typedef struct {
    int64_t ts_ms;
    uint64_t seq;
    char time_text[20];
    uint64_t hz;
    double confidence;
    char raw[180];
    char backend[48];
    char path[CS_PATH_MAX];
    char shot_name[160];
    bool demo;
} MeterSample;

#define VIS_FRAME_MAX 512

typedef struct {
    double hz;
    double confidence;
    float frame_score;
    int64_t frame_ts_ms;   /* epoch ms when this frame appeared in the webcam */
    int64_t extracted_ms;  /* epoch ms when the VLM result arrived */
    uint8_t *rgb;
    int w, h;
    char raw[256];
} VisibilityFrame;

typedef struct {
    bool active;
    bool committed;
    int64_t started_ms;
    int total_readings;
    int readable_readings;
    int frame_count;
    int frame_cap;
    VisibilityFrame *frames;
} VisibilitySession;

typedef struct {
    bool active;
    bool ready;
    bool error;
    bool server_ready;
    RuntimeStage stage;
    int percent;
    char message[512];
    char detail[1024];
    char error_text[1024];
    char server_label[128];
} RuntimeView;

typedef struct {
    bool busy;
    bool visible;          /* best result is currently displayed */
    bool screen_visible;   /* VLM currently sees a device screen */
    bool live_valid;
    double live_hz;
    double live_confidence;
    int64_t live_ms;
    int64_t display_until_ms;
    char live_raw[256];
    int64_t last_attempt_ms;
    int64_t last_ok_ms;
    double hz;
    double confidence;
    int stable;
    char raw[256];
    char status[256];
    char backend[96];
    char detail[1024];
    char error[256];
    char capture_path[CS_PATH_MAX];
    VlmBackend kind;
    bool is_vlm;
    float best_score;
    int burst_count;
    int burst_best;
    int frame_w, frame_h;
    int attempts;
    int accepted;
} ReaderView;

typedef struct {
    int64_t ts_ms;
    char status[24];
    char archive[256];
    char message[512];
} UploadHistoryEntry;

typedef struct {
    char archive[CS_PATH_MAX];
    char sig[CS_PATH_MAX];
} UploadJob;

typedef enum {
    APP_TAB_RF = 0,
    APP_TAB_SYMPTOM = 1
} AppTab;

#define SYMPTOM_MAX_CARDS 64

typedef enum {
    SYMPTOM_CARD_REGION = 0,
    SYMPTOM_CARD_WHOLE_BODY = 1,
    SYMPTOM_CARD_NON_PHYSICAL = 2
} SymptomCardKind;

typedef struct {
    SymptomCardKind kind;
    int region;
    float point_x;
    float point_y;
    float marker_x;
    float marker_y;
    bool has_point;
    bool minimized;
} SymptomCard;

typedef struct App {
    /* options */
    bool train_mode;
    bool setup_runtime_mode;
    bool no_runtime;
    TrainConfig train;
    bool demo;
    bool demo_phone;
    bool readme_fixture;
    int readme_fixture_mode;
    bool autoplay;
    bool no_modal;
    bool image_mode;
    bool no_camera;
    bool relaunched;
    bool auto_connect;
    bool vlm_explicit;
    char vlm_mode[24];
    char vlm_url[256];
    char vlm_model[128];
    char image_path[CS_PATH_MAX];
    char capture_dir[CS_PATH_MAX];
    char datasets_dir[CS_PATH_MAX];
    char session_dir[CS_PATH_MAX];
    char session_name[64];
    char asset_dir[CS_PATH_MAX];
    char default_unit[8];
    int camera_index;
    bool camera_cli;
    char download_model[256];
    char adb_override[CS_PATH_MAX];
    char adb_path[CS_PATH_MAX];
    char screenshot_path[CS_PATH_MAX];
    char export_json_path[CS_PATH_MAX];
    int64_t screenshot_delay_ms;
    bool quit_after_screenshot;
    bool export_now;
    int64_t run_ms;

    /* runtime */
    SDL_Window *win;
    GpuRenderer *r;
    Anatomy3D *anatomy;
    char anatomy_model_path[CS_PATH_MAX];
    Ui ui;
    Camera cam;
    bool camera_ok;
    bool camera_permission_requested;
    int64_t last_camera_retry_ms;
    char camera_error[512];
    char project_root[CS_PATH_MAX];
    PythonRuntime runtime;
    MiniMindServer server;
    RuntimeView runtime_view;
    AppSettings settings;
    char local_reader_label[128];
    char local_reader_model[128];
    SDL_Thread *bootstrap_thread;
    bool runtime_enabled;
    bool retry_runtime;
    bool prefer_minimind;
    bool vlm_switched;
    bool runtime_toasted;
    uint64_t wifi_salt;
    char minimind_url[128];
    int ui_banner_h;
    uint8_t *img_rgb;
    int img_w, img_h;
    GpuTexture *img_tex;
    GpuTexture *preview_tex;
    char preview_path[CS_PATH_MAX];

    SDL_Mutex *mu;
    SDL_AtomicInt quit;
    SDL_Thread *adb_thread;
    SDL_Thread *vlm_thread;
    SDL_Thread *cloud_status_thread;

    CellSample cells[CELL_MAX];
    int cell_head;
    int cell_count;
    uint64_t cell_seq;
    MeterSample meters[METER_MAX];
    int meter_head;
    int meter_count;
    uint64_t meter_seq;
    AdbStatus adb;
    ReaderView reader;
    VisibilitySession visibility;
    bool ever_connected;
    bool modal_open;
    bool modal_checking;
    bool remove_meter_open;
    int remove_meter_index;
    bool settings_modal_open;
    bool upload_modal_open;
    bool model_modal_open;
    int model_count;
    char model_repos[16][256];
    char model_paths[16][CS_PATH_MAX];
    bool reload_runtime;
    int settings_focus;
    char settings_edit[9][CS_PATH_MAX];
    char settings_target[CS_PATH_MAX];
    char settings_error[256];
    bool want_check;
    bool background_retry;
    int64_t last_check_ms;
    int64_t last_poll_ms;
    int64_t last_poll_ok_ms;
    int poll_failures;
    char device_label[128];

    BurstFrame burst[BURST_MAX];
    uint8_t *latest_rgb;
    int latest_w, latest_h;
    int64_t latest_ts_ms;
    FrameQuality latest_quality;
    uint8_t *motion_sig;
    int motion_sig_n;
    bool have_motion_sig;
    int burst_count;
    int burst_next;
    uint64_t burst_seq;
    bool paused;
    int active_tab;
    int symptom_body_view;
    int symptom_zoom_region;
    int symptom_pending_region;
    int64_t symptom_pending_ms;
    float symptom_pending_x;
    float symptom_pending_y;
    int symptom_focus_card;
    int symptom_scroll;
    SymptomCard symptom_cards[SYMPTOM_MAX_CARDS];
    int symptom_card_count;
    bool anatomy_downloading;
    bool anatomy_ready;
    bool anatomy_failed;
    int anatomy_percent;
    char anatomy_status[256];
    SDL_Thread *anatomy_thread;
    bool session_active;
    bool session_write_failed;
    bool silent_export;
    char last_export_archive[CS_PATH_MAX];
    char last_export_sig[CS_PATH_MAX];
    char upload_status[512];
    UploadHistoryEntry upload_history[20];
    int upload_history_count;
    UploadJob upload_jobs[16];
    int upload_job_count;
    SDL_Thread *upload_thread;
    int64_t session_started_ms;
    uint64_t session_cell_start;
    uint64_t session_meter_start;

    VlmConfig vlm;
    MeterTracker tracker;

    float cell_scroll;
    float cell_scroll_x;
    bool cell_follow;
    float meter_scroll;
    float meter_scroll_x;
    bool meter_follow;
    int64_t selected_meter_ts;
    int selected_meter;

    char toast[640];
    int64_t toast_until_ms;
    bool toast_error;

    int64_t started_ms;
    int64_t last_snapshot_ms;
    int64_t last_camera_rgb_ms;
    int64_t last_demo_cell_ms;
    int64_t last_demo_meter_ms;
    int64_t last_probe_ms;
    int64_t last_preview_check_ms;
    int64_t last_dump_save_ms;
    bool auto_connect_done;
    char log_path[CS_PATH_MAX];
} App;

typedef enum {
    APP_ARGS_OK = 0,
    APP_ARGS_HELP,
    APP_ARGS_VERSION,
    APP_ARGS_ERROR
} AppArgsResult;

AppArgsResult app_parse_args(App *app, int argc, char **argv, char *err, size_t err_cap);
void app_print_usage(void);
bool app_init(App *app, SDL_Window *win, GpuRenderer *r, char *err, size_t err_cap);
void app_shutdown(App *app);
void app_update(App *app);
void app_draw(App *app);
void app_handle_event(App *app, const SDL_Event *e);
void app_request_check(App *app);
void app_retry_runtime(App *app);
void app_toggle_pause(App *app);
bool app_retry_camera(App *app, bool notify);
void app_export_dataset(App *app);
void app_set_tab(App *app, int tab);
int app_get_tab(App *app);
void app_notify(App *app, bool error, const char *fmt, ...);
void app_copy_diagnostics(App *app);
void app_save_window_png(App *app, const char *path);
void app_seed_demo(App *app);

/* helpers shared with app_ui.c */
int app_cell_count(App *app);
CellSample app_cell_at(App *app, int index);
int app_meter_count(App *app);
MeterSample app_meter_at(App *app, int index);
void app_add_cell(App *app, const CellSample *sample);
void app_add_meter(App *app, const MeterSample *sample);
void app_request_remove_meter(App *app, int index);
void app_confirm_remove_meter(App *app);
void app_cancel_remove_meter(App *app);
void app_open_settings_modal(App *app);
void app_close_settings_modal(App *app);
void app_save_settings_modal(App *app);
void app_open_upload_modal(App *app);
void app_close_upload_modal(App *app);
void app_upload_dataset(App *app);
void app_enroll_device(App *app);
void app_reset_identity(App *app);
void app_open_model_modal(App *app);
void app_close_model_modal(App *app);
void app_select_hf_model(App *app, int index);

#endif
