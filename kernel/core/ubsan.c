/* UBSan runtime for the kernel (ARCHITECTURE §3/§21, D-075). Compiled out entirely in release
 * builds (KERNEL_UBSAN=0, mk/kernel.mk) since -fsanitize=undefined isn't applied there and
 * nothing calls these. Compiled itself with -fno-sanitize=all (mk/kernel.mk): a handler that
 * itself tripped a check it's reporting would recurse.
 *
 * Every check's own "Data" struct begins with a `SourceLocation` (the LLVM compiler-rt ABI,
 * stable across the handler set) except __ubsan_handle_nonnull_return_v1, whose call site passes
 * the report location as a separate argument instead -- handled as a special case below. Every
 * handler here is the `_abort` variant only: the kernel builds with -fno-sanitize-recover=all, so
 * clang only ever emits calls to those symbols; the plain (non-abort) names are defined too,
 * delegating to the same abort path, purely as a defensive fallback. */
#include "panic.h"

#include <stddef.h>
#include <stdint.h>

#if KERNEL_UBSAN

typedef struct SourceLocation {
    const char *filename;
    uint32_t line;
    uint32_t column;
} SourceLocation;

typedef struct TypeDescriptor {
    uint16_t typeKind;
    uint16_t typeInfo;
    char typeName[1];
} TypeDescriptor;

typedef uintptr_t UbsanValue;

static _Noreturn void ubsanAbort(const SourceLocation *loc, const char *what) {
    const char *file = (loc != NULL && loc->filename != NULL) ? loc->filename : "?";
    uint32_t line = loc != NULL ? loc->line : 0;
    uint32_t col = loc != NULL ? loc->column : 0;
    panic("ubsan: %s at %s:%u:%u", what, file, line, col);
}

typedef struct TypeMismatchData {
    SourceLocation loc;
    const TypeDescriptor *type;
    unsigned char logAlignment;
    unsigned char typeCheckKind;
} TypeMismatchData;

void __ubsan_handle_type_mismatch_v1_abort(TypeMismatchData *d, UbsanValue ptr) {
    const char *what = "type mismatch";
    if (ptr == 0) {
        what = "null pointer access";
    } else if (d->logAlignment != 0 && (ptr & (((UbsanValue)1 << d->logAlignment) - 1)) != 0) {
        what = "misaligned access";
    } else if (d->typeCheckKind == 1) { /* TCK_MemberAccess and friends land elsewhere; kind 1 is
                                         * the common "insufficient object size" case */
        what = "insufficient object size for access";
    }
    ubsanAbort(&d->loc, what);
}
void __ubsan_handle_type_mismatch_v1(TypeMismatchData *d, UbsanValue ptr) {
    __ubsan_handle_type_mismatch_v1_abort(d, ptr);
}

typedef struct OverflowData {
    SourceLocation loc;
    const TypeDescriptor *type;
} OverflowData;

#define OVERFLOW_HANDLER(name, message)                                                            \
    void __ubsan_handle_##name##_abort(OverflowData *d, UbsanValue a, UbsanValue b) {              \
        (void)a;                                                                                   \
        (void)b;                                                                                   \
        ubsanAbort(&d->loc, message);                                                              \
    }                                                                                              \
    void __ubsan_handle_##name(OverflowData *d, UbsanValue a, UbsanValue b) {                      \
        __ubsan_handle_##name##_abort(d, a, b);                                                    \
    }

OVERFLOW_HANDLER(add_overflow, "signed integer overflow")
OVERFLOW_HANDLER(sub_overflow, "signed integer overflow")
OVERFLOW_HANDLER(mul_overflow, "signed integer overflow")
OVERFLOW_HANDLER(divrem_overflow, "integer divide overflow")

void __ubsan_handle_negate_overflow_abort(OverflowData *d, UbsanValue v) {
    (void)v;
    ubsanAbort(&d->loc, "signed integer overflow (negation)");
}
void __ubsan_handle_negate_overflow(OverflowData *d, UbsanValue v) {
    __ubsan_handle_negate_overflow_abort(d, v);
}

typedef struct ShiftOutOfBoundsData {
    SourceLocation loc;
    const TypeDescriptor *lhsType;
    const TypeDescriptor *rhsType;
} ShiftOutOfBoundsData;

void __ubsan_handle_shift_out_of_bounds_abort(ShiftOutOfBoundsData *d, UbsanValue lhs,
                                              UbsanValue rhs) {
    (void)lhs;
    (void)rhs;
    ubsanAbort(&d->loc, "shift out of bounds");
}
void __ubsan_handle_shift_out_of_bounds(ShiftOutOfBoundsData *d, UbsanValue lhs, UbsanValue rhs) {
    __ubsan_handle_shift_out_of_bounds_abort(d, lhs, rhs);
}

typedef struct OutOfBoundsData {
    SourceLocation loc;
    const TypeDescriptor *arrayType;
    const TypeDescriptor *indexType;
} OutOfBoundsData;

void __ubsan_handle_out_of_bounds_abort(OutOfBoundsData *d, UbsanValue index) {
    (void)index;
    ubsanAbort(&d->loc, "array index out of bounds");
}
void __ubsan_handle_out_of_bounds(OutOfBoundsData *d, UbsanValue index) {
    __ubsan_handle_out_of_bounds_abort(d, index);
}

typedef struct InvalidValueData {
    SourceLocation loc;
    const TypeDescriptor *type;
} InvalidValueData;

void __ubsan_handle_load_invalid_value_abort(InvalidValueData *d, UbsanValue val) {
    (void)val;
    ubsanAbort(&d->loc, "invalid enum/bool load");
}
void __ubsan_handle_load_invalid_value(InvalidValueData *d, UbsanValue val) {
    __ubsan_handle_load_invalid_value_abort(d, val);
}

typedef struct VlaBoundData {
    SourceLocation loc;
    const TypeDescriptor *type;
} VlaBoundData;

void __ubsan_handle_vla_bound_not_positive_abort(VlaBoundData *d, UbsanValue bound) {
    (void)bound;
    ubsanAbort(&d->loc, "VLA bound not positive");
}
void __ubsan_handle_vla_bound_not_positive(VlaBoundData *d, UbsanValue bound) {
    __ubsan_handle_vla_bound_not_positive_abort(d, bound);
}

typedef struct NonNullArgData {
    SourceLocation loc;
    SourceLocation attrLoc;
    int argIndex;
} NonNullArgData;

void __ubsan_handle_nonnull_arg_abort(NonNullArgData *d) {
    ubsanAbort(&d->loc, "null argument to a nonnull parameter");
}
void __ubsan_handle_nonnull_arg(NonNullArgData *d) {
    __ubsan_handle_nonnull_arg_abort(d);
}

typedef struct NonNullReturnData {
    SourceLocation attrLoc;
} NonNullReturnData;

/* Unlike every other handler, the report location is passed separately (the Data struct only
 * carries the attribute's own location). */
void __ubsan_handle_nonnull_return_v1_abort(NonNullReturnData *d, SourceLocation *loc) {
    (void)d;
    ubsanAbort(loc, "null return from a nonnull-returning function");
}
void __ubsan_handle_nonnull_return_v1(NonNullReturnData *d, SourceLocation *loc) {
    __ubsan_handle_nonnull_return_v1_abort(d, loc);
}

typedef struct PointerOverflowData {
    SourceLocation loc;
} PointerOverflowData;

void __ubsan_handle_pointer_overflow_abort(PointerOverflowData *d, UbsanValue base,
                                           UbsanValue result) {
    (void)base;
    (void)result;
    ubsanAbort(&d->loc, "pointer overflow");
}
void __ubsan_handle_pointer_overflow(PointerOverflowData *d, UbsanValue base, UbsanValue result) {
    __ubsan_handle_pointer_overflow_abort(d, base, result);
}

typedef struct InvalidBuiltinData {
    SourceLocation loc;
    unsigned char kind;
} InvalidBuiltinData;

void __ubsan_handle_invalid_builtin_abort(InvalidBuiltinData *d) {
    ubsanAbort(&d->loc, "invalid use of a builtin function");
}
void __ubsan_handle_invalid_builtin(InvalidBuiltinData *d) {
    __ubsan_handle_invalid_builtin_abort(d);
}

typedef struct AlignmentAssumptionData {
    SourceLocation loc;
    SourceLocation assumptionLoc;
    const TypeDescriptor *type;
} AlignmentAssumptionData;

void __ubsan_handle_alignment_assumption_abort(AlignmentAssumptionData *d, UbsanValue pointer,
                                               UbsanValue alignment, UbsanValue offset) {
    (void)pointer;
    (void)alignment;
    (void)offset;
    ubsanAbort(&d->loc, "failed alignment assumption");
}
void __ubsan_handle_alignment_assumption(AlignmentAssumptionData *d, UbsanValue pointer,
                                         UbsanValue alignment, UbsanValue offset) {
    __ubsan_handle_alignment_assumption_abort(d, pointer, alignment, offset);
}

typedef struct UnreachableData {
    SourceLocation loc;
} UnreachableData;

/* No recoverable/abort split -- reaching __builtin_unreachable() is always fatal. */
void __ubsan_handle_builtin_unreachable(UnreachableData *d) {
    ubsanAbort(&d->loc, "reached __builtin_unreachable()");
}

#endif /* KERNEL_UBSAN */
