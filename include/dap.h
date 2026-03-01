#ifndef DAP_H
#define DAP_H

#include "debugger.h"
#include <stdio.h>

struct cJSON;

/* ── DAP message I/O ── */

/* Read one DAP message (Content-Length framed JSON). Returns cJSON* or NULL. */
struct cJSON *dap_read_message(FILE *in);

/* Write a DAP message (Content-Length framed JSON). */
void dap_write_message(struct cJSON *msg, FILE *out);

/* ── DAP response/event helpers ── */

void dap_send_response(Debugger *dbg, int request_seq, const char *command, struct cJSON *body);
void dap_send_event(Debugger *dbg, const char *event, struct cJSON *body);
void dap_send_error(Debugger *dbg, int request_seq, const char *command, const char *message);

/* ── DAP lifecycle ── */

/* Process initialize + launch + configurationDone handshake.
 * Returns true on success, false if client disconnected or error. */
bool dap_handshake(Debugger *dbg, const char *source_path);

/* Send terminated event after program completes. */
void dap_send_terminated(Debugger *dbg);

/* Wait for disconnect request before exiting. */
void dap_wait_disconnect(Debugger *dbg);

/* ── DAP debugger integration ── */

/* Called from debugger_check() when in DAP mode.
 * Sends stopped event and enters DAP message loop.
 * Returns true to continue execution, false to quit. */
bool dap_debugger_check(Debugger *dbg, void *vm, void *frame, size_t frame_count, int line, const char *stop_reason);

#endif /* DAP_H */
