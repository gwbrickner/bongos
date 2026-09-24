# make run / run-bios / debug / gdb: interactive QEMU sessions for a human, via
# tests/harness/run-qemu.sh. Each needs build/bongos.img (M1.2). `gdb` gains symbol loading in
# M2.1, once the kernel has a symbol table to embed.
.PHONY: run run-bios debug gdb
run: image
	@[ -f $(IMAGE) ] || { echo "run: no image yet -- run 'make image' once M1.2 lands (see ROADMAP.md)"; exit 1; }
	tests/harness/run-qemu.sh --image $(IMAGE) --fw uefi --interactive

run-bios: image
	@[ -f $(IMAGE) ] || { echo "run-bios: no image yet -- run 'make image' once M1.2 lands (see ROADMAP.md)"; exit 1; }
	tests/harness/run-qemu.sh --image $(IMAGE) --fw bios --interactive

debug: image
	@[ -f $(IMAGE) ] || { echo "debug: no image yet -- run 'make image' once M1.2 lands (see ROADMAP.md)"; exit 1; }
	tests/harness/run-qemu.sh --image $(IMAGE) --fw uefi --debug --interactive

gdb: image
	@[ -f $(IMAGE) ] || { echo "gdb: no image yet -- run 'make image' once M1.2 lands (see ROADMAP.md)"; exit 1; }
	tests/harness/run-qemu.sh --image $(IMAGE) --fw uefi --interactive --extra "-s -S" & \
	sleep 1; \
	gdb -ex "target remote :1234"
