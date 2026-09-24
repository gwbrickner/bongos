# make test / make test-full: the boot matrix (ARCHITECTURE §23), run through
# tests/harness/run-matrix.sh.
#
# Until the kernel exists (M1.3) there's no isa-debug-exit/KTEST protocol to report PASS/FAIL, so
# the matrix runs in run-qemu.sh's --expect-serial banner-match mode instead (see that script):
# success is the M1.2 loader's banner showing up on serial. M1.3 must drop LOADER_EXPECT_SERIAL
# and let the real KTEST protocol (isa-debug-exit) decide PASS/FAIL.
LOADER_EXPECT_SERIAL := bongOS loader

.PHONY: test test-full
test: image
	tests/harness/run-matrix.sh tests/harness/matrix.conf --image $(IMAGE) \
		--expect-serial "$(LOADER_EXPECT_SERIAL)"

test-full: image
	tests/harness/run-matrix.sh tests/harness/matrix-full.conf --image $(IMAGE) \
		--expect-serial "$(LOADER_EXPECT_SERIAL)"
