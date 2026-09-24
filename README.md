# bongOS

A from-scratch x86_64 desktop operating system in C and assembly. Custom bootloaders
(UEFI + legacy BIOS), a hybrid multicore kernel with capability handles, its own journaling
filesystem, networking with an SSH server, and a dark Plasma-style desktop. Every part of the
base system is written here, from the first boot sector up.

> **Status:** early development. See [`docs/STATUS.md`](docs/STATUS.md) and the
> [roadmap](docs/ROADMAP.md).

## Build and run
```sh
sudo bash tools/ci/install-deps.sh   # Ubuntu 24.04
make image                           # -> build/bongos.img
make run                             # boot in QEMU (UEFI)
make run-bios                        # boot in QEMU (legacy BIOS)
make test                            # automated boot tests
```
To try it on real hardware, write `build/bongos.img` (or a release image) to a USB stick and
boot it in UEFI mode. bongOS refuses to write to any disk other than the one it booted from.

## Docs
- [Architecture](docs/ARCHITECTURE.md)
- [Decision log](docs/DECISIONS.md)
- [Roadmap](docs/ROADMAP.md) (phases are named after strains; v1.0 is "OG Kush")

## Security note
bongOS's cryptography (TLS, SSH, disk encryption) is custom and **experimental**. It is
tested against official test vectors and real clients, but it has not been audited. Don't
trust it with real secrets.

## License
MIT. See [LICENSE](LICENSE).
