#include "progress.h"
#include "time_ops.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#include <sys/ioctl.h>
#endif

static ProgressBar bars[MAX_PROGRESS_BARS];

/* Minimum ms between re-renders. */
#define RENDER_INTERVAL_MS 50

int progress_term_width(void) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 80;
#else
    struct winsize ws;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
#endif
}

static void format_time(int64_t seconds, char *buf, size_t buflen) {
    if (seconds < 0) seconds = 0;
    int64_t m = seconds / 60;
    int64_t s = seconds % 60;
    snprintf(buf, buflen, "%02d:%02d", (int)m, (int)s);
}

static void render_bar(ProgressBar *bar) {
    /* Suppress rendering if stderr is not a TTY. */
    if (!isatty(fileno(stderr))) return;

    int term_w = progress_term_width();

    /* Calculate percentage */
    double pct = bar->total > 0 ? (double)bar->current / (double)bar->total * 100.0 : 0.0;
    if (pct > 100.0) pct = 100.0;

    /* Timing */
    int64_t now = time_now_ms();
    double elapsed_s = (double)(now - bar->start_ms) / 1000.0;
    double speed = elapsed_s > 0.01 ? (double)bar->current / elapsed_s : 0.0;
    double eta_s = (speed > 0.01 && bar->current < bar->total) ? (double)(bar->total - bar->current) / speed : 0.0;

    char elapsed_str[16], eta_str[16];
    format_time((int64_t)elapsed_s, elapsed_str, sizeof(elapsed_str));
    format_time((int64_t)eta_s, eta_str, sizeof(eta_str));

    /* Build the stats suffix: " 450/1000 [00:30<00:37, 15.0 it/s]" */
    char stats[128];
    snprintf(stats, sizeof(stats), " %lld/%lld [%s<%s, %.1f it/s]", (long long)bar->current, (long long)bar->total,
             elapsed_str, eta_str, speed);

    /* Build the description + pct prefix: "Compiling:  45%|" */
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "%s: %3d%%|", bar->desc, (int)pct);

    /* Calculate bar width: (term_w - 1) to avoid exact-width wrap causing \r to fail */
    int avail = term_w - 1;
    int bar_width = avail - (int)strlen(prefix) - 1 - (int)strlen(stats);
    if (bar_width < 5) bar_width = 5;
    if (bar_width > 60) bar_width = 60;

    int filled = bar->total > 0 ? (int)((int64_t)bar_width * bar->current / bar->total) : 0;
    if (filled > bar_width) filled = bar_width;
    int empty = bar_width - filled;

    /* Render to stderr */
    fprintf(stderr, "\r%s", prefix);
    for (int i = 0; i < filled; i++) fprintf(stderr, "\xe2\x96\x88"); /* U+2588 FULL BLOCK */
    for (int i = 0; i < empty; i++) fprintf(stderr, "\xe2\x96\x91");  /* U+2591 LIGHT SHADE */
    fprintf(stderr, "|%s", stats);

    /* Clear any leftover characters from previous longer render */
    int cur_len = (int)strlen(prefix) + filled * 3 + empty * 3 + 1 + (int)strlen(stats);
    if (bar->last_render_len > cur_len) {
        int diff = bar->last_render_len - cur_len;
        for (int i = 0; i < diff; i++) fputc(' ', stderr);
    }
    bar->last_render_len = cur_len;

    fflush(stderr);
    bar->last_render_ms = now;
}

int progress_new(int64_t total, const char *desc) {
    for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
        if (!bars[i].active) {
            bars[i].active = true;
            bars[i].total = total;
            bars[i].current = 0;
            bars[i].start_ms = time_now_ms();
            bars[i].last_render_ms = 0;
            bars[i].last_render_len = 0;
            if (desc) {
                strncpy(bars[i].desc, desc, sizeof(bars[i].desc) - 1);
                bars[i].desc[sizeof(bars[i].desc) - 1] = '\0';
            } else {
                bars[i].desc[0] = '\0';
            }
            bars[i].bar_width = 30;
            render_bar(&bars[i]);
            return i;
        }
    }
    return -1;
}

void progress_update(int handle, int64_t n) {
    if (handle < 0 || handle >= MAX_PROGRESS_BARS) return;
    ProgressBar *bar = &bars[handle];
    if (!bar->active) return;

    bar->current += n;
    if (bar->current > bar->total) bar->current = bar->total;

    /* Rate-limit rendering. */
    int64_t now = time_now_ms();
    if (now - bar->last_render_ms >= RENDER_INTERVAL_MS || bar->current >= bar->total) { render_bar(bar); }
}

void progress_finish(int handle) {
    if (handle < 0 || handle >= MAX_PROGRESS_BARS) return;
    ProgressBar *bar = &bars[handle];
    if (!bar->active) return;

    bar->current = bar->total;
    render_bar(bar);
    if (isatty(fileno(stderr))) fprintf(stderr, "\n");
    bar->active = false;
}

void progress_free_all(void) {
    for (int i = 0; i < MAX_PROGRESS_BARS; i++) { bars[i].active = false; }
}
