#!/bin/bash
# The one source of truth for build and test dependencies.
# - Claude Code cloud: paste this file into the environment's SETUP SCRIPT at claude.ai/code
#   (it runs as root on Ubuntu 24.04 and gets cached).
# - GitHub Actions: `sudo bash tools/ci/install-deps.sh`
# When a milestone needs a new tool, add it here AND tell the owner to update the cloud
# environment's setup script (note it in STATUS.md under "Waiting on owner").
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential make git python3 ca-certificates xz-utils \
  clang lld llvm clang-format nasm \
  qemu-system-x86 qemu-utils ovmf \
  mtools dosfstools gdisk xorriso e2fsprogs exfatprogs ntfs-3g \
  acpica-tools sbsigntool \
  openssh-client openssl curl netcat-openbsd
