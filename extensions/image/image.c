/*
 * Lattice Image Extension
 *
 * Lightweight image metadata and basic operations.
 * Reads image headers directly (no external library dependencies).
 * Uses macOS sips for resize/convert/thumbnail operations.
 */

/* Enable POSIX extensions for strcasecmp() on Linux with -std=c11 */
#define _POSIX_C_SOURCE 200809L

#include "lattice_ext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <strings.h> /* strcasecmp */

/* Forward declare the init function (exported symbol) */
void lat_ext_init(LatExtContext *ctx);

/* ── Image format detection from magic bytes ── */

typedef enum { IMG_PNG, IMG_JPEG, IMG_GIF, IMG_BMP, IMG_WEBP, IMG_UNKNOWN } ImageFormat;

static const char *format_name(ImageFormat fmt) {
    switch (fmt) {
        case IMG_PNG: return "png";
        case IMG_JPEG: return "jpeg";
        case IMG_GIF: return "gif";
        case IMG_BMP: return "bmp";
        case IMG_WEBP: return "webp";
        case IMG_UNKNOWN: return "unknown";
    }
    return "unknown";
}

static ImageFormat detect_format(const unsigned char *buf, size_t len) {
    if (len >= 8 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G' && buf[4] == 0x0D &&
        buf[5] == 0x0A && buf[6] == 0x1A && buf[7] == 0x0A) {
        return IMG_PNG;
    }
    if (len >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF) { return IMG_JPEG; }
    if (len >= 6 && buf[0] == 'G' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == '8' &&
        (buf[4] == '7' || buf[4] == '9') && buf[5] == 'a') {
        return IMG_GIF;
    }
    if (len >= 2 && buf[0] == 'B' && buf[1] == 'M') { return IMG_BMP; }
    if (len >= 12 && buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F' && buf[8] == 'W' &&
        buf[9] == 'E' && buf[10] == 'B' && buf[11] == 'P') {
        return IMG_WEBP;
    }
    return IMG_UNKNOWN;
}

/* ── Helper: read big-endian u32 ── */

static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ── Helper: read big-endian u16 ── */

static uint16_t read_be16(const unsigned char *p) { return (uint16_t)((uint16_t)p[0] << 8) | (uint16_t)p[1]; }

/* ── Helper: read little-endian u16 ── */

static uint16_t read_le16(const unsigned char *p) { return (uint16_t)((uint16_t)p[1] << 8) | (uint16_t)p[0]; }

/* ── Helper: read little-endian u32 ── */

static uint32_t read_le32(const unsigned char *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

/* ── Dimension parsing per format ── */

typedef struct {
    int valid;
    uint32_t width;
    uint32_t height;
} Dimensions;

static Dimensions parse_png_dimensions(FILE *f) {
    Dimensions d = {0, 0, 0};
    unsigned char ihdr[8];

    /* IHDR chunk starts at offset 16 (8 byte signature + 4 byte length + 4 byte type) */
    if (fseek(f, 16, SEEK_SET) != 0) return d;
    if (fread(ihdr, 1, 8, f) != 8) return d;

    d.width = read_be32(ihdr);
    d.height = read_be32(ihdr + 4);
    d.valid = 1;
    return d;
}

static Dimensions parse_jpeg_dimensions(FILE *f) {
    Dimensions d = {0, 0, 0};
    unsigned char buf[2];
    long pos;

    /* Skip SOI marker (FF D8) */
    if (fseek(f, 2, SEEK_SET) != 0) return d;

    /* Scan through markers looking for SOF0..SOF2 */
    while (1) {
        if (fread(buf, 1, 2, f) != 2) return d;

        /* Must be a marker (FF xx) */
        if (buf[0] != 0xFF) return d;

        /* Skip padding FF bytes */
        while (buf[1] == 0xFF) {
            if (fread(&buf[1], 1, 1, f) != 1) return d;
        }

        /* SOF0 (C0), SOF1 (C1), SOF2 (C2) contain dimensions */
        if (buf[1] == 0xC0 || buf[1] == 0xC1 || buf[1] == 0xC2) {
            unsigned char sof[7];
            /* Read length (2) + precision (1) + height (2) + width (2) */
            if (fread(sof, 1, 7, f) != 7) return d;
            d.height = read_be16(sof + 3);
            d.width = read_be16(sof + 5);
            d.valid = 1;
            return d;
        }

        /* Skip to next marker: read segment length and advance */
        {
            unsigned char lenbuf[2];
            uint16_t seg_len;
            if (fread(lenbuf, 1, 2, f) != 2) return d;
            seg_len = read_be16(lenbuf);
            if (seg_len < 2) return d;
            pos = ftell(f);
            if (pos < 0) return d;
            if (fseek(f, (long)(seg_len - 2), SEEK_CUR) != 0) return d;
        }
    }
}

static Dimensions parse_gif_dimensions(FILE *f) {
    Dimensions d = {0, 0, 0};
    unsigned char buf[4];

    /* Width at offset 6 (LE u16), height at offset 8 (LE u16) */
    if (fseek(f, 6, SEEK_SET) != 0) return d;
    if (fread(buf, 1, 4, f) != 4) return d;

    d.width = read_le16(buf);
    d.height = read_le16(buf + 2);
    d.valid = 1;
    return d;
}

static Dimensions parse_bmp_dimensions(FILE *f) {
    Dimensions d = {0, 0, 0};
    unsigned char buf[8];
    int32_t h;

    /* Width at offset 18 (LE i32), height at offset 22 (LE i32) */
    if (fseek(f, 18, SEEK_SET) != 0) return d;
    if (fread(buf, 1, 8, f) != 8) return d;

    d.width = read_le32(buf);
    /* BMP height can be negative (top-down), use absolute value */
    h = (int32_t)read_le32(buf + 4);
    d.height = (uint32_t)(h < 0 ? -h : h);
    d.valid = 1;
    return d;
}

static Dimensions parse_webp_dimensions(FILE *f) {
    Dimensions d = {0, 0, 0};
    unsigned char chunk_hdr[8];

    /* After RIFF header (12 bytes), read the first chunk */
    if (fseek(f, 12, SEEK_SET) != 0) return d;
    if (fread(chunk_hdr, 1, 8, f) != 8) return d;

    /* VP8 (lossy) */
    if (chunk_hdr[0] == 'V' && chunk_hdr[1] == 'P' && chunk_hdr[2] == '8' && chunk_hdr[3] == ' ') {
        unsigned char vp8[10];
        /* Skip 3 bytes of VP8 bitstream header (frame tag) */
        if (fread(vp8, 1, 10, f) != 10) return d;
        /* Bytes 6-7: width (LE u16, lower 14 bits), bytes 8-9: height (LE u16, lower 14 bits) */
        d.width = read_le16(vp8 + 6) & 0x3FFF;
        d.height = read_le16(vp8 + 8) & 0x3FFF;
        d.valid = 1;
        return d;
    }

    /* VP8L (lossless) */
    if (chunk_hdr[0] == 'V' && chunk_hdr[1] == 'P' && chunk_hdr[2] == '8' && chunk_hdr[3] == 'L') {
        unsigned char sig_and_bits[5];
        uint32_t bits;
        if (fread(sig_and_bits, 1, 5, f) != 5) return d;
        /* First byte is signature (0x2F), next 4 bytes contain width/height */
        bits = read_le32(sig_and_bits + 1);
        d.width = (bits & 0x3FFF) + 1;
        d.height = ((bits >> 14) & 0x3FFF) + 1;
        d.valid = 1;
        return d;
    }

    /* VP8X (extended) */
    if (chunk_hdr[0] == 'V' && chunk_hdr[1] == 'P' && chunk_hdr[2] == '8' && chunk_hdr[3] == 'X') {
        unsigned char ext[10];
        if (fread(ext, 1, 10, f) != 10) return d;
        /* Bytes 4-6: canvas width - 1 (24-bit LE), bytes 7-9: canvas height - 1 (24-bit LE) */
        d.width = ((uint32_t)ext[4] | ((uint32_t)ext[5] << 8) | ((uint32_t)ext[6] << 16)) + 1;
        d.height = ((uint32_t)ext[7] | ((uint32_t)ext[8] << 8) | ((uint32_t)ext[9] << 16)) + 1;
        d.valid = 1;
        return d;
    }

    return d;
}

static Dimensions get_dimensions(FILE *f, ImageFormat fmt) {
    switch (fmt) {
        case IMG_PNG: return parse_png_dimensions(f);
        case IMG_JPEG: return parse_jpeg_dimensions(f);
        case IMG_GIF: return parse_gif_dimensions(f);
        case IMG_BMP: return parse_bmp_dimensions(f);
        case IMG_WEBP: return parse_webp_dimensions(f);
        default: {
            Dimensions d = {0, 0, 0};
            return d;
        }
    }
}

/* ── Helper: get file size ── */

static int64_t get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

/* ── Helper: infer sips format from file extension (macOS only) ── */

#ifdef __APPLE__
static const char *extension_to_sips_format(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    dot++; /* skip the dot */

    if (strcasecmp(dot, "png") == 0) return "png";
    if (strcasecmp(dot, "jpg") == 0 || strcasecmp(dot, "jpeg") == 0) return "jpeg";
    if (strcasecmp(dot, "gif") == 0) return "gif";
    if (strcasecmp(dot, "bmp") == 0) return "bmp";
    if (strcasecmp(dot, "tiff") == 0 || strcasecmp(dot, "tif") == 0) return "tiff";
    if (strcasecmp(dot, "heic") == 0) return "heic";
    if (strcasecmp(dot, "pdf") == 0) return "pdf";
    if (strcasecmp(dot, "ico") == 0) return "ico";
    return NULL;
}
#endif /* __APPLE__ */

/* ── Extension functions ── */

/* image.format(path) -> String */
static LatExtValue *image_format(LatExtValue **args, size_t argc) {
    const char *path;
    FILE *f;
    unsigned char header[12];
    size_t nread;
    ImageFormat fmt;

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_STRING) {
        return lat_ext_error("image.format() expects a file path (String)");
    }
    path = lat_ext_as_string(args[0]);

    f = fopen(path, "rb");
    if (!f) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "image.format: cannot open file '%s'", path);
        return lat_ext_error(errbuf);
    }

    nread = fread(header, 1, sizeof(header), f);
    fclose(f);

    fmt = detect_format(header, nread);
    return lat_ext_string(format_name(fmt));
}

/* image.dimensions(path) -> Map {"width": Int, "height": Int} */
static LatExtValue *image_dimensions(LatExtValue **args, size_t argc) {
    const char *path;
    FILE *f;
    unsigned char header[12];
    size_t nread;
    ImageFormat fmt;
    Dimensions dim;
    LatExtValue *map, *w, *h;

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_STRING) {
        return lat_ext_error("image.dimensions() expects a file path (String)");
    }
    path = lat_ext_as_string(args[0]);

    f = fopen(path, "rb");
    if (!f) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "image.dimensions: cannot open file '%s'", path);
        return lat_ext_error(errbuf);
    }

    nread = fread(header, 1, sizeof(header), f);
    fmt = detect_format(header, nread);

    if (fmt == IMG_UNKNOWN) {
        fclose(f);
        return lat_ext_error("image.dimensions: unsupported or unrecognized image format");
    }

    dim = get_dimensions(f, fmt);
    fclose(f);

    if (!dim.valid) { return lat_ext_error("image.dimensions: failed to read image dimensions"); }

    map = lat_ext_map_new();
    w = lat_ext_int((int64_t)dim.width);
    h = lat_ext_int((int64_t)dim.height);
    lat_ext_map_set(map, "width", w);
    lat_ext_map_set(map, "height", h);
    lat_ext_free(w);
    lat_ext_free(h);
    return map;
}

/* image.info(path) -> Map {"width", "height", "format", "file_size"} */
static LatExtValue *image_info(LatExtValue **args, size_t argc) {
    const char *path;
    FILE *f;
    unsigned char header[12];
    size_t nread;
    ImageFormat fmt;
    Dimensions dim;
    int64_t fsize;
    LatExtValue *map, *v;

    if (argc < 1 || lat_ext_type(args[0]) != LAT_EXT_STRING) {
        return lat_ext_error("image.info() expects a file path (String)");
    }
    path = lat_ext_as_string(args[0]);

    fsize = get_file_size(path);
    if (fsize < 0) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "image.info: cannot stat file '%s'", path);
        return lat_ext_error(errbuf);
    }

    f = fopen(path, "rb");
    if (!f) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "image.info: cannot open file '%s'", path);
        return lat_ext_error(errbuf);
    }

    nread = fread(header, 1, sizeof(header), f);
    fmt = detect_format(header, nread);

    map = lat_ext_map_new();

    /* format */
    v = lat_ext_string(format_name(fmt));
    lat_ext_map_set(map, "format", v);
    lat_ext_free(v);

    /* file_size */
    v = lat_ext_int(fsize);
    lat_ext_map_set(map, "file_size", v);
    lat_ext_free(v);

    /* dimensions (if we can parse them) */
    if (fmt != IMG_UNKNOWN) {
        dim = get_dimensions(f, fmt);
        if (dim.valid) {
            v = lat_ext_int((int64_t)dim.width);
            lat_ext_map_set(map, "width", v);
            lat_ext_free(v);

            v = lat_ext_int((int64_t)dim.height);
            lat_ext_map_set(map, "height", v);
            lat_ext_free(v);
        } else {
            v = lat_ext_int(0);
            lat_ext_map_set(map, "width", v);
            lat_ext_free(v);

            v = lat_ext_int(0);
            lat_ext_map_set(map, "height", v);
            lat_ext_free(v);
        }
    } else {
        v = lat_ext_int(0);
        lat_ext_map_set(map, "width", v);
        lat_ext_free(v);

        v = lat_ext_int(0);
        lat_ext_map_set(map, "height", v);
        lat_ext_free(v);
    }

    fclose(f);
    return map;
}

/* image.resize(src, dst, width, height) -> Bool */
static LatExtValue *image_resize(LatExtValue **args, size_t argc) {
#ifndef __APPLE__
    (void)args;
    (void)argc;
    return lat_ext_error("image.resize: not supported on this platform (requires macOS sips)");
#else
    const char *src, *dst;
    int64_t w, h;
    char cmd[2048];
    int rc;

    if (argc < 4 || lat_ext_type(args[0]) != LAT_EXT_STRING || lat_ext_type(args[1]) != LAT_EXT_STRING ||
        lat_ext_type(args[2]) != LAT_EXT_INT || lat_ext_type(args[3]) != LAT_EXT_INT) {
        return lat_ext_error("image.resize() expects (src: String, dst: String, width: Int, height: Int)");
    }

    src = lat_ext_as_string(args[0]);
    dst = lat_ext_as_string(args[1]);
    w = lat_ext_as_int(args[2]);
    h = lat_ext_as_int(args[3]);

    if (w <= 0 || h <= 0) { return lat_ext_error("image.resize: width and height must be positive"); }

    snprintf(cmd, sizeof(cmd), "sips -z %lld %lld '%s' --out '%s' > /dev/null 2>&1", (long long)h, (long long)w, src,
             dst);

    rc = system(cmd);
    if (rc != 0) { return lat_ext_error("image.resize: sips command failed"); }
    return lat_ext_bool(true);
#endif
}

/* image.convert(src, dst) -> Bool */
static LatExtValue *image_convert(LatExtValue **args, size_t argc) {
#ifndef __APPLE__
    (void)args;
    (void)argc;
    return lat_ext_error("image.convert: not supported on this platform (requires macOS sips)");
#else
    const char *src, *dst, *fmt;
    char cmd[2048];
    int rc;

    if (argc < 2 || lat_ext_type(args[0]) != LAT_EXT_STRING || lat_ext_type(args[1]) != LAT_EXT_STRING) {
        return lat_ext_error("image.convert() expects (src: String, dst: String)");
    }

    src = lat_ext_as_string(args[0]);
    dst = lat_ext_as_string(args[1]);

    fmt = extension_to_sips_format(dst);
    if (!fmt) { return lat_ext_error("image.convert: cannot infer output format from destination file extension"); }

    snprintf(cmd, sizeof(cmd), "sips -s format %s '%s' --out '%s' > /dev/null 2>&1", fmt, src, dst);

    rc = system(cmd);
    if (rc != 0) { return lat_ext_error("image.convert: sips command failed"); }
    return lat_ext_bool(true);
#endif
}

/* image.thumbnail(src, dst, max_size) -> Bool */
static LatExtValue *image_thumbnail(LatExtValue **args, size_t argc) {
#ifndef __APPLE__
    (void)args;
    (void)argc;
    return lat_ext_error("image.thumbnail: not supported on this platform (requires macOS sips)");
#else
    const char *src, *dst;
    int64_t max_size;
    FILE *f;
    unsigned char header[12];
    size_t nread;
    ImageFormat fmt;
    Dimensions dim;
    uint32_t new_w, new_h;
    char cmd[2048];
    int rc;

    if (argc < 3 || lat_ext_type(args[0]) != LAT_EXT_STRING || lat_ext_type(args[1]) != LAT_EXT_STRING ||
        lat_ext_type(args[2]) != LAT_EXT_INT) {
        return lat_ext_error("image.thumbnail() expects (src: String, dst: String, max_size: Int)");
    }

    src = lat_ext_as_string(args[0]);
    dst = lat_ext_as_string(args[1]);
    max_size = lat_ext_as_int(args[2]);

    if (max_size <= 0) { return lat_ext_error("image.thumbnail: max_size must be positive"); }

    /* Read original dimensions to compute aspect-ratio-preserving size */
    f = fopen(src, "rb");
    if (!f) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "image.thumbnail: cannot open file '%s'", src);
        return lat_ext_error(errbuf);
    }

    nread = fread(header, 1, sizeof(header), f);
    fmt = detect_format(header, nread);

    if (fmt == IMG_UNKNOWN) {
        fclose(f);
        return lat_ext_error("image.thumbnail: unsupported or unrecognized image format");
    }

    dim = get_dimensions(f, fmt);
    fclose(f);

    if (!dim.valid || dim.width == 0 || dim.height == 0) {
        return lat_ext_error("image.thumbnail: failed to read source image dimensions");
    }

    /* Compute new dimensions preserving aspect ratio */
    if (dim.width <= (uint32_t)max_size && dim.height <= (uint32_t)max_size) {
        /* Already fits, just copy with sips */
        new_w = dim.width;
        new_h = dim.height;
    } else if (dim.width >= dim.height) {
        new_w = (uint32_t)max_size;
        new_h = (uint32_t)((double)dim.height * (double)max_size / (double)dim.width + 0.5);
        if (new_h == 0) new_h = 1;
    } else {
        new_h = (uint32_t)max_size;
        new_w = (uint32_t)((double)dim.width * (double)max_size / (double)dim.height + 0.5);
        if (new_w == 0) new_w = 1;
    }

    snprintf(cmd, sizeof(cmd), "sips -z %u %u '%s' --out '%s' > /dev/null 2>&1", new_h, new_w, src, dst);

    rc = system(cmd);
    if (rc != 0) { return lat_ext_error("image.thumbnail: sips command failed"); }
    return lat_ext_bool(true);
#endif
}

/* ── Extension init ── */

void lat_ext_init(LatExtContext *ctx) {
    lat_ext_register(ctx, "info", image_info);
    lat_ext_register(ctx, "format", image_format);
    lat_ext_register(ctx, "dimensions", image_dimensions);
    lat_ext_register(ctx, "resize", image_resize);
    lat_ext_register(ctx, "convert", image_convert);
    lat_ext_register(ctx, "thumbnail", image_thumbnail);
}
