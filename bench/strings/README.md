# String routines benchmark

The routines of the unit library that scan bytes, on short and long
inputs, in ns per call: substring search (PosEx, StringReplace),
CompareStr, CompareText, UpperCase, LowerCase, TStringList.Text (line
splitting), nethttp's HttpParseRequest. `run.sh` builds `strbench.pas`
twice, with the units of the working tree and with those of a commit, and
runs both:

```sh
bench/strings/run.sh                 # against main
bench/strings/run.sh HEAD~4 1        # another commit, 1 second per routine
```

## Results

Apple M5, -O2, two runs each (best of each run); `main` before the 16-byte
scans (vector[16] of Byte, lane comparisons, VectorMask,
CountTrailingZeros), then with them. "short" strings are under 32 bytes,
where the routines keep their byte-by-byte code.

| Routine, input | before (ns) | with SIMD (ns) | |
|---|---:|---:|---:|
| PosEx, 19 bytes | 12.6 / 17.8 | 5.1 / 5.2 | 2.9x |
| PosEx, 61 bytes | 23.9 / 23.4 | 8.5 / 8.5 | 2.8x |
| PosEx, 120 KB | 62800 / 62800 | 8300 / 9150 | 7.2x |
| StringReplace, short | 79.8 / 80.4 | 71.6 / 70.2 | 1.1x |
| StringReplace, 120 KB | 128500 / 128450 | 21250 / 21250 | 6.0x |
| CompareStr, 64 bytes | 6.7 / 6.7 | 5.7 / 5.8 | 1.2x |
| CompareStr, 1 KB | 68.4 / 69.2 | 48.0 / 48.0 | 1.4x |
| CompareText, short | 9.9 / 9.9 | 10.1 / 11.2 | 0.93x |
| CompareText, 1 KB | 73.6 / 62.4 | 47.8 / 48.6 | 1.4x |
| UpperCase, short | 39.0 / 39.4 | 42.1 / 42.1 | 0.93x |
| UpperCase, 120 KB | 65050 / 64350 | 8600 / 8500 | 7.6x |
| UpperCase, 120 KB, nothing to change | 75900 / 75850 | 4400 / 4400 | 17x |
| LowerCase, 120 KB | 63950 / 63650 | 8550 / 8550 | 7.5x |
| TStringList.Text, 2000 lines | 170800 / 170200 | 78000 / 77200 | 2.2x |
| HttpParseRequest, 8 headers | 1022 / 1007 | 377 / 369 | 2.7x |

The short CompareText and UpperCase cost about 1-3 ns more: the test of
the length, and (UpperCase) the long path being in the routine, although
not taken; the byte-by-byte code they run is the one they had.
