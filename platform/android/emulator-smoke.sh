#!/bin/sh
# Installs a game APK on the running emulator, runs it headless for 300 frames and checks the
# frame hash it logs against the desktop build's.
#   sh platform/android/emulator-smoke.sh <game.apk> <expected sha256>
set -eu
apk=$1
expected=$2
# The runner image may ship several build-tools versions; use the newest aapt2.
aapt2=$(ls -d "$ANDROID_HOME"/build-tools/*/aapt2 | sort -V | tail -n1)
pkg=$("$aapt2" dump packagename "$apk" | head -n1)
adb install -r "$apk"
adb logcat -c
adb shell am start -W -n "$pkg/io.anyport.anygba.GameActivity" \
  --esa args "--headless,--frames,300,--dump-frame-hash,--no-save,--no-miss-log"
hash=""
for _ in $(seq 1 120); do
  hash=$(adb logcat -d -s anygba:I | sed -n 's/.*frame 300 hash \([0-9a-f]*\).*/\1/p' | tail -n1)
  [ -n "$hash" ] && break
  sleep 1
done
adb logcat -d -s anygba:V SDL:V AndroidRuntime:E | tail -n 40
echo "android: $hash"
echo "desktop: $expected"
[ -n "$hash" ] && [ "$hash" = "$expected" ]
