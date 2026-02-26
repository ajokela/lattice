#ifndef DOC_GEN_H
#define DOC_GEN_H

#include <stddef.h>
#include <stdbool.h>

/* Output format for doc generation */
typedef enum {
    DOC_FMT_MARKDOWN,
    DOC_FMT_JSON,
    DOC_FMT_HTML,
} DocFormat;

/* Kind of documented item */
typedef enum {
    DOC_FUNCTION,
    DOC_STRUCT,
    DOC_ENUM,
    DOC_TRAIT,
    DOC_IMPL,
    DOC_VARIABLE,
    DOC_MODULE,
} DocItemKind;

/* A documented struct field */
typedef struct {
    char *name;
    char *type_name;
    char *doc; /* nullable */
} DocField;

/* A documented enum variant */
typedef struct {
    char *name;
    char *params; /* nullable — e.g. "Int, Int, Int" */
    char *doc;    /* nullable */
} DocVariant;

/* A documented function parameter */
typedef struct {
    char *name;
    char *type_name; /* nullable */
    bool is_variadic;
    bool has_default;
} DocParam;

/* A documented trait method (signature only) */
typedef struct {
    char *name;
    DocParam *params;
    size_t param_count;
    char *return_type; /* nullable */
    char *doc;         /* nullable */
} DocTraitMethod;

/* A single documented item extracted from a .lat file */
typedef struct {
    DocItemKind kind;
    char *name;
    char *doc; /* doc comment text, nullable */
    int line;  /* source line number */

    union {
        struct {
            DocParam *params;
            size_t param_count;
            char *return_type; /* nullable */
        } fn;

        struct {
            DocField *fields;
            size_t field_count;
        } strct;

        struct {
            DocVariant *variants;
            size_t variant_count;
        } enm;

        struct {
            DocTraitMethod *methods;
            size_t method_count;
        } trait;

        struct {
            char *trait_name;
            char *type_name;
            DocTraitMethod *methods;
            size_t method_count;
        } impl;

        struct {
            char *phase;     /* "flux", "fix", "let" */
            char *type_name; /* nullable */
        } var;
    } as;
} DocItem;

/* Result of extracting docs from a file */
typedef struct {
    char *filename;
    char *module_doc; /* nullable — first /// block before any decl */
    DocItem *items;
    size_t item_count;
} DocFile;

/* Extract documentation from a .lat source string.
 * Returns a DocFile with all documented items.
 * Caller must call doc_file_free() when done. */
DocFile doc_extract(const char *source, const char *filename);

/* Free a DocFile and all its contents. */
void doc_file_free(DocFile *df);

/* Render documentation to a string in the given format.
 * Caller must free() the returned string. */
char *doc_render(const DocFile *files, size_t file_count, DocFormat fmt);

/* Entry point for `clat doc` subcommand.
 * Returns exit code (0 = success). */
int doc_cmd(int argc, char **argv);

#endif /* DOC_GEN_H */
