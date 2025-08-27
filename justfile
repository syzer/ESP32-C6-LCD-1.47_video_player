# Justfile — MJPEG builder for ESP32-C6 1.47" player
# Requires: ffmpeg, bash
# Usage examples:
#   just all                  # convert every .mp4/.mov in mp4/ using "aspect" mode
#   just force-all            # convert every file forcing 172x320
#   just rotate-all           # rotate landscape → portrait then scale
#   just aspect foo.mp4       # convert single file with aspect-preserve
#   just force foo.mov        # force 172x320 for a single file
#   just rotate bar.mp4       # rotate+scale for a single file
#   just clean                # remove *.mjpeg in output dir
#   just ls                   # list input files it will process
#   just sd-sanitize /Volumes/SDCARD   # optional: minimize macOS junk on SD (non-destructive)

#---------------------------------------
# Config
#---------------------------------------
# Input and output directories (relative to this Justfile)
MP4_DIR := "mp4"
OUT_DIR := "SD_CONTENT2/mjpeg"

# Video parameters
FPS     := "24"
WIDTH   := "172"
HEIGHT  := "320"
Q       := "7"               # JPEG quality (1=best … 31=worst)
PIXFMT  := "yuvj420p"

#---------------------------------------
# Utilities
#---------------------------------------
set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

# Show which files will be processed
ls:
	@mkdir -p {{OUT_DIR}}
	@echo "Input dir: {{MP4_DIR}}"
	@echo "Output dir: {{OUT_DIR}}"
	@echo
	@shopt -s nullglob; \
	files=( {{MP4_DIR}}/*.mp4 {{MP4_DIR}}/*.MP4 {{MP4_DIR}}/*.mov {{MP4_DIR}}/*.MOV ); \
	if (( $${#files[@]} == 0 )); then \
	  echo "No input videos found in '{{MP4_DIR}}'"; \
	else \
	  printf "Found %d file(s):\n" "$${#files[@]}"; \
	  printf "  %s\n" "$${files[@]}"; \
	fi

#---------------------------------------
# Bulk conversions
#---------------------------------------

# Default target: convert all using aspect-preserving mode
default: all

# Keep aspect (scale width=172, height auto)
all: _ensure-out
	@shopt -s nullglob; \
	for f in {{MP4_DIR}}/*.mp4 {{MP4_DIR}}/*.MP4 {{MP4_DIR}}/*.mov {{MP4_DIR}}/*.MOV; do \
	  just aspect "$$f"; \
	done

# Force to exactly 172x320 (may stretch)
force-all: _ensure-out
	@shopt -s nullglob; \
	for f in {{MP4_DIR}}/*.mp4 {{MP4_DIR}}/*.MP4 {{MP4_DIR}}/*.mov {{MP4_DIR}}/*.MOV; do \
	  just force "$$f"; \
	done

# Rotate 90° clockwise then scale 172x320 (for landscape → portrait)
rotate-all: _ensure-out
	@shopt -s nullglob; \
	for f in {{MP4_DIR}}/*.mp4 {{MP4_DIR}}/*.MP4 {{MP4_DIR}}/*.mov {{MP4_DIR}}/*.MOV; do \
	  just rotate "$$f"; \
	done

#---------------------------------------
# Per-file conversions
#---------------------------------------

# just aspect path/to/video.mp4
aspect FILE:
	@mkdir -p {{OUT_DIR}}
	@in="$FILE"; \
	base="$${in##*/}"; \
	name="$${base%.*}"; \
	out="{{OUT_DIR}}/$${name}.mjpeg"; \
	echo "→ [aspect] $$in  →  $$out"; \
	ffmpeg -y -i "$$in" -pix_fmt {{PIXFMT}} -q:v {{Q}} \
	  -vf "fps={{FPS}},scale={{WIDTH}}:-1:flags=lanczos" \
	  "$$out"

# just force path/to/video.mov
force FILE:
	@mkdir -p {{OUT_DIR}}
	@in="$FILE"; \
	base="$${in##*/}"; \
	name="$${base%.*}"; \
	out="{{OUT_DIR}}/$${name}.mjpeg"; \
	echo "→ [force]  $$in  →  $$out"; \
	ffmpeg -y -i "$$in" -pix_fmt {{PIXFMT}} -q:v {{Q}} \
	  -vf "fps={{FPS}},scale={{WIDTH}}:{{HEIGHT}}:flags=lanczos" \
	  "$$out"

# just rotate path/to/video.mp4
rotate FILE:
	@mkdir -p {{OUT_DIR}}
	@in="$FILE"; \
	base="$${in##*/}"; \
	name="$${base%.*}"; \
	out="{{OUT_DIR}}/$${name}.mjpeg"; \
	echo "→ [rotate] $$in  →  $$out"; \
	ffmpeg -y -i "$$in" -pix_fmt {{PIXFMT}} -q:v {{Q}} \
	  -vf "transpose=1,fps={{FPS}},scale={{WIDTH}}:{{HEIGHT}}:flags=lanczos" \
	  "$$out"

#---------------------------------------
# Housekeeping
#---------------------------------------
clean:
	@shopt -s nullglob; \
	rm -f {{OUT_DIR}}/*.mjpeg || true; \
	echo "Cleaned {{OUT_DIR}}/*.mjpeg"

_ensure-out:
	@mkdir -p {{OUT_DIR}}

# Optional: reduce macOS metadata on SD (run on the *mounted* volume path)
# Example: just sd-sanitize /Volumes/SDCARD
sd-sanitize MOUNT:
	@echo "Sanitizing: {{MOUNT}}"
	@touch "{{MOUNT}}/.metadata_never_index" || true
	@find "{{MOUNT}}" -name ".DS_Store" -delete || true
	@find "{{MOUNT}}" -name "._*" -delete || true
	@echo "Done. (System folders like .Spotlight-V100 may be protected by macOS and can be ignored.)"