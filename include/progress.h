#ifndef PROGRESS_H
#define PROGRESS_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_PROGRESS_BARS 8

typedef struct {
    bool active;
    int64_t total;
    int64_t current;
    int64_t start_ms;
    int64_t last_render_ms;
    char desc[64];
    int bar_width;
    int last_render_len;
} ProgressBar;

/* Get terminal width (columns). Returns fallback (80) if unavailable. */
int progress_term_width(void);

/* Allocate a progress bar slot. Returns handle (0..MAX-1) or -1 on failure. */
int progress_new(int64_t total, const char *desc);

/* Increment bar by n steps and re-render (rate-limited). */
void progress_update(int handle, int64_t n);

/* Finish the bar: render final state and print newline. */
void progress_finish(int handle);

/* Free all progress bar state. */
void progress_free_all(void);

#endif /* PROGRESS_H */
