#ifndef INLINE_CACHE_H
#define INLINE_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

/*
 * Polymorphic Inline Cache (PIC) for method dispatch.
 *
 * Each call site (OP_INVOKE_LOCAL, OP_INVOKE_GLOBAL) gets a small fixed-size
 * cache that maps (type_tag, method_hash) to a handler ID.  On a cache hit
 * the VM can skip the full method-hash + strcmp dispatch chain and jump
 * directly to the right handler.
 *
 * The cache uses 4 entries (PIC_SIZE) to handle polymorphic call sites where
 * the receiver type varies (e.g. a helper function called with Arrays and Strings).
 *
 * Handler IDs are small integers that index into a jump table in the VM.
 * ID 0 means "not cached / cache miss".
 */

#define PIC_SIZE         4                      /* Max entries per call-site (polymorphic) */
#define PIC_DIRECT_SLOTS 64                     /* Direct-mapped cache slots per chunk */
#define PIC_DIRECT_MASK  (PIC_DIRECT_SLOTS - 1) /* Must be power-of-2 - 1 */

/* A single cache entry: (type_tag, method_hash) -> handler_id */
typedef struct {
    uint8_t type_tag;     /* ValueType of the receiver */
    uint32_t method_hash; /* djb2 hash of the method name */
    uint16_t handler_id;  /* Cached handler index (0 = empty/miss) */
} PICEntry;

/* Per-call-site inline cache */
typedef struct {
    PICEntry entries[PIC_SIZE];
    uint8_t count; /* Number of valid entries (0..PIC_SIZE) */
} PICSlot;

/* Direct-mapped PIC table: fixed-size array of PICSlots, lazily allocated.
 * Indexed by (ip_offset & PIC_DIRECT_MASK) where ip_offset is the bytecode
 * offset of the invoke opcode within the chunk.  Collisions between different
 * invoke sites are harmless (just cause cache misses). */
typedef struct {
    PICSlot *slots; /* NULL until first access; then PIC_DIRECT_SLOTS entries */
} PICTable;

/* ── API ── */

/* Initialize a PIC table (lazy — no allocation). */
static inline void pic_table_init(PICTable *t) { t->slots = NULL; }

/* Free a PIC table. */
static inline void pic_table_free(PICTable *t) {
    if (t->slots) {
        free(t->slots);
        t->slots = NULL;
    }
}

/* Ensure the PIC table is allocated. Returns the slots pointer. */
static inline PICSlot *pic_table_ensure(PICTable *t) {
    if (!t->slots) { t->slots = (PICSlot *)calloc(PIC_DIRECT_SLOTS, sizeof(PICSlot)); }
    return t->slots;
}

/* Get the PIC slot for a given instruction offset.
 * Returns NULL if the table hasn't been allocated yet. */
static inline PICSlot *pic_slot_for(PICTable *t, size_t ip_offset) {
    if (!t->slots) return NULL;
    return &t->slots[ip_offset & PIC_DIRECT_MASK];
}

/* Look up (type_tag, method_hash) in a PIC slot.
 * Returns the handler_id on hit, or 0 on miss. */
static inline uint16_t pic_lookup(const PICSlot *slot, uint8_t type_tag, uint32_t mhash) {
    for (int i = 0; i < slot->count; i++) {
        if (slot->entries[i].type_tag == type_tag && slot->entries[i].method_hash == mhash) {
            return slot->entries[i].handler_id;
        }
    }
    return 0;
}

/* Update a PIC slot with a new entry.  If full, evicts the oldest entry
 * (FIFO replacement). */
static inline void pic_update(PICSlot *slot, uint8_t type_tag, uint32_t mhash, uint16_t handler_id) {
    /* Check if already present — update in place */
    for (int i = 0; i < slot->count; i++) {
        if (slot->entries[i].type_tag == type_tag && slot->entries[i].method_hash == mhash) {
            slot->entries[i].handler_id = handler_id;
            return;
        }
    }
    /* Not present — add or evict */
    if (slot->count < PIC_SIZE) {
        slot->entries[slot->count].type_tag = type_tag;
        slot->entries[slot->count].method_hash = mhash;
        slot->entries[slot->count].handler_id = handler_id;
        slot->count++;
    } else {
        /* FIFO eviction: shift entries down, add at end */
        for (int i = 0; i < PIC_SIZE - 1; i++) slot->entries[i] = slot->entries[i + 1];
        slot->entries[PIC_SIZE - 1].type_tag = type_tag;
        slot->entries[PIC_SIZE - 1].method_hash = mhash;
        slot->entries[PIC_SIZE - 1].handler_id = handler_id;
    }
}

/*
 * Handler IDs for builtin methods.
 *
 * These are assigned dense integers so the VM can use a switch or jump table
 * to dispatch directly on cache hit, bypassing the hash+strcmp chain.
 *
 * Convention: IDs 1-127 for non-closure methods, 128+ for closure methods.
 * 0 = miss/empty.
 */

/* Array methods (non-closure) */
#define PIC_ARRAY_LEN       1
#define PIC_ARRAY_PUSH      2
#define PIC_ARRAY_POP       3
#define PIC_ARRAY_CONTAINS  4
#define PIC_ARRAY_ENUMERATE 5
#define PIC_ARRAY_REVERSE   6
#define PIC_ARRAY_JOIN      7
#define PIC_ARRAY_FLAT      8
#define PIC_ARRAY_FLATTEN   9
#define PIC_ARRAY_SLICE     10
#define PIC_ARRAY_TAKE      11
#define PIC_ARRAY_DROP      12
#define PIC_ARRAY_INDEX_OF  13
#define PIC_ARRAY_ZIP       14
#define PIC_ARRAY_UNIQUE    15
#define PIC_ARRAY_REMOVE_AT 16
#define PIC_ARRAY_INSERT    17
#define PIC_ARRAY_FIRST     18
#define PIC_ARRAY_LAST      19
#define PIC_ARRAY_SUM       20
#define PIC_ARRAY_MIN       21
#define PIC_ARRAY_MAX       22
#define PIC_ARRAY_CHUNK     23
#define PIC_ARRAY_LENGTH    24

/* Array methods (closure) */
#define PIC_ARRAY_MAP      128
#define PIC_ARRAY_FILTER   129
#define PIC_ARRAY_REDUCE   130
#define PIC_ARRAY_EACH     131
#define PIC_ARRAY_SORT     132
#define PIC_ARRAY_FOR_EACH 133
#define PIC_ARRAY_FIND     134
#define PIC_ARRAY_ANY      135
#define PIC_ARRAY_ALL      136
#define PIC_ARRAY_FLAT_MAP 137
#define PIC_ARRAY_SORT_BY  138
#define PIC_ARRAY_GROUP_BY 139

/* String methods */
#define PIC_STRING_LEN         30
#define PIC_STRING_LENGTH      31
#define PIC_STRING_SPLIT       32
#define PIC_STRING_TRIM        33
#define PIC_STRING_TO_UPPER    34
#define PIC_STRING_TO_LOWER    35
#define PIC_STRING_STARTS_WITH 36
#define PIC_STRING_ENDS_WITH   37
#define PIC_STRING_REPLACE     38
#define PIC_STRING_CONTAINS    39
#define PIC_STRING_CHARS       40
#define PIC_STRING_BYTES       41
#define PIC_STRING_REVERSE     42
#define PIC_STRING_REPEAT      43
#define PIC_STRING_PAD_LEFT    44
#define PIC_STRING_PAD_RIGHT   45
#define PIC_STRING_COUNT       46
#define PIC_STRING_IS_EMPTY    47
#define PIC_STRING_INDEX_OF    48
#define PIC_STRING_SUBSTRING   49
#define PIC_STRING_TRIM_START  50
#define PIC_STRING_TRIM_END    51
#define PIC_STRING_CAPITALIZE  52
#define PIC_STRING_TITLE_CASE  53
#define PIC_STRING_SNAKE_CASE  54
#define PIC_STRING_CAMEL_CASE  55
#define PIC_STRING_KEBAB_CASE  56

/* Map methods */
#define PIC_MAP_LEN      60
#define PIC_MAP_KEYS     61
#define PIC_MAP_VALUES   62
#define PIC_MAP_ENTRIES  63
#define PIC_MAP_GET      64
#define PIC_MAP_HAS      65
#define PIC_MAP_REMOVE   66
#define PIC_MAP_MERGE    67
#define PIC_MAP_SET      68
#define PIC_MAP_CONTAINS 69
#define PIC_MAP_LENGTH   70

/* Buffer methods */
#define PIC_BUFFER_LEN       75
#define PIC_BUFFER_PUSH      76
#define PIC_BUFFER_PUSH_U16  77
#define PIC_BUFFER_PUSH_U32  78
#define PIC_BUFFER_READ_U8   79
#define PIC_BUFFER_WRITE_U8  80
#define PIC_BUFFER_READ_U16  81
#define PIC_BUFFER_WRITE_U16 82
#define PIC_BUFFER_READ_U32  83
#define PIC_BUFFER_WRITE_U32 84
#define PIC_BUFFER_SLICE     85
#define PIC_BUFFER_LENGTH    86
#define PIC_BUFFER_CLEAR     87
#define PIC_BUFFER_FILL      88
#define PIC_BUFFER_RESIZE    89
#define PIC_BUFFER_TO_STRING 90
#define PIC_BUFFER_TO_ARRAY  91
#define PIC_BUFFER_TO_HEX    92
#define PIC_BUFFER_READ_I8   93
#define PIC_BUFFER_READ_I16  94
#define PIC_BUFFER_READ_I32  95
#define PIC_BUFFER_READ_F32  96
#define PIC_BUFFER_READ_F64  97
#define PIC_BUFFER_CAPACITY  98

/* Set methods */
#define PIC_SET_LEN                  100
#define PIC_SET_HAS                  101
#define PIC_SET_ADD                  102
#define PIC_SET_REMOVE               103
#define PIC_SET_TO_ARRAY             104
#define PIC_SET_UNION                105
#define PIC_SET_INTERSECTION         106
#define PIC_SET_DIFFERENCE           107
#define PIC_SET_IS_SUBSET            108
#define PIC_SET_IS_SUPERSET          109
#define PIC_SET_LENGTH               110
#define PIC_SET_CONTAINS             111
#define PIC_SET_SYMMETRIC_DIFFERENCE 112

/* Enum methods */
#define PIC_ENUM_TAG          115
#define PIC_ENUM_NAME         116
#define PIC_ENUM_PAYLOAD      117
#define PIC_ENUM_IS_VARIANT   118
#define PIC_ENUM_VARIANT_NAME 119

/* Channel methods */
#define PIC_CHANNEL_SEND  120
#define PIC_CHANNEL_RECV  121
#define PIC_CHANNEL_CLOSE 122

/* Range methods */
#define PIC_RANGE_CONTAINS 123
#define PIC_RANGE_TO_ARRAY 124

/* Ref methods */
#define PIC_REF_DEREF      125
#define PIC_REF_INNER_TYPE 126

/* Special: not a builtin method — indicates the method was NOT found in
 * builtin dispatch and the VM should fall through to struct/map/impl lookup. */
#define PIC_NOT_BUILTIN 255

#endif /* INLINE_CACHE_H */
