/*
 * app.h - Application state shared by the screens (modes) and the modal
 * windows, plus the services they use.
 */
#ifndef CWTTY_APP_H
#define CWTTY_APP_H

#include "audio.h"
#include "decoder.h"
#include "dsp.h"
#include "input.h"
#include "net.h"
#include "over.h"
#include "settings.h"
#include "statsbook.h"
#include "training.h"
#include "transcript.h"
#include "ui.h"

typedef enum { MODE_HOME, MODE_TX, MODE_RX, MODE_TRX, MODE_TRAIN, MODE_STATS, MODE_NET, MODE_COUNT } app_mode;

typedef enum {
    MODAL_NONE,
    MODAL_MENU,
    MODAL_TABLE,
    MODAL_SETTINGS,
    MODAL_HELP,
    MODAL_JOIN,
    MODAL_LEAVE,
    MODAL_IMPORT,
    MODAL_CLOUD
} modal_kind;

/* MODAL_LEAVE: what follows once leaving the room is confirmed. */
enum { LEAVE_QUIT = -1 };

/* ------------------------------------------------------------------------ */
/* Application                                                               */
/* ------------------------------------------------------------------------ */

#define MAX_DEVICES 24

typedef struct {
    int  force_legacy_keys;
    int  start_mode;
    int  demo; /* feed the receiver with a simulated band */
    char config_override[1024];
} app_options;

typedef struct app {
    settings_t cfg;
    char       cfg_path[1024];
    int        have_cfg_path;

    int mode;
    int modals[4];
    int nmodal;

    /* Modal window state. */
    int  menu_sel;
    int  set_sel;
    int  set_scroll;
    int  set_dirty;
    int  set_audio_dirty;
    char dev_out[MAX_DEVICES][AUDIO_NAME_CAP];
    int  n_out;
    char dev_in[MAX_DEVICES][AUDIO_NAME_CAP];
    int  n_in;
    int  table_scroll;
    int  help_scroll;

    /* Keyboard. */
    const kbd_info *kbd;
    int             key_left, key_right, key_space; /* physical state */
    int             key_usb; /* USB straight key: mouse buttons held (bit 0 left, bit 1 right) */

    audio_status audio;
    double       audio_status_at;

    /* Transmit side: operator keying decoded back to text. */
    cw_decoder txdec;
    timeline   tx_tl;
    transcript tx_log;
    char       tx_last[8];
    char       tx_last_code[12];
    double     tx_last_at;

    /* Receive side. */
    rx_dsp    *dsp;
    cw_decoder rxdec;
    timeline   rx_tl;
    transcript rx_log;
    char       rx_last[8];
    char       rx_last_code[12];
    double     rx_last_at;
    int        rx_on;
    double     rx_last_char_ms; /* receiver clock of the last character */
    int        demo;            /* simulated band instead of the input */
    double     demo_at;

    /* Transceiver. */
    transcript trx_log;
    int        ptt;
    double     ptt_at; /* wall clock of the last PTT change */

    training_t tr;

    /* Online room. */
    net_t     *net; /* NULL if the network could not be set up */
    transcript net_log;
    timeline   net_tl[ROOM_STATIONS]; /* each station's keying, room clock */
    double     net_heard[ROOM_STATIONS];  /* its line last grew (0: no open line) */
    double     net_active[ROOM_STATIONS]; /* its key last moved */
    over_t     net_over[ROOM_STATIONS];   /* where its transmission ends */
    double     net_tx_heard, net_tx_active; /* the same for the operator */
    over_t     net_tx_over;
    int        net_up; /* brokers carrying the room, last seen */
    char       join_call[ROOM_CALL_CAP];
    char       join_room[ROOM_NAME_CAP];
    char       join_pass[64];
    int        join_sel;
    char       join_error[80];
    int        leave_to; /* a mode, or LEAVE_QUIT */
    int        leave_ok; /* confirmed: go on */

    /* Training records and statistics, saved after every answer: this
     * device's section of the book is the training's, the total is shown. */
    statsbook_t book;
    char    stats_path[1024];
    int     have_stats_path;
    int     stats_error; /* saving failed: told once */
    int     stats_scroll;
    char    export_path[1024]; /* the last export, offered to import */
    char    import_path[1024];
    int     import_stage; /* 0 the file, 1 confirm */
    char    import_error[80];

    /* The statistics on Filen (cloud.h): a sync is due at cloud_due (0
     * none), after a change settles or when asked. */
    double  cloud_due, cloud_last;
    int     cloud_failed; /* the last sync failed */
    char    cloud_mail[256];
    char    cloud_pass[256]; /* wiped once sent and on closing */
    char    cloud_code[16];
    int     cloud_sel;
    int     cloud_confirm; /* asking before signing out */
    int     today; /* local day number (stats.h) */
    double  today_at;

    double now; /* wall clock, ms */
    double started;
    int    quit;
    char   toast[160];
    int    toast_warn;
    double toast_until;
} app_t;

/* Lifecycle, called from main(). */
app_t *app_create(const app_options *opt);
void   app_destroy(app_t *a);
void   app_handle_key(app_t *a, const key_event *ev);
void   app_update(app_t *a);
void   app_render(app_t *a);

/* Services for screens and modals. Both ask first while in a room. */
void app_set_mode(app_t *a, int mode);
void app_quit(app_t *a);
void app_open_modal(app_t *a, int kind);
void app_close_modal(app_t *a);
int  app_top_modal(const app_t *a);
int  app_has_modal(const app_t *a, int kind);
void app_apply_settings(app_t *a);
void app_save_settings(app_t *a);
void app_save_stats(app_t *a);
void app_stats_changed(app_t *a); /* the training counted: save */
void app_export_stats(app_t *a);  /* to ~/cwtty-stats-<date>.txt */

/* A path as typed: a leading ~ is the home directory. 0 on success. */
int app_expand_path(const char *in, char *out, size_t cap);
void app_reopen_audio(app_t *a);
void app_toast(app_t *a, int warn, const char *fmt, ...);
void app_release_keys(app_t *a);
void app_refresh_operator(app_t *a);
int  app_blink(const app_t *a, int period_ms);
/* Keying is accepted (Space and arrows key the transmitter). */
int app_keying_allowed(const app_t *a);

/* ------------------------------------------------------------------------ */
/* Screens (one file each)                                                   */
/* ------------------------------------------------------------------------ */

void home_render(app_t *a, rect_t r);
int  home_key(app_t *a, const key_event *ev);

void tx_render(app_t *a, rect_t r);
int  tx_key(app_t *a, const key_event *ev);
/* The keying panel (tape, live pattern, keycaps), shared with training. */
void keying_panel(app_t *a, rect_t r);

void rx_render(app_t *a, rect_t r);
int  rx_key(app_t *a, const key_event *ev);
/* Receiver controls shared by RX and TX/RX. */
int  rx_controls_key(app_t *a, const key_event *ev);
void rx_status_line(app_t *a, int x, int y, int w, uint32_t bg);
void rx_meter(app_t *a, int x, int y, int w, uint32_t bg);
/* Waterfall with its frequency axis (h >= 3). */
void rx_waterfall(app_t *a, rect_t r);

void trx_enter(app_t *a);
void trx_leave(app_t *a);
void trx_update(app_t *a);
void trx_render(app_t *a, rect_t r);
int  trx_key(app_t *a, const key_event *ev);
void trx_operator_pressed(app_t *a);

void train_enter(app_t *a);
void train_leave(app_t *a);
void train_render(app_t *a, rect_t r);
int  train_key(app_t *a, const key_event *ev);
void train_on_sent(app_t *a, const dec_token *tok);
void train_operator_pressed(app_t *a);
int  train_wants_keying(const app_t *a);

void stats_render(app_t *a, rect_t r);
int  stats_key(app_t *a, const key_event *ev);

void online_enter(app_t *a);
void online_leave(app_t *a);
void online_update(app_t *a);
void online_render(app_t *a, rect_t r);
int  online_key(app_t *a, const key_event *ev);
/* A station's decoded text (s < 0: the operator's) for the room's log. */
void online_text(app_t *a, int s, const dec_token *tok);
/* The join window: enters the room typed there. Returns 0 on success. */
int online_join(app_t *a);

/* Modal windows. */
void modal_render(app_t *a, int kind);
int  modal_key(app_t *a, int kind, const key_event *ev);
/* The pattern being keyed or received right now (for the table). */
const char *app_live_pattern(const app_t *a);

#endif
