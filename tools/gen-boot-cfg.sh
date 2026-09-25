#!/bin/bash
# Generates build/boot.cfg from boot/boot.cfg.in by substituting @BRANDING_NAME@ with
# branding/name's contents (ARCHITECTURE §5.2, D-067/D-046: the OS name lives only in
# branding/, so boot.cfg.in is a template rather than hardcoding it). Mirrors
# tools/gen-branding.sh's own clean/escape approach.
set -euo pipefail

clean() { head -n1 | tr -d '\r'; }

name=$(clean < branding/name)
# sed's replacement text treats '&', '\', and the delimiter specially; escape all three so a
# branding/name containing any of them substitutes literally instead of corrupting the template.
nameEscaped=$(printf '%s' "$name" | sed -e 's/[&\\]/\\&/g')

sed "s/@BRANDING_NAME@/$nameEscaped/g" boot/boot.cfg.in
