# The Rust ports of p3's benches (optimization3.md item 7)

Faithful ports of `bench/ansistr.pas`, `bench/lexer.pas`,
`bench/intloops.pas`, `bench/dynarrays.pas` and `bench/reals.pas`, with
the values printed identical to the Pascal benches' (checked against
the `.expected` files), plus `vectors.rs`, the port of the SIMD-vector
bench `bench/vectors.pas` (llvm-23.md LB-6): the whole-vector chain is
written lane by lane (stable Rust arrays have no vector arithmetic;
LLVM sees the same `<4 x ...>`), the integer lanes are wrapping_* (p3's
vector lanes are unchecked), and the accumulator of the integer chain
goes through `black_box(p)` per pass (p3's chains live in global
slots, so its LLVM keeps the loop's code; the register-only Rust one
would be constant-folded away). Each builds with:

    rustc -O -C overflow-checks=on NAME.rs -o NAME

(`-C overflow-checks=on` matches p3's checked `-O2`.) Where the port
chooses a shape the Pascal source cannot (`&text[start..i]` slices,
`Vec::push`'s headroom), the file says so in a comment.
