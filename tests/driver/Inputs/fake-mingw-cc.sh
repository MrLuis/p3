#!/bin/sh
# A stand-in for x86_64-w64-mingw32-gcc (windows-crt.test): it answers p3's
# question about the C library from $FAKE_CRT (UCRT or MSVCRT), and records
# a link command in $FAKE_LOG, writing an empty output file.
case " $* " in
  *" -E "*) echo "P3_CRT_$FAKE_CRT"; exit 0 ;;
esac
echo "$@" >> "$FAKE_LOG"
while [ $# -gt 0 ]; do
  if [ "$1" = -o ]; then : > "$2"; fi
  shift
done
