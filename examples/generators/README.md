# generators demo

Code written by the compiler instead of found at run time: two **source
generators** (programs the compiler builds and runs on the types marked
with an attribute) and two **compiletime functions** (functions the
compiler runs to compute a constant). The program has no run-time type
information and does no reflection: everything it needs is ordinary
Pascal, generated or computed before the build, and checked by the
compiler like the rest.

```sh
cd examples/generators
../p3                    # builds the generators, then bin/shop
bin/shop                 # an order as JSON, a status by name
bin/shop cancelled
```

```
Orders today
============
{"Number": 1042, "Customer": {"Id": 7, "Name": "Ada \"the Countess\"", "vip": true}, "Status": "Paid", "Total": 99.5, "Note": "leave at the door"}
status: Paid
'cancelled' is Cancelled (3)
crc32: 8D9D8D36
```

## What it shows

- `[EnumNames] TStatus = (Pending, Paid, ...)` — `generators/enumgen.pas`
  writes a type helper for the enumeration: `S.ToString`,
  `TStatus.TryParse('paid', S)`, and `ToJsonValue`. The value names are
  written into the code (`W.Declaration` for the helper, `W.Line` for its
  methods), so there is no table of names to look up when the program runs.
- `[Json] TOrder = partial record ... end` — `generators/jsongen.pas` adds
  `ToJson` to the record (`W.Member`, in a `partial` half of the type) and
  writes its body: one piece per field, by its type. `[Skip]` leaves a field
  out, `[JsonName('vip')]` renames it, a field of another `[Json]` type or of
  an `[EnumNames]` enumeration is written through its `ToJsonValue`. A field
  the generator cannot write is an error at that field, while compiling:

  ```
  src/lib/orders.pas:27:5: error: a field of type array[1..3] of string cannot be written as JSON; mark it [Skip]
  ```

- `const CrcTable = BuildCrcTable;` (unit `orders`) — a compiletime
  function fills the CRC-32 table; the compiler stores the 256 values as a
  typed constant, and `Crc32` uses it. `Title = Underlined('Orders today')`
  (the program) does the same for a string.

`p3.json` lists the generators (`"generators"`); their output goes to
`gen/` (`"generatedDir"`): `orders.intf.inc` and `orders.impl.inc` are
what the compiler included in the unit, readable and debuggable, and
`gen/.ct/` has the programs that computed the constants. A generator is
built again only when its source changes and runs again only when the
types it reads change.

## Files

| File | What |
|---|---|
| `generators/enumgen.pas` | `generator EnumNamesGenerator for EnumNames;` |
| `generators/jsongen.pas` | `generator JsonGenerator for Json;` |
| `src/lib/orders.pas` | the types (`TStatus`, `TCustomer`, `TOrder`) and `Crc32` |
| `src/lib/jsonutil.pas` | `JsonString` and `JsonBool`, which the generated code calls |
| `src/shop.pas` | the program |

The generated code calls `IntToStr`, `FloatToStr` and `LowerCase`, so the
unit that holds the types uses `sysutils` (and `jsonutil`). Generators are
native programs, not sandboxed beyond a time and memory limit: only use
generators you trust.
