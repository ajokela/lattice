#include "lsp.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void idx_add_builtin(LspSymbolIndex *idx, const char *name,
                            const char *sig, const char *doc) {
    if (idx->builtin_count >= idx->builtin_cap) {
        idx->builtin_cap = idx->builtin_cap ? idx->builtin_cap * 2 : 64;
        idx->builtins = realloc(idx->builtins, idx->builtin_cap * sizeof(LspSymbol));
    }
    LspSymbol *s = &idx->builtins[idx->builtin_count++];
    s->name = strdup(name);
    s->signature = strdup(sig);
    s->doc = strdup(doc);
    s->kind = LSP_SYM_FUNCTION;
    s->line = -1;
    s->col = -1;
}

static void idx_add_method(LspSymbolIndex *idx, const char *name,
                           const char *sig, const char *doc) {
    if (idx->method_count >= idx->method_cap) {
        idx->method_cap = idx->method_cap ? idx->method_cap * 2 : 64;
        idx->methods = realloc(idx->methods, idx->method_cap * sizeof(LspSymbol));
    }
    LspSymbol *s = &idx->methods[idx->method_count++];
    s->name = strdup(name);
    s->signature = strdup(sig);
    s->doc = strdup(doc);
    s->kind = LSP_SYM_METHOD;
    s->line = -1;
    s->col = -1;
}

/* Extract name from signature: "name(args) -> Type" → "name" */
static char *extract_name(const char *sig) {
    const char *paren = strchr(sig, '(');
    if (!paren) return strdup(sig);
    size_t len = (size_t)(paren - sig);
    char *name = malloc(len + 1);
    memcpy(name, sig, len);
    name[len] = '\0';
    return name;
}

/* Scan a single source file for /// @builtin and /// @method doc comments */
static void scan_file(LspSymbolIndex *idx, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[2048];
    char sig[512];
    char desc[4096];

    while (fgets(line, sizeof(line), f)) {
        bool is_builtin = (strstr(line, "/// @builtin ") != NULL);
        bool is_method = (strstr(line, "/// @method ") != NULL);

        if (!is_builtin && !is_method) continue;

        /* Extract signature */
        const char *start;
        if (is_builtin)
            start = strstr(line, "@builtin ") + 9;
        else
            start = strstr(line, "@method ") + 8;

        snprintf(sig, sizeof(sig), "%s", start);
        sig[strcspn(sig, "\n\r")] = '\0';

        /* Read following comment lines for category + description */
        desc[0] = '\0';
        char category[128] = "";

        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "///", 3) != 0) break;

            const char *text = line + 3;
            while (*text == ' ') text++;

            if (strncmp(text, "@category ", 10) == 0) {
                snprintf(category, sizeof(category), "%s", text + 10);
                category[strcspn(category, "\n\r")] = '\0';
            } else if (strncmp(text, "@example ", 9) == 0) {
                /* Skip examples */
            } else {
                /* Description text */
                char trimmed[1024];
                snprintf(trimmed, sizeof(trimmed), "%s", text);
                trimmed[strcspn(trimmed, "\n\r")] = '\0';
                if (desc[0]) strncat(desc, " ", sizeof(desc) - strlen(desc) - 1);
                strncat(desc, trimmed, sizeof(desc) - strlen(desc) - 1);
            }
        }

        /* Build doc string */
        char doc[5120];
        if (category[0])
            snprintf(doc, sizeof(doc), "**%s**\n\n*%s*\n\n%s", sig, category, desc);
        else
            snprintf(doc, sizeof(doc), "**%s**\n\n%s", sig, desc);

        char *name = extract_name(sig);

        if (is_builtin)
            idx_add_builtin(idx, name, sig, doc);
        else
            idx_add_method(idx, name, sig, doc);

        free(name);
    }

    fclose(f);
}

LspSymbolIndex *lsp_symbol_index_new(const char *eval_path) {
    LspSymbolIndex *idx = calloc(1, sizeof(LspSymbolIndex));
    scan_file(idx, eval_path);
    return idx;
}

void lsp_symbol_index_add_file(LspSymbolIndex *idx, const char *path) {
    if (!idx || !path) return;
    scan_file(idx, path);
}

void lsp_symbol_index_free(LspSymbolIndex *idx) {
    if (!idx) return;
    for (size_t i = 0; i < idx->builtin_count; i++) {
        free(idx->builtins[i].name);
        free(idx->builtins[i].signature);
        free(idx->builtins[i].doc);
    }
    free(idx->builtins);
    for (size_t i = 0; i < idx->method_count; i++) {
        free(idx->methods[i].name);
        free(idx->methods[i].signature);
        free(idx->methods[i].doc);
    }
    free(idx->methods);
    free(idx);
}
