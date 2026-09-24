# make test / make test-full: the boot matrix (ARCHITECTURE §23), run through
# tests/harness/run-matrix.sh. A no-op until M1.2 produces build/bongos.img (ROADMAP M1.1).
.PHONY: test test-full
test: image
	@if [ -f $(IMAGE) ]; then \
		tests/harness/run-matrix.sh tests/harness/matrix.conf; \
	else \
		echo "test: no image yet (M1.2 produces $(IMAGE)); nothing to boot-test"; \
	fi

test-full: image
	@if [ -f $(IMAGE) ]; then \
		tests/harness/run-matrix.sh tests/harness/matrix-full.conf; \
	else \
		echo "test-full: no image yet (M1.2 produces $(IMAGE)); nothing to boot-test"; \
	fi
