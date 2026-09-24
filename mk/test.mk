# make test / make test-full: the boot matrix (ARCHITECTURE §23), run through
# tests/harness/run-matrix.sh. A no-op until M1.2 produces build/bongos.img (D-053).
.PHONY: test test-full
test: image
	@if [ -f $(IMAGE) ]; then \
		tests/harness/run-matrix.sh tests/harness/matrix.conf --image $(IMAGE); \
	else \
		echo "test: no image yet (M1.2 produces $(IMAGE)); nothing to boot-test"; \
	fi

test-full: image
	@if [ -f $(IMAGE) ]; then \
		tests/harness/run-matrix.sh tests/harness/matrix-full.conf --image $(IMAGE); \
	else \
		echo "test-full: no image yet (M1.2 produces $(IMAGE)); nothing to boot-test"; \
	fi
