# make test / make test-full: the boot matrix (ARCHITECTURE §23), run through
# tests/harness/run-matrix.sh, against build/bongos-ktest.img (mk/image.mk): the same image as
# `make image` but with tests/harness/ktest-boot.cfg baked in, so the kernel runs `ktest=all` and
# reports PASS/FAIL through the real isa-debug-exit/KTEST protocol (D-063) instead of the
# --expect-serial banner-match bridge M1.2 used before the kernel existed (D-057). The release/USB
# image (build/bongos.img) never runs ktests or writes port 0xF4.
.PHONY: test test-full
test: image
	tests/harness/run-matrix.sh tests/harness/matrix.conf --image $(KTEST_IMAGE)

test-full: image
	tests/harness/run-matrix.sh tests/harness/matrix-full.conf --image $(KTEST_IMAGE)
