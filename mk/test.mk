# make test / make test-full: the boot matrix (ARCHITECTURE §23), run through
# tests/harness/run-matrix.sh.
#
# Until the kernel exists (M1.3) there's no isa-debug-exit/KTEST protocol to report PASS/FAIL, so
# the matrix runs in run-qemu.sh's --expect-serial banner-match mode instead (see that script).
# Two patterns, both required: the banner (which OVMF's ConOut also mirrors to serial on its
# own) and a marker the loader only ever writes over raw COM1 (boot/uefi/main.c) -- so this
# actually proves loaderSerialWriteString() works, not just that OVMF's console did. M1.3 must
# drop both --expect-serial flags and let the real KTEST protocol (isa-debug-exit) decide
# PASS/FAIL.
LOADER_EXPECT_BANNER := $(shell cat branding/name) loader
LOADER_EXPECT_COM1 := loader: com1 ok

.PHONY: test test-full
test: image
	tests/harness/run-matrix.sh tests/harness/matrix.conf --image $(IMAGE) \
		--expect-serial "$(LOADER_EXPECT_BANNER)" --expect-serial "$(LOADER_EXPECT_COM1)"

test-full: image
	tests/harness/run-matrix.sh tests/harness/matrix-full.conf --image $(IMAGE) \
		--expect-serial "$(LOADER_EXPECT_BANNER)" --expect-serial "$(LOADER_EXPECT_COM1)"
