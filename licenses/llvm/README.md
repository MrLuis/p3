`LICENSE.TXT` is `llvm/LICENSE.TXT` of the LLVM project, tag `llvmorg-23.1.6`,
unchanged. Packages that have LLVM linked into p3 include it
(`utils/package-linux.sh`, `package-macos.sh`, `package-windows.sh`).

`THIRD-PARTY.txt` holds the notices of the third-party code in LLVM's Support
library, which every binary with LLVM linked in contains: the regular
expressions (Henry Spencer, the Regents of the University of California, Todd
C. Miller), xxHash (Yann Collet) and the Unicode conversions (Unicode, Inc.).
Their licenses ask for the notice with binary distributions. The texts are
taken unchanged from the same LLVM tag (`llvm/lib/Support/COPYRIGHT.regex`
and the headers of `regstrlcpy.c`, `xxhash.cpp` and `ConvertUTF.cpp`). The
packages install it as `share/doc/p3/LICENSE.llvm-third-party`.

Update both with LLVM: look again for files under other licenses in
`llvm/lib` and `lld` (for example, "Redistributions in binary form" and
"permission notice").
