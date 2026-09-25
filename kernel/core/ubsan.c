/* UBSan runtime (ARCHITECTURE §3, D-076): the `__ubsan_handle_*` entry points clang calls when
 * `-fsanitize=...` instrumentation catches undefined behavior. Only in debug builds
 * (KERNEL_UBSAN, mk/kernel.mk). The data structures below are the compiler ABI (stable across
 * clang/GCC UBSan for years), written from that ABI rather than copied from any runtime -- see
 * llvm-project's compiler-rt/lib/ubsan/ubsan_handlers.h for the reference shape, which this
 * mirrors structurally without reusing any of its code.
 *
 * Policy (D-076): every check always panics. This is a debug/CI-only build; undefined behavior is
 * always a bug here, never something to log past. Before panicking, offers the trip to
 * archTrapCatchSoftware() so a ktest can deliberately trigger one and prove it's detected instead
 * of ending the whole ktest run (D-078, not built yet -- archTrapCatchSoftware() doesn't exist
 * until then, so every trip panics unconditionally for now). A recursion guard means a UBSan trip
 * *inside* this file's own reporting path (which shouldn't happen, since this file itself is
 * compiled with -fno-sanitize=all) can't recurse; it can still happen if panic()'s own call chain
 * trips one, so this guards against that. */
#include "format.h"
#include "klog.h"
#include "panic.h"

#include <arch/trap.h>
#include <stdint.h>

typedef struct {
    const char *filename;
    uint32_t line;
    uint32_t column;
} SourceLocation;

typedef struct {
    uint16_t kind;
    uint16_t info;
    char name[];
} TypeDescriptor;

typedef uintptr_t ValueHandle;

static int ubsanReporting = 0;

/* Every value is reported as a raw hex `ValueHandle` rather than fully decoded into a signed/
 * unsigned integer of its real width: TypeDescriptor.info encodes whether the value is packed
 * inline or stored out-of-line via a pointer, and how many bits wide it is, decoding which is a
 * lot of ABI-fragile machinery for a debug-only diagnostic whose job is "tell a developer where
 * and what kind of UB happened", not "reproduce the exact operand values in decimal". */
static _Noreturn void report(const char *check, const SourceLocation *loc, const char *detail) {
    if (ubsanReporting) {
        /* A UBSan trip while already reporting one -- panic() itself has its own recursion guard
         * (panicEnter()/panicNested()), so just get there directly with a minimal message. */
        panic("UBSAN: recursive trip (%s)", check);
    }
    ubsanReporting = 1;

    /* Offers the trip to a ktest-armed archTrapCatch(TRAP_CATCH_UBSAN, ...) before printing
     * anything (D-078): if one is armed and claims it, archTrapCatchSoftware() redirects execution
     * back to that ktest's call site and never returns here. A caught trip is deliberately silent
     * on serial (the ktest itself reports pass/fail); only an uncaught one panics loudly below. */
    archTrapCatchSoftware(TRAP_CATCH_UBSAN, (uint64_t)(uintptr_t)__builtin_return_address(0));

    char msg[220];
    const char *file = (loc != NULL && loc->filename != NULL) ? loc->filename : "?";
    uint32_t line = (loc != NULL) ? loc->line : 0;
    uint32_t col = (loc != NULL) ? loc->column : 0;
    if (detail != NULL) {
        ksnprintf(msg, sizeof(msg), "UBSAN: %s at %s:%u:%u (%s)", check, file, line, col, detail);
    } else {
        ksnprintf(msg, sizeof(msg), "UBSAN: %s at %s:%u:%u", check, file, line, col);
    }
    panic("%s", msg);
}

static void formatValue(char *buf, size_t bufSize, ValueHandle v) {
    ksnprintf(buf, bufSize, "value=0x%llx", (unsigned long long)v);
}

/* ---- Overflow checks: add/sub/mul/negate/divrem (integer-divide-by-zero, signed-integer-
 * overflow) ---- */
typedef struct {
    SourceLocation loc;
    const TypeDescriptor *type;
} OverflowData;

void __ubsan_handle_add_overflow(OverflowData *data, ValueHandle lhs, ValueHandle rhs) {
    char detail[64];
    ksnprintf(detail, sizeof(detail), "lhs=0x%llx rhs=0x%llx", (unsigned long long)lhs,
              (unsigned long long)rhs);
    report("addition overflow", &data->loc, detail);
}
void __ubsan_handle_sub_overflow(OverflowData *data, ValueHandle lhs, ValueHandle rhs) {
    char detail[64];
    ksnprintf(detail, sizeof(detail), "lhs=0x%llx rhs=0x%llx", (unsigned long long)lhs,
              (unsigned long long)rhs);
    report("subtraction overflow", &data->loc, detail);
}
void __ubsan_handle_mul_overflow(OverflowData *data, ValueHandle lhs, ValueHandle rhs) {
    char detail[64];
    ksnprintf(detail, sizeof(detail), "lhs=0x%llx rhs=0x%llx", (unsigned long long)lhs,
              (unsigned long long)rhs);
    report("multiplication overflow", &data->loc, detail);
}
void __ubsan_handle_negate_overflow(OverflowData *data, ValueHandle oldVal) {
    char detail[64];
    formatValue(detail, sizeof(detail), oldVal);
    report("negation overflow", &data->loc, detail);
}
void __ubsan_handle_divrem_overflow(OverflowData *data, ValueHandle lhs, ValueHandle rhs) {
    char detail[64];
    ksnprintf(detail, sizeof(detail), "lhs=0x%llx rhs=0x%llx", (unsigned long long)lhs,
              (unsigned long long)rhs);
    report("division/remainder overflow (or divide by zero)", &data->loc, detail);
}

/* ---- shift ---- */
typedef struct {
    SourceLocation loc;
    const TypeDescriptor *lhsType;
    const TypeDescriptor *rhsType;
} ShiftOutOfBoundsData;

void __ubsan_handle_shift_out_of_bounds(ShiftOutOfBoundsData *data, ValueHandle lhs,
                                        ValueHandle rhs) {
    char detail[64];
    ksnprintf(detail, sizeof(detail), "lhs=0x%llx shift=0x%llx", (unsigned long long)lhs,
              (unsigned long long)rhs);
    report("shift out of bounds", &data->loc, detail);
}

/* ---- bounds ---- */
typedef struct {
    SourceLocation loc;
    const TypeDescriptor *arrayType;
    const TypeDescriptor *indexType;
} OutOfBoundsData;

void __ubsan_handle_out_of_bounds(OutOfBoundsData *data, ValueHandle index) {
    char detail[64];
    formatValue(detail, sizeof(detail), index);
    report("array index out of bounds", &data->loc, detail);
}

/* ---- null/alignment/object-size (type_mismatch_v1 covers all three: kind decoded from
 * `logAlignment`/`typeCheckKind`) ---- */
typedef struct {
    SourceLocation loc;
    const TypeDescriptor *type;
    uint8_t logAlignment;
    uint8_t typeCheckKind;
} TypeMismatchData;

void __ubsan_handle_type_mismatch_v1(TypeMismatchData *data, ValueHandle pointer) {
    const char *check;
    if (pointer == 0) {
        check = "null pointer access";
    } else if (data->logAlignment != 0 && (pointer & ((1ULL << data->logAlignment) - 1)) != 0) {
        check = "misaligned pointer access";
    } else {
        check = "insufficient object size for type";
    }
    char detail[48];
    ksnprintf(detail, sizeof(detail), "pointer=0x%llx", (unsigned long long)pointer);
    report(check, &data->loc, detail);
}

/* ---- enum / bool ("load_invalid_value" covers both -fsanitize=enum and =bool) ---- */
typedef struct {
    SourceLocation loc;
    const TypeDescriptor *type;
} InvalidValueData;

void __ubsan_handle_load_invalid_value(InvalidValueData *data, ValueHandle val) {
    char detail[64];
    formatValue(detail, sizeof(detail), val);
    report("invalid enum/bool value loaded", &data->loc, detail);
}

/* ---- nonnull-attribute / returns-nonnull-attribute ---- */
typedef struct {
    SourceLocation loc;
    SourceLocation attrLoc;
    int argIndex;
} NonNullArgData;

void __ubsan_handle_nonnull_arg(NonNullArgData *data) {
    char detail[32];
    ksnprintf(detail, sizeof(detail), "arg #%d", data->argIndex);
    report("null argument to nonnull parameter", &data->loc, detail);
}

typedef struct {
    SourceLocation loc;
} NonNullReturnData;

void __ubsan_handle_nonnull_return_v1(NonNullReturnData *data, const SourceLocation *loc) {
    (void)loc; /* the caller's return-statement location; data->loc is the attribute's */
    report("null return from a nonnull-declared function", &data->loc, NULL);
}

/* ---- pointer-overflow ---- */
typedef struct {
    SourceLocation loc;
} PointerOverflowData;

void __ubsan_handle_pointer_overflow(PointerOverflowData *data, ValueHandle base,
                                     ValueHandle result) {
    char detail[64];
    ksnprintf(detail, sizeof(detail), "base=0x%llx result=0x%llx", (unsigned long long)base,
              (unsigned long long)result);
    report("pointer arithmetic overflow", &data->loc, detail);
}

/* ---- builtin (-fsanitize=builtin: e.g. __builtin_ctz(0)) ---- */
typedef struct {
    SourceLocation loc;
    uint8_t kind;
} InvalidBuiltinData;

void __ubsan_handle_invalid_builtin(InvalidBuiltinData *data) {
    report("invalid argument to a builtin function", &data->loc, NULL);
}

/* ---- alignment_assumption (__builtin_assume_aligned) ---- */
typedef struct {
    SourceLocation loc;
    SourceLocation assumptionLoc;
    const TypeDescriptor *type;
} AlignmentAssumptionData;

void __ubsan_handle_alignment_assumption(AlignmentAssumptionData *data, ValueHandle pointer,
                                         ValueHandle alignment, ValueHandle offset) {
    char detail[80];
    ksnprintf(detail, sizeof(detail), "pointer=0x%llx alignment=0x%llx offset=0x%llx",
              (unsigned long long)pointer, (unsigned long long)alignment,
              (unsigned long long)offset);
    report("alignment assumption violated", &data->loc, detail);
}

/* ---- vla-bound ---- */
typedef struct {
    SourceLocation loc;
    const TypeDescriptor *type;
} VLABoundData;

void __ubsan_handle_vla_bound_not_positive(VLABoundData *data, ValueHandle bound) {
    char detail[64];
    formatValue(detail, sizeof(detail), bound);
    report("variable-length array bound is not positive", &data->loc, detail);
}

/* ---- unreachable (__builtin_unreachable actually reached) ---- */
typedef struct {
    SourceLocation loc;
} UnreachableData;

_Noreturn void __ubsan_handle_builtin_unreachable(UnreachableData *data) {
    report("__builtin_unreachable() reached", &data->loc, NULL);
}
