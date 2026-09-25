/* Status: the kernel's only error-reporting type (ARCHITECTURE §4). Negative means error; the
 * kernel has no errno. Shared with userspace (kernel/include/uapi/), since syscalls return this
 * same type. Kept minimal in M1.3 (just enough for bootInfoValidate and paging self-checks);
 * later milestones append more codes here, never renumber existing ones. */
#ifndef KERNEL_UAPI_STATUS_H
#define KERNEL_UAPI_STATUS_H

#include <stdint.h>

typedef int32_t Status;

#define STATUS_OK              0
#define STATUS_ERR_INVALID     (-1) /* a bad argument or a malformed structure (e.g. BootInfo) */
#define STATUS_ERR_NOT_FOUND   (-2)
#define STATUS_ERR_NO_MEMORY   (-3)
#define STATUS_ERR_UNSUPPORTED (-4)

#endif
