#include "completion.h"

#if defined(LATTICE_HAS_EDITLINE) || defined(LATTICE_HAS_READLINE)

#if defined(LATTICE_HAS_EDITLINE)
#include <editline/readline.h>
#elif defined(LATTICE_HAS_READLINE)
#include <readline/readline.h>
#include <readline/history.h>
#endif

#include <stdlib.h>
#include <string.h>

/* ── Static keyword list ── */
static const char *lattice_keywords[] = {
    "fn",    "let",    "flux",    "fix",         "if",    "else",     "while",  "for",  "in",    "return",
    "match", "struct", "enum",    "trait",       "impl",  "import",   "export", "try",  "catch", "throw",
    "spawn", "scope",  "select",  "freeze",      "thaw",  "clone",    "anneal", "true", "false", "nil",
    "print", "test",   "require", "require_ext", "break", "continue", NULL};

/* ── Builtin functions (from runtime.c rt_register_native calls) ── */
static const char *lattice_builtins[] = {
    /* Core */
    "to_string", "typeof", "len", "parse_int", "parse_float", "ord", "chr", "abs", "floor", "ceil", "exit", "error",
    "is_error", "phase_of", "assert", "version", "input",
    /* Phase tracking */
    "track", "phases", "history", "rewind", "pressurize", "depressurize", "pressure_of", "grow",
    /* Math */
    "round", "sqrt", "pow", "min", "max", "random", "random_int", "log", "log2", "log10", "sin", "cos", "tan", "asin",
    "acos", "atan", "atan2", "exp", "sign", "gcd", "lcm", "is_nan", "is_inf", "sinh", "cosh", "tanh", "lerp", "clamp",
    "math_pi", "math_e",
    /* File I/O */
    "read_file", "write_file", "read_file_bytes", "write_file_bytes", "file_exists", "delete_file", "list_dir",
    "append_file", "mkdir", "rename", "is_dir", "is_file", "rmdir", "glob", "stat", "copy_file", "realpath", "tempdir",
    "tempfile", "chmod", "file_size",
    /* Compilation */
    "compile_file", "load_bytecode",
    /* Paths */
    "path_join", "path_dir", "path_base", "path_ext",
    /* Networking */
    "tcp_listen", "tcp_accept", "tcp_connect", "tcp_read", "tcp_read_bytes", "tcp_write", "tcp_close", "tcp_peer_addr",
    "tcp_set_timeout",
    /* TLS */
    "tls_connect", "tls_read", "tls_read_bytes", "tls_write", "tls_close", "tls_available",
    /* HTTP */
    "http_get", "http_post", "http_request",
    /* Data formats */
    "json_parse", "json_stringify", "toml_parse", "toml_stringify", "yaml_parse", "yaml_stringify",
    /* Crypto */
    "sha256", "md5", "base64_encode", "base64_decode", "sha512", "hmac_sha256", "random_bytes",
    /* Regex */
    "regex_match", "regex_find_all", "regex_replace",
    /* Time */
    "time", "sleep", "time_format", "time_parse", "time_year", "time_month", "time_day", "time_hour", "time_minute",
    "time_second", "time_weekday", "time_add", "is_leap_year",
    /* Duration */
    "duration", "duration_from_seconds", "duration_from_millis", "duration_add", "duration_sub", "duration_to_string",
    "duration_hours", "duration_minutes", "duration_seconds", "duration_millis",
    /* DateTime */
    "datetime_now", "datetime_from_epoch", "datetime_to_epoch", "datetime_from_iso", "datetime_to_iso",
    "datetime_add_duration", "datetime_sub", "datetime_format", "datetime_to_utc", "datetime_to_local",
    "timezone_offset",
    /* Calendar */
    "days_in_month", "day_of_week", "day_of_year",
    /* Environment / Process */
    "env", "env_set", "env_keys", "cwd", "exec", "shell", "platform", "hostname", "pid",
    /* Conversion */
    "to_int", "to_float", "struct_name", "struct_fields", "struct_to_map", "struct_from_map", "repr", "format", "range",
    "print_raw", "eprint", "identity", "debug_assert", "panic",
    /* Module */
    "args", "is_complete", "tokenize", "lat_eval",
    /* Bit manipulation */
    "float_to_bits", "bits_to_float",
    /* URL encoding */
    "url_encode", "url_decode",
    /* CSV */
    "csv_parse", "csv_stringify",
    /* Functional */
    "pipe", "compose",
    /* Recursion */
    "set_recursion_limit", "recursion_limit", NULL};

/* ── Constructor-style builtins (Type::method) ── */
static const char *lattice_constructors[] = {"Map::new",    "Set::new",     "Set::from",           "Channel::new",
                                             "Buffer::new", "Buffer::from", "Buffer::from_string", "Ref::new",
                                             NULL};

/* ── Method names (for completion after '.') ── */
static const char *lattice_methods[] = {
    /* Array methods */
    "push", "pop", "len", "length", "map", "filter", "reduce", "each", "for_each", "find", "any", "all", "contains",
    "index_of", "reverse", "enumerate", "sort", "sort_by", "flat", "flatten", "flat_map", "slice", "take", "drop",
    "chunk", "join", "unique", "zip", "insert", "remove_at", "sum", "min", "max", "first", "last", "group_by",
    /* String methods */
    "split", "trim", "trim_start", "trim_end", "starts_with", "ends_with", "to_upper", "to_lower", "capitalize",
    "title_case", "snake_case", "camel_case", "kebab_case", "replace", "substring", "chars", "bytes", "repeat",
    "pad_left", "pad_right", "count", "is_empty",
    /* Map methods */
    "get", "set", "has", "keys", "values", "entries", "remove", "merge",
    /* Set methods */
    "add", "to_array", "union", "intersection", "difference", "is_subset", "is_superset",
    /* Buffer methods */
    "capacity", "push_u16", "push_u32", "read_u8", "write_u8", "read_u16", "write_u16", "read_u32", "write_u32",
    "read_i8", "read_i16", "read_i32", "read_f32", "read_f64", "clear", "fill", "resize", "to_string", "to_hex",
    /* Channel methods */
    "send", "recv", "close",
    /* Ref methods */
    "deref", "inner_type",
    /* Enum methods */
    "tag", "payload", "enum_name", "variant_name", "is_variant", NULL};

/* ── Scope constructor names (for completion after '::') ── */
static const char *lattice_scope_constructors[] = {"new", "from", "from_string", NULL};

/* ── All global completion lists, walked sequentially ── */
static const char **global_lists[] = {lattice_keywords, lattice_builtins, lattice_constructors, NULL};

/* ── State for the generator function ── */
static int gen_list_idx; /* which list in global_lists[] (or 0 for single-list modes) */
static int gen_item_idx; /* index within the current list */
static size_t gen_text_len;

/* Which completion mode are we in? */
typedef enum {
    COMPLETE_GLOBAL, /* keywords + builtins + constructors */
    COMPLETE_METHOD, /* after '.' */
    COMPLETE_SCOPE,  /* after '::' */
} CompletionMode;

static CompletionMode completion_mode;

/* ── Generator: returns one match at a time, NULL when exhausted ── */
static char *lattice_completion_generator(const char *text, int state) {
    if (state == 0) {
        gen_list_idx = 0;
        gen_item_idx = 0;
        gen_text_len = strlen(text);
    }

    if (completion_mode == COMPLETE_METHOD) {
        while (lattice_methods[gen_item_idx] != NULL) {
            const char *name = lattice_methods[gen_item_idx++];
            if (strncmp(name, text, gen_text_len) == 0) return strdup(name);
        }
        return NULL;
    }

    if (completion_mode == COMPLETE_SCOPE) {
        while (lattice_scope_constructors[gen_item_idx] != NULL) {
            const char *name = lattice_scope_constructors[gen_item_idx++];
            if (strncmp(name, text, gen_text_len) == 0) return strdup(name);
        }
        return NULL;
    }

    /* COMPLETE_GLOBAL: walk through global_lists[] sequentially */
    while (global_lists[gen_list_idx] != NULL) {
        const char **list = global_lists[gen_list_idx];
        while (list[gen_item_idx] != NULL) {
            const char *name = list[gen_item_idx++];
            if (strncmp(name, text, gen_text_len) == 0) return strdup(name);
        }
        /* Advance to next list */
        gen_list_idx++;
        gen_item_idx = 0;
    }

    return NULL;
}

/* ── Attempted completion function (registered with readline) ── */
static char **lattice_completion(const char *text, int start, int end) {
    (void)end;

    /* Determine completion mode based on what precedes the cursor */
    completion_mode = COMPLETE_GLOBAL;

    if (start >= 2 && rl_line_buffer[start - 1] == ':' && rl_line_buffer[start - 2] == ':') {
        completion_mode = COMPLETE_SCOPE;
    } else if (start >= 1 && rl_line_buffer[start - 1] == '.') {
        completion_mode = COMPLETE_METHOD;
    }

    /* Disable default filename completion as fallback */
    rl_attempted_completion_over = 1;

    return rl_completion_matches(text, lattice_completion_generator);
}

/* ── Public init ── */
void lattice_completion_init(void) { rl_attempted_completion_function = lattice_completion; }

#endif /* LATTICE_HAS_EDITLINE || LATTICE_HAS_READLINE */
