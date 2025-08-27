# Justfile for ESP32-C6-LCD-1.47 Video Player
# Converts all mp4 files in mp4/ → SD_CONTENT2/mjpeg/*.mjpeg

default:
    @just --list

# Ensure output dirs exist
ensure-dirs:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p SD_CONTENT2/mjpeg

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

# Convert all .mp4 → .mjpeg (172x320, fps=17, q=8 by default)
convert-all:
    #!/usr/bin/env bash
    set -euo pipefail

    mkdir -p SD_CONTENT2/mjpeg
    files=(mp4/*.mp4)
    if [[ ${#files[@]} -eq 0 ]]; then
        echo "❌ No MP4 files found in mp4/"
        exit 1
    fi

    fps="${FPS:-21}"
    q="${Q:-8}"
    width=172
    height=320

    echo "⚙️ Converting ${#files[@]} file(s) to SD_CONTENT2/mjpeg/ (fps=$fps, q=$q, size=${width}x${height})"

    for f in "${files[@]}"; do
        base="$(basename "$f" .mp4)"
        out="SD_CONTENT2/mjpeg/${base}.mjpeg"
        echo "▶️  $f → $out"
        ffmpeg -y -i "$f" \
            -pix_fmt yuvj420p \
            -q:v "$q" \
            -vf "fps=$fps,scale=${width}:${height}:flags=lanczos" \
            "$out"
        if [[ -f "$out" ]]; then
            echo "  ✓ $(du -h "$out" | cut -f1)  $out"
        else
            echo "  ✗ Failed for $f" >&2
            exit 1
        fi
    done

    echo "✅ Done. Converted ${#files[@]} movie(s) into SD_CONTENT2/mjpeg/"

all: ensure-dirs list-movies convert-all

sync-sd:
    rsync -av --progress --delete ./SD_CONTENT2/mjpeg/ /Volumes/SDCARD/mjpeg/