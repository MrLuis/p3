# The Rust ports of p3's benches (optimization3.md item 7)

Faithful ports of `bench/ansistr.pas`, `bench/lexer.pas`,
`bench/intloops.pas`, `bench/dynarrays.pas` and `bench/reals.pas`, with
the values printed identical to the Pascal benches' (checked against
the `.expected` files). Each builds with:

    rustc -O -C overflow-checks=on NAME.rs -o NAME

(`-C overflow-checks=on` matches p3's checked `-O2`.) Where the port
chooses a shape the Pascal source cannot (`&text[start..i]` slices,
`Vec::push`'s headroom), the file says so in a comment.
