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

/* ── Thread-safety (Stage 5 / LAT-457) ──
 *
 * Chunks are SHARED across spawn threads (spawn sub-chunks and fn-chunk
 * constants are passed by pointer to child VMs), so PIC reads and writes
 * race between sibling threads. Each entry is therefore a single packed
 * 64-bit word accessed with relaxed atomics:
 *
 *     [63..48] type_tag   [47..16] method_hash   [15..0] handler_id
 *
 * Word 0 == empty (handler_id 0 is never stored: pic_update is only called
 * with a real handler id or PIC_NOT_BUILTIN). Relaxed is sufficient: the
 * handler id is a PURE FUNCTION of (type_tag, mhash), so a stale or lost
 * write can only cause a cache miss and re-resolve - never a wrong
 * dispatch - and word-sized atomicity rules out torn reads. The lazily
 * allocated slots array is published with a release CAS and consumed with
 * acquire loads (a losing allocator frees its block). */
typedef uint64_t PICWord;

#define PIC_PACK(tag, mhash, hid) (((uint64_t)(tag) << 48) | ((uint64_t)(mhash) << 16) | (uint64_t)(hid))
#define PIC_KEY_MASK              (~(uint64_t)0xFFFF)

/* Per-call-site inline cache: PIC_SIZE packed entry words. */
typedef struct {
    PICWord entries[PIC_SIZE];
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

/* Free a PIC table (teardown is single-threaded: a chunk is only freed
 * after every thread that could touch it has been joined). */
static inline void pic_table_free(PICTable *t) {
    if (t->slots) {
        free(t->slots);
        t->slots = NULL;
    }
}

/* Ensure the PIC table is allocated. Returns the slots pointer.
 * Race-safe lazy init: the winning thread publishes its calloc'd block
 * with a release CAS; losers free theirs and adopt the winner's. */
static inline PICSlot *pic_table_ensure(PICTable *t) {
    PICSlot *s = (PICSlot *)__atomic_load_n(&t->slots, __ATOMIC_ACQUIRE);
    if (!s) {
        PICSlot *fresh = (PICSlot *)calloc(PIC_DIRECT_SLOTS, sizeof(PICSlot));
        if (!fresh) return NULL;
        PICSlot *expected = NULL;
        if (__atomic_compare_exchange_n(&t->slots, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            s = fresh;
        } else {
            free(fresh);
            s = expected;
        }
    }
    return s;
}

/* Get the PIC slot for a given instruction offset.
 * Returns NULL if the table hasn't been allocated yet. */
static inline PICSlot *pic_slot_for(PICTable *t, size_t ip_offset) {
    PICSlot *s = (PICSlot *)__atomic_load_n(&t->slots, __ATOMIC_ACQUIRE);
    if (!s) return NULL;
    return &s[ip_offset & PIC_DIRECT_MASK];
}

/* Look up (type_tag, method_hash) in a PIC slot.
 * Returns the handler_id on hit, or 0 on miss. */
static inline uint16_t pic_lookup(const PICSlot *slot, uint8_t type_tag, uint32_t mhash) {
    uint64_t key = PIC_PACK(type_tag, mhash, 0);
    for (int i = 0; i < PIC_SIZE; i++) {
        uint64_t w = __atomic_load_n(&slot->entries[i], __ATOMIC_RELAXED);
        if (w == 0) return 0; /* first empty word ends the slot */
        if ((w & PIC_KEY_MASK) == key) return (uint16_t)(w & 0xFFFF);
    }
    return 0;
}

/* Update a PIC slot with a new entry: refresh a matching key, fill the
 * first empty word, or overwrite a key-chosen victim when full (the
 * eviction policy is a perf detail; concurrent racers may lose an update,
 * which only costs a later re-resolve). */
static inline void pic_update(PICSlot *slot, uint8_t type_tag, uint32_t mhash, uint16_t handler_id) {
    uint64_t key = PIC_PACK(type_tag, mhash, 0);
    uint64_t word = PIC_PACK(type_tag, mhash, handler_id);
    for (int i = 0; i < PIC_SIZE; i++) {
        uint64_t w = __atomic_load_n(&slot->entries[i], __ATOMIC_RELAXED);
        if (w == 0 || (w & PIC_KEY_MASK) == key) {
            __atomic_store_n(&slot->entries[i], word, __ATOMIC_RELAXED);
            return;
        }
    }
    __atomic_store_n(&slot->entries[mhash & (PIC_SIZE - 1)], word, __ATOMIC_RELAXED);
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
#define PIC_ARRAY_MAP        128
#define PIC_ARRAY_FILTER     129
#define PIC_ARRAY_REDUCE     130
#define PIC_ARRAY_EACH       131
#define PIC_ARRAY_SORT       132
#define PIC_ARRAY_FOR_EACH   133
#define PIC_ARRAY_FIND       134
#define PIC_ARRAY_ANY        135
#define PIC_ARRAY_ALL        136
#define PIC_ARRAY_FLAT_MAP   137
#define PIC_ARRAY_SORT_BY    138
#define PIC_ARRAY_GROUP_BY   139
#define PIC_ARRAY_FIND_INDEX 140
#define PIC_ARRAY_PARTITION  141

/* String methods */
#define PIC_STRING_LEN             30
#define PIC_STRING_LENGTH          31
#define PIC_STRING_SPLIT           32
#define PIC_STRING_TRIM            33
#define PIC_STRING_TO_UPPER        34
#define PIC_STRING_TO_LOWER        35
#define PIC_STRING_STARTS_WITH     36
#define PIC_STRING_ENDS_WITH       37
#define PIC_STRING_REPLACE         38
#define PIC_STRING_CONTAINS        39
#define PIC_STRING_CHARS           40
#define PIC_STRING_BYTES           41
#define PIC_STRING_REVERSE         42
#define PIC_STRING_REPEAT          43
#define PIC_STRING_PAD_LEFT        44
#define PIC_STRING_PAD_RIGHT       45
#define PIC_STRING_COUNT           46
#define PIC_STRING_IS_EMPTY        47
#define PIC_STRING_INDEX_OF        48
#define PIC_STRING_SUBSTRING       49
#define PIC_STRING_TRIM_START      50
#define PIC_STRING_TRIM_END        51
#define PIC_STRING_CAPITALIZE      52
#define PIC_STRING_TITLE_CASE      53
#define PIC_STRING_SNAKE_CASE      54
#define PIC_STRING_CAMEL_CASE      55
#define PIC_STRING_KEBAB_CASE      56
#define PIC_STRING_LAST_INDEX_OF   57
#define PIC_STRING_IS_ALPHA        58
#define PIC_STRING_IS_DIGIT        59
#define PIC_STRING_IS_ALPHANUMERIC 25

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
#define PIC_MAP_ANY      142
#define PIC_MAP_ALL      143

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
#define PIC_BUFFER_READ_U64  71
#define PIC_BUFFER_WRITE_U64 72
#define PIC_BUFFER_READ_I64  73
#define PIC_BUFFER_WRITE_I64 74

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
#define PIC_SET_CLEAR                113

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
