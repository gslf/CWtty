/*
 * stats.h - Training records and statistics, kept across sessions: the
 * best streaks (copy: one per level, send: one), answers, accuracy, time,
 * listens, sending speed, per-character results, the days practised and
 * the activity of the last weeks.
 *
 * Pure data, no I/O: the terminal app keeps the text form in a file, the
 * web app in the browser's storage. Days are local days since 1970-01-01
 * (0 = unknown), supplied by the interface.
 */
#ifndef CWTTY_STATS_H
#define CWTTY_STATS_H

#define STATS_LEVELS 4 /* copy levels, see settings_t.train_level */
#define STATS_DAYS 28  /* days of activity kept */
#define STATS_RANGES 128 /* runs of consecutive days practised kept */
#define STATS_TEXT_CAP 16384 /* the text form of one stats_t always fits */

enum { STATS_COPY = 0, STATS_SEND = 1 }; /* = TRAIN_COPY, TRAIN_SEND */

typedef struct {
    int value; /* best streak of correct answers, 0 = none yet */
    int wpm;   /* character speed it was set at */
    int day;   /* when */
} stats_record;

typedef struct {
    int sessions; /* sessions with at least one answer */
    int answers;  /* answers checked */
    int correct;
    int seconds;  /* time spent answering */
    int seen[128], miss[128]; /* per character of the targets */
} stats_dir;

typedef struct {
    stats_dir    dir[2]; /* STATS_COPY, STATS_SEND */
    stats_record copy_rec[STATS_LEVELS];
    stats_record send_rec;

    /* Copy: items end when copied or out of listens. */
    int copy_items, copy_listens, copy_first, copy_failed;

    /* Send: correct items with a measured speed. */
    int send_timed, send_wpm10, send_ratio10, send_best10; /* tenths */

    int koch; /* highest Koch lesson practised */

    /* Days practised: runs of consecutive days [from, to], the oldest
     * first. Past STATS_RANGES runs the oldest are folded into old_days and
     * old_best. The days, the streaks and the last day are derived from
     * them (stats_merge() needs the days themselves: two devices may have
     * practised on the same day). */
    int nranges;
    int range[STATS_RANGES][2];
    int old_days, old_best;
    int first_day, last_day, days, day_streak, best_day_streak;
    int act_day;          /* the day of act[0] */
    int act[STATS_DAYS];  /* answers on act_day - i */

    double ms_rest; /* time not yet counted in seconds (not saved) */
} stats_t;

void stats_reset(stats_t *s);

/* The record of a direction (and copy level, 1..STATS_LEVELS). */
const stats_record *stats_record_of(const stats_t *s, int dir, int level);

/* A checked answer, ms after the previous one. */
void stats_answer(stats_t *s, int dir, int ok, double ms, int day);

/* A character of a checked target, missed or not. */
void stats_char(stats_t *s, int dir, int ch, int missed);

/* The current streak: stores it and returns 1 when it beats the record. */
int stats_streak(stats_t *s, int dir, int level, int streak, int wpm, int day);

/* The daily streak as of today: 0 once a day was skipped. */
int stats_day_streak(const stats_t *s, int today);

/* Answers on today - ago (0 when not kept). */
int stats_activity(const stats_t *s, int today, int ago);

/* The characters with the most misses for their count (up to cap). */
int stats_weak(const stats_t *s, int dir, char *out, int cap);

/* Accuracy of a character in percent, -1 if never seen. */
int stats_char_pct(const stats_t *s, int dir, int ch);

/* Adds src to dst, as if both histories had happened in one place:
 * counts are summed, the days practised joined, the activity aligned and
 * the records, the Koch lesson and the fastest sending take the best. */
void stats_merge(stats_t *dst, const stats_t *src);

/* Raises the records, the Koch lesson and the fastest sending of s to
 * those of total, so that a new record is a record of all the devices. */
void stats_lift(stats_t *s, const stats_t *total);

/* Text form, "key=value" lines: the length, or -1 if cap is too small.
 * The parsers return the number of lines they recognised; text from before
 * the practice= line gets its days rebuilt from the activity and the
 * streak. */
int stats_format(const stats_t *s, char *out, int cap);
int stats_parse(stats_t *s, const char *text);
int stats_parse_n(stats_t *s, const char *text, int n);

/* An integer by its key in the text form ("copy_answers"), or NULL. */
int *stats_field(stats_t *s, const char *key);

/* Civil dates and day numbers. */
int  stats_day(int year, int month, int mday);
void stats_date(int day, int *year, int *month, int *mday);

#endif
