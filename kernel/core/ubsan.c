/* UBSan runtime (ARCHITECTURE §3, D-076): the `__ubsan_handle_*` entry points clang calls when
 * `-fsanitize=...` instrumentation catches undefined behavior. Only in debug builds
 * (KERNEL_UBSAN, mk/kernel.mk). The data structures below are the compiler ABI (stable across
 * clang/GCC UBSan for years), written from that ABI rather than copied from any runtime -- see
 * llvm-project's compiler-rt/lib/ubsan/ubsan_handlers.h for the reference shape, which this
 * mirrors structurally without reusing any of its code.
 *
 * Policy (D-076): every check always panics. This is a debug/CI-only build; undefined behavior is
 * always a bug here, never something to log past. Before panicking, offers the trip to
 * archTrapCatchSoftware() (D-078) so a ktest can deliberately trigger one and prove it's detected
 * instead of ending the whole ktest run -- offered *before* the recursion guard below is set, so a
 * caught trip never leaves that guard wrongly latched for a later, unrelated real trip. A
 * recursion guard means a UBSan trip *inside* this file's own reporting path (which shouldn't
 * happen, since this file itself is compiled with -fno-sanitize=all) can't recurse; it can still
 * happen if panic()'s own call chain trips one, so this guards against that. */
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
 * and what kind of UB happened", not "reproduce the exact operand values in decimal".
 *
 * `pc` is the *calling* `__ubsan_handle_*` function's own return address
 * (`__builtin_return_address(0)`, taken at each call site below, not in here) -- report() is a
 * shared helper several handlers call, so `__builtin_return_address(0)` evaluated inside report()
 * itself would only ever point at whichever handler called it, never at the instrumented user code
 * that actually tripped the check. Every handler is marked `noinline` so this stays a real
 * `__builtin_return_address(0)` measurement at a real call site, not something that shifts meaning
 * if a handler gets folded into its own caller. */
static _Noreturn void report(const char *check, const SourceLocation *loc, const char *detail,
                             uint64_t pc) {
    if (ubsanReporting) {
        /* A UBSan trip while already reporting one -- panic() itself has its own recursion guard
         * (panicEnter()/panicNested()), so just get there directly with a minimal message. */
        panic("UBSAN: recursive trip (%s)", check);
    }

    /* Offers the trip to a ktest-armed archTrapCatch(TRAP_CATCH_UBSAN, ...) *before* touching the
     * recursion guard (D-078): a caught trip redirects execution away via archTrapCatchSoftware()
     * and never reaches the `ubsanReporting = 1` below, so the guard stays correctly clear for
     * whatever real (uncaught) trip comes next -- setting it first would wrongly panic that next
     * trip as "recursive" even though this one was cleanly caught, not left mid-report. A caught
     * trip is deliberately silent on serial (the ktest itself reports pass/fail); only an
     * uncaught one panics loudly below. */
    archTrapCatchSoftware(TRAP_CATCH_UBSAN, pc);
    ubsanReporting = 1;

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

__attribute__((noinline)) void __ubsan_handle_add_overflow(OverflowData *data, ValueHandle lhs,
                                                           ValueHandle rhs) {
    char detail[64];
    ksnprintf(detail, sizeof(detail), "lhs=0x%llx rhs=0x%llx", (unsigned long long)lhs,
              (unsigned long long)rhs);
    report("addition overflow", &data->loc, detail,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}
__attribute__((noinline)) void __ubsan_handle_sub_overflow(OverflowData *data, ValueHandle lhs,
                                                           ValueHandle rhs) {
    char detail[64];
    ksnprintf(detail, sizeof(detail), "lhs=0x%llx rhs=0x%llx", (unsigned long long)lhs,
              (unsigned long long)rhs);
    report("subtraction overflow", &data->loc, detail,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}
__attribute__((noinline)) void __ubsan_handle_mul_overflow(OverflowData *data, ValueHandle lhs,
                                                           ValueHandle rhs) {
    char detail[64];
    ksnprintf(detail, sizeof(detail), "lhs=0x%llx rhs=0x%llx", (unsigned long long)lhs,
              (unsigned long long)rhs);
    report("multiplication overflow", &data->loc, detail,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}
__attribute__((noinline)) void __ubsan_handle_negate_overflow(OverflowData *data,
                                                              ValueHandle oldVal) {
    char detail[64];
    formatValue(detail, sizeof(detail), oldVal);
    report("negation overflow", &data->loc, detail,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}
__attribute__((noinline)) void __ubsan_handle_divrem_overflow(OverflowData *data, ValueHandle lhs,
                                                              ValueHandle rhs) {
    char detail[64];
    ksnprintf(detail, sizeof(detail), "lhs=0x%llx rhs=0x%llx", (unsigned long long)lhs,
              (unsigned long long)rhs);
    report("division/remainder overflow (or divide by zero)", &data->loc, detail,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}

/* ---- shift ---- */
typedef struct {
    SourceLocation loc;
    const TypeDescriptor *lhsType;
    const TypeDescriptor *rhsType;
} ShiftOutOfBoundsData;

__attribute__((noinline)) void
__ubsan_handle_shift_out_of_bounds(ShiftOutOfBoundsData *data, ValueHandle lhs, ValueHandle rhs) {
    char detail[64];
    ksnprintf(detail, sizeof(detail), "lhs=0x%llx shift=0x%llx", (unsigned long long)lhs,
              (unsigned long long)rhs);
    report("shift out of bounds", &data->loc, detail,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}

/* ---- bounds ---- */
typedef struct {
    SourceLocation loc;
    const TypeDescriptor *arrayType;
    const TypeDescriptor *indexType;
} OutOfBoundsData;

__attribute__((noinline)) void __ubsan_handle_out_of_bounds(OutOfBoundsData *data,
                                                            ValueHandle index) {
    char detail[64];
    formatValue(detail, sizeof(detail), index);
    report("array index out of bounds", &data->loc, detail,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}

/* ---- null/alignment/object-size (type_mismatch_v1 covers all three: kind decoded from
 * `logAlignment`/`typeCheckKind`) ---- */
typedef struct {
    SourceLocation loc;
    const TypeDescriptor *type;
    uint8_t logAlignment;
    uint8_t typeCheckKind;
} TypeMismatchData;

__attribute__((noinline)) void __ubsan_handle_type_mismatch_v1(TypeMismatchData *data,
                                                               ValueHandle pointer) {
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
    report(check, &data->loc, detail, (uint64_t)(uintptr_t)__builtin_return_address(0));
}

/* ---- enum / bool ("load_invalid_value" covers both -fsanitize=enum and =bool) ---- */
typedef struct {
    SourceLocation loc;
    const TypeDescriptor *type;
} InvalidValueData;

__attribute__((noinline)) void __ubsan_handle_load_invalid_value(InvalidValueData *data,
                                                                 ValueHandle val) {
    char detail[64];
    formatValue(detail, sizeof(detail), val);
    report("invalid enum/bool value loaded", &data->loc, detail,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}

/* ---- nonnull-attribute / returns-nonnull-attribute ---- */
typedef struct {
    SourceLocation loc;
    SourceLocation attrLoc;
    int argIndex;
} NonNullArgData;

__attribute__((noinline)) void __ubsan_handle_nonnull_arg(NonNullArgData *data) {
    char detail[32];
    ksnprintf(detail, sizeof(detail), "arg #%d", data->argIndex);
    report("null argument to nonnull parameter", &data->loc, detail,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}

typedef struct {
    SourceLocation loc;
} NonNullReturnData;

__attribute__((noinline)) void __ubsan_handle_nonnull_return_v1(NonNullReturnData *data,
                                                                const SourceLocation *loc) {
    (void)loc; /* the caller's return-statement location; data->loc is the attribute's */
    report("null return from a nonnull-declared function", &data->loc, NULL,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}

/* ---- pointer-overflow ---- */
typedef struct {
    SourceLocation loc;
} PointerOverflowData;

__attribute__((noinline)) void
__ubsan_handle_pointer_overflow(PointerOverflowData *data, ValueHandle base, ValueHandle result) {
    char detail[64];
    ksnprintf(detail, sizeof(detail), "base=0x%llx result=0x%llx", (unsigned long long)base,
              (unsigned long long)result);
    report("pointer arithmetic overflow", &data->loc, detail,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}

/* ---- builtin (-fsanitize=builtin: e.g. __builtin_ctz(0)) ---- */
typedef struct {
    SourceLocation loc;
    uint8_t kind;
} InvalidBuiltinData;

__attribute__((noinline)) void __ubsan_handle_invalid_builtin(InvalidBuiltinData *data) {
    report("invalid argument to a builtin function", &data->loc, NULL,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}

/* ---- alignment_assumption (__builtin_assume_aligned) ---- */
typedef struct {
    SourceLocation loc;
    SourceLocation assumptionLoc;
    const TypeDescriptor *type;
} AlignmentAssumptionData;

__attribute__((noinline)) void __ubsan_handle_alignment_assumption(AlignmentAssumptionData *data,
                                                                   ValueHandle pointer,
                                                                   ValueHandle alignment,
                                                                   ValueHandle offset) {
    char detail[80];
    ksnprintf(detail, sizeof(detail), "pointer=0x%llx alignment=0x%llx offset=0x%llx",
              (unsigned long long)pointer, (unsigned long long)alignment,
              (unsigned long long)offset);
    report("alignment assumption violated", &data->loc, detail,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}

/* ---- vla-bound ---- */
typedef struct {
    SourceLocation loc;
    const TypeDescriptor *type;
} VLABoundData;

__attribute__((noinline)) void __ubsan_handle_vla_bound_not_positive(VLABoundData *data,
                                                                     ValueHandle bound) {
    char detail[64];
    formatValue(detail, sizeof(detail), bound);
    report("variable-length array bound is not positive", &data->loc, detail,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}

/* ---- unreachable (__builtin_unreachable actually reached) ---- */
typedef struct {
    SourceLocation loc;
} UnreachableData;

__attribute__((noinline)) _Noreturn void __ubsan_handle_builtin_unreachable(UnreachableData *data) {
    report("__builtin_unreachable() reached", &data->loc, NULL,
           (uint64_t)(uintptr_t)__builtin_return_address(0));
}
