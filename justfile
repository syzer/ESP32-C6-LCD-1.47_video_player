# Justfile for ESP32-C6-LCD-1.47 Video Player
# - Converts all mp4 files in mp4/ → SD_CONTENT2/mjpeg/*.mjpeg
# - Converts all png files in png/ → SD_CONTENT2/jpeg/*.jpg
# - Syncs outputs to SD card

# ---------------------- Defaults ----------------------

# Default target: list available commands
default:
    @just --list

# SD card mount point (change if your volume name differs)
SD_MOUNT := "/Volumes/SDCARD"

# Output directories (local workspace)
MJPEG_OUT := "SD_CONTENT2/mjpeg"
JPEG_OUT  := "SD_CONTENT2/jpeg"

# Encode defaults
FPS := "17"
Q   := "8"        # MJPEG quality (lower = better quality, larger files)
JPG_QUALITY := "90"

# Target display size
WIDTH  := "172"
HEIGHT := "320"

# ---------------------- Movies (MP4 → MJPEG) ----------------------

# Ensure output dirs exist (both mjpeg and jpeg)
ensure-dirs:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p {{MJPEG_OUT}} {{JPEG_OUT}}

# List input movies
list-movies:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -d mp4 ]]; then
        echo "❌ No mp4/ directory found"
        exit 1
    fi
    echo "🎬 Found MP4 files in mp4/:"
    ls -1 mp4/*.mp4 2>/dev/null || echo "  (none)"

# Convert all .mp4 → .mjpeg (172x320, fps={{FPS}}, q={{Q}})
convert-all:
    #!/usr/bin/env bash
    set -euo pipefail

    mkdir -p {{MJPEG_OUT}}
    shopt -s nullglob
    files=(mp4/*.mp4)
    if [[ ${#files[@]} -eq 0 ]]; then
        echo "❌ No MP4 files found in mp4/"
        exit 1
    fi

    fps="{{FPS}}"
    q="{{Q}}"
    width="{{WIDTH}}"
    height="{{HEIGHT}}"

    echo "⚙️ Converting ${#files[@]} file(s) to {{MJPEG_OUT}} (fps=$fps, q=$q, size=${width}x${height})"

    for f in "${files[@]}"; do
        base="$(basename "$f" .mp4)"
        out="{{MJPEG_OUT}}/${base}.mjpeg"
        echo "▶️  $f → $out"
        ffmpeg -y -i "$f" \
            -pix_fmt yuvj420p \
            -q:v "$q" \
            -vf "fps=$fps,scale=${width}:${height}:flags=lanczos,setsar=1" \
            "$out"
        if [[ -f "$out" ]]; then
            echo "  ✓ $(du -h "$out" | cut -f1)  $out"
        else
            echo "  ✗ Failed for $f" >&2
            exit 1
        fi
    done

    echo "✅ Done. Converted ${#files[@]} movie(s) into {{MJPEG_OUT}}"

# One-shot full pipeline
all: ensure-dirs list-movies convert-all

# Sync converted MJPEGs to SD card /mjpeg
sync-sd:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -d {{SD_MOUNT}} ]]; then
        echo "❌ SD card not found at {{SD_MOUNT}}"
        exit 1
    fi
    mkdir -p "{{SD_MOUNT}}/mjpeg"
    rsync -av --progress --delete "{{MJPEG_OUT}}/"/ "{{SD_MOUNT}}/mjpeg/"

# ---------------------- Stills (PNG → JPEG) ----------------------

# List input stills
list-stills:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -d png ]]; then
        echo "❌ No png/ directory found"
        exit 1
    fi
    echo "🖼️  Found PNG files in png/:"
    ls -1 png/*.png 2>/dev/null || echo "  (none)"

# Convert all .png → .jpg (172x320, quality={{JPG_QUALITY}})
convert-stills:
    #!/usr/bin/env bash
    set -euo pipefail

    mkdir -p {{JPEG_OUT}}
    shopt -s nullglob
    files=(png/*.png)
    if [[ ${#files[@]} -eq 0 ]]; then
        echo "❌ No PNG files found in png/"
        exit 1
    fi

    width="{{WIDTH}}"
    height="{{HEIGHT}}"
    q="{{JPG_QUALITY}}"

    echo "🛠️  Converting ${#files[@]} PNG(s) to {{JPEG_OUT}} (size=${width}x${height}, quality=$q)"

    for f in "${files[@]}"; do
        base="$(basename "$f" .png)"
        out="{{JPEG_OUT}}/${base}.jpg"
        echo "🖼️  $f → $out"
        # Using ImageMagick (`magick`) for robust PNG→JPEG with resize + strip
        magick "$f" -resize ${width}x${height}\! -strip -quality "$q" "$out"
        if [[ -f "$out" ]]; then
            echo "  ✓ $(du -h "$out" | cut -f1)  $out"
        else
            echo "  ✗ Failed for $f" >&2
            exit 1
        fi
    done

    echo "✅ Done. Converted ${#files[@]} still image(s) into {{JPEG_OUT}}"

# Sync converted JPEGs to SD card /jpeg
sync-jpeg:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -d {{SD_MOUNT}} ]]; then
        echo "❌ SD card not found at {{SD_MOUNT}}"
        exit 1
    fi
    mkdir -p "{{SD_MOUNT}}/jpeg"
    rsync -av --progress --delete "{{JPEG_OUT}}/"/ "{{SD_MOUNT}}/jpeg/"

# Convenience target: convert stills then sync to SD
stills: ensure-dirs list-stills convert-stills sync-jpeg