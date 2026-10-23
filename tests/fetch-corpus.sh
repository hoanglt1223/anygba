#!/usr/bin/env sh
# Downloads the test corpus listed in corpus.lock and verifies every SHA-256.
# usage: tests/fetch-corpus.sh [--dest DIR]     (default: tests/corpus)
set -eu
set -f  # lock lines are split unquoted; never glob them

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
lock_file="$script_dir/corpus.lock"
dest="$script_dir/corpus"

while [ $# -gt 0 ]; do
    case "$1" in
        --dest)
            [ $# -ge 2 ] || { echo "--dest needs a directory" >&2; exit 2; }
            dest=$2
            shift 2
            ;;
        -h|--help)
            sed -n '2,3p' "$0"
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

for tool in curl unzip; do
    command -v "$tool" >/dev/null 2>&1 || { echo "missing required tool: $tool" >&2; exit 1; }
done

if command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    echo "missing required tool: sha256sum or shasum" >&2
    exit 1
fi

mkdir -p "$dest/.archives"
tmp_file="$dest/.download.tmp"
trap 'rm -f "$tmp_file"' EXIT

# Downloads $1 to $tmp_file and checks it against $2. Returns non-zero on any failure.
download_verified() {
    rm -f "$tmp_file"
    if ! curl -fsSL --retry 3 --retry-delay 2 -o "$tmp_file" "$1"; then
        echo "  download failed: $1" >&2
        return 1
    fi
    actual=$(sha256 "$tmp_file")
    if [ "$actual" != "$2" ]; then
        echo "  hash mismatch for $1" >&2
        echo "    expected $2" >&2
        echo "    actual   $actual" >&2
        rm -f "$tmp_file"
        return 1
    fi
}

failures=0
count=0
line_no=0
while IFS= read -r line || [ -n "$line" ]; do
    line_no=$((line_no + 1))
    line=$(printf '%s' "$line" | tr -d '\r')
    case "$line" in ''|'#'*) continue ;; esac

    # shellcheck disable=SC2086
    set -- $line
    if [ $# -ne 4 ]; then
        echo "corpus.lock:$line_no: expected 4 fields, got $#" >&2
        failures=$((failures + 1))
        continue
    fi
    kind=$1 hash=$2 path=$3 url=$4
    case "$path" in
        /*|*..*) echo "corpus.lock:$line_no: unsafe path: $path" >&2; failures=$((failures + 1)); continue ;;
    esac
    if ! printf '%s' "$hash" | grep -Eq '^[0-9a-f]{64}$'; then
        echo "corpus.lock:$line_no: invalid sha256: $hash" >&2
        failures=$((failures + 1))
        continue
    fi
    count=$((count + 1))

    case "$kind" in
        file)
            target="$dest/$path"
            if [ -f "$target" ] && [ "$(sha256 "$target")" = "$hash" ]; then
                echo "ok      $path"
                continue
            fi
            echo "fetch   $path"
            if download_verified "$url" "$hash"; then
                mkdir -p "$(dirname -- "$target")"
                mv -f "$tmp_file" "$target"
            else
                failures=$((failures + 1))
            fi
            ;;
        zip)
            target="$dest/$path"
            archive="$dest/.archives/$hash.zip"
            stamp="$target/.corpus-sha256"
            if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$hash" ]; then
                echo "ok      $path/"
                continue
            fi
            if [ ! -f "$archive" ] || [ "$(sha256 "$archive")" != "$hash" ]; then
                echo "fetch   $path/"
                if ! download_verified "$url" "$hash"; then
                    failures=$((failures + 1))
                    continue
                fi
                mv -f "$tmp_file" "$archive"
            fi
            rm -rf "$target"
            mkdir -p "$target"
            if unzip -q "$archive" -d "$target"; then
                printf '%s' "$hash" > "$stamp"
                echo "extract $path/"
            else
                echo "  extraction failed: $archive" >&2
                rm -rf "$target"
                failures=$((failures + 1))
            fi
            ;;
        *)
            echo "corpus.lock:$line_no: unknown kind: $kind" >&2
            failures=$((failures + 1))
            ;;
    esac
done < "$lock_file"

if [ "$failures" -ne 0 ]; then
    echo "corpus: $failures error(s) while checking $count entries" >&2
    exit 1
fi
echo "corpus: $count entries verified in $dest"
