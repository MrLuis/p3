# pcc — a Pascal compiler targeting LLVM

`pcc` compiles Pascal (ISO 7185, with optional Turbo/Free Pascal extensions)
to native executables through LLVM. See [PLAN.md](PLAN.md) for the roadmap.

**Status:** M11 complete (full ISO 7185 level 0). Supported:

- procedures and functions: value and `var` parameters, recursion, `forward`,
  nested routines with access to enclosing variables
- `const` and `type` declarations; `integer`, `real`, `boolean`, `char`,
  enumerations, subranges, arrays (multi-dimensional), records (with variant
  parts), strings (`packed array [1..n] of char`), `with`
- all statements except `goto`; full ISO expressions and type checking
- `read`/`readln`/`write`/`writeln` with field widths; the standard functions
  `abs sqr odd ord chr succ pred sin cos exp ln sqrt arctan trunc round`
- runtime checks for ranges, array bounds and integer overflow (on by
  default; `-fno-range-checks`, `-fno-overflow-checks`, or `{$R-}` / `{$Q-}`
  in the source)
- pointers, sets, files (text and typed; program parameters bind to
  command-line arguments), `goto`, procedural parameters, `pack`/`unpack`
- local variables and function results start from zero (numbers 0,
  pointers and objects `nil`, strings empty), which ISO leaves undefined.
  The elements of local arrays, the fields of local records and the
  characters of local strings are cleared too at `-O0`; from `-O1` they
  are not, unless they hold ansistrings, interfaces or files
  (`-fzero-init` / `-fno-zero-init` choose either way at any level)
- `-g` debug info (gdb/lldb), "did you mean" diagnostics, cross-compiling
  objects with `--target`

With `--mode=tp`, Turbo/Free Pascal extensions: units (`uses`, `-I`),
`string`/`string[N]` and string routines, `break continue exit(value) inc
dec`, `shl shr xor`, `write` of enumeration values (the name as declared,
padded on the right to a field width, as in Free Pascal; a value outside
its type is a runtime error), `$hex %bin #13` literals, `//` comments; the integer
types `longint int64 cardinal byte word shortint smallint` (FPC widening
rules) and `qword` (unsigned 64-bit, also named `UInt64`, `NativeUInt`,
`PtrUInt` and `SizeUInt`; see below); typecasts `T(x)`; distinct types `T = type X` (as an alias of `X`:
Free Pascal also tells them apart in overloads); `bitpacked record` and
`bitpacked array` (Free Pascal's layout: ordinal fields and elements take
the bits their values need, least significant first, so `bitpacked array
[0..7] of boolean` is one byte; other fields start at a byte; ordinal
fields and elements have no address, so they cannot be `var` arguments or
used with `@`; fields of types that need alignment, such as `real`,
pointers or plain records, and typed constants of bitpacked types are not
supported yet); `pointer`, `pchar` (also `AnsiChar` and
`PAnsiChar`), `@`, `getmem freemem
reallocmem fillchar move sizeof low high assigned`; `const` parameters and
`constref` ones (read-only like `const`, always passed by reference, as in
Free Pascal: a variable of the parameter's type itself, so its address is
the parameter's; any other argument through a temporary; also to `cdecl`
routines, which get the pointer; overloads cannot tell `const` and
`constref` apart);
procedural types; typed constants including arrays and records; external C
functions (`cdecl; external 'c' name 'strlen'`, see below); `{$I file}`; `paramcount
paramstr halt`. This is the dialect the self-hosted compiler is written in
(see [SELFHOST.md](SELFHOST.md) and [selfhost/DIALECT.md](selfhost/DIALECT.md)).

With `--mode=objfpc`, everything of `--mode=tp` plus classes, as in Free
Pascal's objfpc mode (`class inherited is as property` become reserved):

```pascal
type
  TShape = class                 { implicitly class(TObject) }
  private
    FName: string;
  public
    constructor Create(const AName: string);
    function Area: real; virtual; abstract;
    procedure Show; virtual;
    property Name: string read FName write FName;
  end;
  TSquare = class(TShape)
    Side: real;
    function Area: real; override;
  end;

function TSquare.Area: real;
begin
  Result := Side * Side;         { Result names the function result }
end;
...
  S := TSquare.Create('square'); { allocates, then runs the constructor }
  if S is TSquare then (S as TSquare).Side := 2;
  S.Show;                        { virtual dispatch }
  S.Free;                        { Destroy (virtual) unless nil, then release }
```

Supported: fields, methods, constructors and destructors (called through
the class they allocate, called on an object they do not), `virtual`,
`override` and `abstract` methods, `inherited` (with or without a method
name and arguments), `Self`, properties with field or method accessors,
`private protected public published` and `strict private`/`strict
protected` (private: visible in the declaring program or unit; protected:
also in descendants' methods; strict: only in the class's (and
descendants') methods), forward declarations (`TNode = class;`), `is`,
`as`, typecasts, `with` on objects, `Result` in functions, and the root
class `TObject` (`Create`, `Destroy`, `Free`, `ClassName`, `ClassType`,
`ClassParent`, `InheritsFrom`, `InstanceSize`). Classes work in units,
compiled whole or separately. Objects live on the heap: a variable of a
class type holds a reference (`nil` by default), and the object starts
with a pointer to its class's VMT. Calls of virtual methods and field
accesses through a `nil` reference are runtime errors under range checks,
as are a failed `as` and calling an abstract method.

- **Properties:** array properties (`property Items[i: integer]: T read
  Get write Put`, several indexes), the default property (`; default;`:
  `list[i]`), indexed properties (`index N`, passed to shared accessors),
  `default V` / `nodefault` (no effect, as without streaming).
- **Class references:** `TFooClass = class of TFoo`, `TClass`; a class
  name is a value; `c.Create(...)` creates an object of the class `c`
  holds, through a virtual constructor if the constructor is virtual.
- **Class methods and variables:** `class procedure/function` (Self is the
  class; may be virtual; called through a class name, a class reference
  or an object), `static` class methods (no Self, not virtual: they use
  class variables and other static methods, and `@TFoo.M` is an ordinary
  procedural value), `class var` sections (one variable shared by the
  class and its descendants).
- **Nested declarations:** `const` and `type` sections in a class or an
  advanced record, in any visibility section, as in Free Pascal: constants
  (typed ones in classes, which are class variables), enumerations,
  records (with methods, implemented as `TOuter.TInner.M`), classes
  (forward ones too) and other types. They are visible unqualified in the
  later members, the methods, the descendants and the classes and records
  declared inside, and elsewhere as `TOuter.Name` (`var x: TOuter.TInner`,
  `TOuter.Cap`, `TOuter.TNode.Create`, `x is TOuter.TLeaf`, case labels,
  `sizeof`, `for ... in`); private ones only in their unit, strict private
  ones only in the class. A nested class's name is `TOuter.TInner` (as
  `ClassName` returns). Not supported yet: nested interfaces, typed
  constants in records, typecasts through a qualified name
  (`TOuter.TKind(1)`) and qualified constants as subrange bounds. Unlike
  Free Pascal, a nested enumeration's values are also members
  (`TOuter.kA`).
- **Overloading:** routines and methods of one name with different
  parameters (the `overload` directive is optional; on a method it also
  offers the ancestors' overloads). The best fit wins: exact types, then
  wider integer types (`integer` before `int64` before `real`), the
  nearest ancestor class; a tie is an error. Overloads get the symbols
  `<name>$N`.
- **Interfaces:** `IFoo = interface[(IBase)] ['GUID'] methods end`,
  classes listing them (`class(TBase, IFoo, IBar)`, every method
  implemented), interface variables, parameters and results, `obj is
  IFoo`, `obj as IFoo`, comparisons and `Assigned`. A value is {object,
  the object's class's method table}, so virtual implementations dispatch
  as usual. Interfaces are COM style unless `{$interfaces corba}` (as in
  Free Pascal's objfpc mode):
  - **COM interfaces** (`{$interfaces com}`, the default) descend from
    `IUnknown` (= `IInterface`) and are reference counted. Their classes
    descend from `TInterfacedObject` (property `RefCount`). An object is
    freed when the last interface reference to it goes away: when a
    variable or field is assigned or a routine ends (its local variables
    and value parameters; also when an exception leaves it), when an
    object or record holding the reference is freed, or with `dispose`.
    `const` parameters take no reference. While a constructor runs the
    count is 1, so `Self` can be passed as an interface there. A COM
    interface also works as the left operand of `is` / `as` (`intf as
    TFoo`, `intf is IBar`). COM interfaces cannot be in variant parts or
    files, and a function cannot return a record holding them. Interface
    types must be declared in a `type` section.
  - **CORBA interfaces** (`{$interfaces corba}`) are not counted, and any
    class can implement them.

  Differences from Free Pascal: a function's interface result used
  without a variable (`Make.Name`, or a call whose result is discarded)
  is released when the statement ends, while Free Pascal keeps it in a
  hidden temporary until the routine ends; comparing interfaces compares
  their objects; global interface variables are not released at program
  end; `QueryInterface`, `_AddRef` and `_Release` are not declared (the
  compiled code counts references itself).

- **Advanced records** (`{$modeswitch advancedrecords}`, as in Free
  Pascal): records with `private` / `public` / `strict private` sections,
  methods (`Self` is the record, by reference), constructors with
  parameters (`TR.Create(...)` gives a new record), static class methods
  (`class function F: T; static;`), properties and overloaded methods.
  Methods can be called on variables, fields, array elements, function
  results and through `with`. Class operators: `class operator +(const a,
  b: TR): TR;` for `+ - * / div mod and or xor shl shr = <> < > <= >= in`,
  unary `- + not`, `:=` (implicit conversions, used in assignments,
  arguments, results and other operators' operands) and `Explicit`
  (casts `T(x)`). `<>` falls back to `not =`. A record with methods must
  be a named type declared at program or unit level.
  Not supported: class variables and default properties in records; Free
  Pascal's global `operator` declarations.

```pascal
{$modeswitch advancedrecords}
type
  TVec = record
    X, Y: integer;
    constructor Create(ax, ay: integer);
    function Len2: integer;
    class operator +(const a, b: TVec): TVec;
    class operator :=(k: integer): TVec;
  end;
...
  v := TVec.Create(1, 2) + 3;    { 3 becomes TVec via := }
  writeln(v.Len2);
```

- **Type helpers** (`{$modeswitch typehelpers}`, as in Free Pascal):
  `TAnsiStringHelper = type helper for AnsiString` adds methods,
  properties (read through a method), constants and static class methods
  to the values of a type: strings, ordinals, enumerations, reals,
  records and dynamic arrays. `Self` is the value, by reference, so
  `s.Append('x')` can change `s`; on a value (`'abc'.ToUpper`, a
  function's result, a constant) the method works on a temporary. Static
  methods and constants are used through the extended type
  (`AnsiString.Join(a, b)`), not the helper's name. The helper declared
  last for a type is the one used; `type helper(TParent) for T` extends
  an earlier helper. A record helper's methods see the record's fields.
  Helpers are declared at program or unit level. Not supported: helpers
  for classes and interfaces, constructors, typed constants and fields
  in helpers, writing a helper's property, and a statement that begins
  with a string literal (`'abc'.Clear;`).

```pascal
{$modeswitch typehelpers}
type
  TAnsiStringHelper = type helper for AnsiString
    function ToUpper: AnsiString;
    procedure Append(const Value: AnsiString);
    class function Join(const a, b: AnsiString): AnsiString; static;
  end;
...
  s.Append(' world');
  writeln(s.ToUpper, ' ', AnsiString.Join('a', 'b'), ' ', 'lit'.ToUpper);
```

- **QWord** (`--mode=tp` and objfpc, as in Free Pascal on 64-bit targets):
  the unsigned 64-bit integer, also named `UInt64`, `NativeUInt`, `PtrUInt`
  and `SizeUInt`, with values up to `High(QWord)` = 18446744073709551615.
  Decimal literals beyond `int64` are qword constants (`$FFFFFFFFFFFFFFFF`
  is the `int64` -1, as in FPC). `div`, `mod`, `shr`, comparisons and
  conversions to `real` are unsigned. With a type of 32 bits or less (or
  an `int64` constant of zero or more) an operand is converted to qword;
  with `int64` the result is `int64`; `-q` is `int64`. Range and overflow
  checks treat it as unsigned (converting a negative value to qword, or a
  qword above `High(int64)` to `int64`, is a range error); `write` and
  `read` handle the whole range. In overloads, unsigned arguments prefer
  `qword` and signed ones `int64`, as in FPC. Differences: a constant
  expression is typed as for variables (`High(int64) + 1` is not promoted
  to qword, and `High(QWord) > High(Int64)` compares as `int64`), and
  subranges cannot reach beyond `High(int64)`.

- **Record helpers** (`{$modeswitch advancedrecords}`, as in Free
  Pascal): `TGuidHelper = record helper for TGUID` is a helper for a
  record type, as above: methods, properties, constants and static class
  methods (`TGUID.Empty`). A record helper's methods see the record's
  fields, methods and properties; its members hide the record's own, also
  in `with r do`. `record helper(TParent) for T` extends an earlier record
  or type helper. A record helper for a type that is not a record is an
  error (use a type helper). `TGUID` and `PGUID` are predeclared (tp and
  objfpc), as in Free Pascal's system unit: a 16-byte packed record with
  the variants `Data1..Data4`, `D1..D4` and RFC 4122's `time_low` ...
  `node`. A `TGUID` typed constant or initialized variable takes a GUID
  string, as in Free Pascal: `const IID: TGUID =
  '{01234567-89AB-CDEF-0123-456789ABCDEF}';` (or `''`, all zeros), also
  inside arrays and records. `DWord` is `longword`.

```pascal
{$modeswitch advancedrecords}
type
  TGuidHelper = record helper for TGUID
    class function Empty: TGUID; static;
    function IsEmpty: boolean;
    function ToString(SkipBrackets: boolean = false): string;
  end;
...
  writeln(g.ToString, ' ', g.IsEmpty, ' ', TGUID.Empty.IsEmpty);
```

Not supported (reported as such): class properties, class constructors
and destructors, `stored`, `implements`, properties in interfaces.

Exceptions (`--mode=objfpc`), as in Free Pascal:

```pascal
uses sysutils;                   { built in: Exception is predeclared }
type EParse = class(Exception);
...
try
  try
    if c < '0' then raise EParse.Create('not a digit');
  finally
    writeln('always');           { also after exit, break and continue }
  end;
except
  on E: EParse do writeln(E.ClassName, ': ', E.Message);
  on E: Exception do raise;      { re-raise }
else
  writeln('anything else');
end;
```

Any object can be raised. Handlers are tried in order; with none matching
(and no `else`), the exception goes on to the next `try`. The exception
object is destroyed (`Destroy`, then released) when its handler ends,
including through `exit`, `break` or `continue`, and when a new exception
leaves the handler; a constructor that raises destroys its new object.
`Exception` has `Create(msg)` and the property `Message`. Integer `div` and
`mod` by zero raise `EDivByZero` (message `Division by zero`; its ancestors
`EIntError` and `EExternal` are predeclared too), as in Free Pascal with
SysUtils; other runtime errors (range and overflow checks, real division by
zero, ...) are not exceptions. Following Free Pascal, `exit`, and
`break`/`continue` out of a `finally` part, are errors, and so are labels
and `goto` in `--mode=objfpc` (Free Pascal without `-Sg`). An unhandled
exception is a runtime error that names the class, where it was raised and
the message (`Runtime error: unhandled exception EParse at p.pas:12:5: not
a digit`); `finally` parts run first. Exceptions are `_setjmp`/`_longjmp`
based: each `try` statement pushes a frame on a stack kept by the runtime.
So does a routine with ansistring or interface variables or temporaries,
which it releases when an exception leaves it, unless nothing it calls can
raise one (the runtime's string functions cannot, nor can a routine
compiled before it that was found not to; a call through a procedural
value, of a virtual method or of an external routine can).

Units are compiled into the program by default. They can also be compiled
separately: `pcc --mode=tp -c unit.pas` writes `unit.o` (its interface
variables and routines are the symbols `pas.<unit>$<name>`, its
initialization part `pas.<unit>$$init`), and a program built with
`-fseparate-units` uses those objects (from next to each unit's source)
instead of compiling the units again:

```sh
pcc --mode=tp -c lib/mathutils.pas && pcc --mode=tp -I lib -c shapes.pas
pcc --mode=tp -I lib -fseparate-units main.pas -o main
```

`-L dir` and `-l name` pass library options to the linker.

### Unit library

pcc ships units named and behaving like Free Pascal's, for Linux, macOS and
64-bit Windows: `sysutils`, `strutils`, `math` and `classes` so far. `uses` finds them
after the program's directory and the `-I` directories, so a unit of your own
with the same name wins. They live in `<pcc dir>/../lib/pascal` (override with
`PCC_UNIT_DIR`), built from `rtl/`; what differs between systems is in
`runtime/sys.c`. Their output is checked against Free Pascal 3.2.2's.

| Unit | Contents |
|---|---|
| `sysutils` | `Format`, `IntToStr`, `UIntToStr`, `IntToHex`, `StrToInt[64][Def]`, `TryStrToInt[64]`, `StrToQWord[Def]`, `TryStrToQWord`, `FloatToStr`, `[Try]StrToFloat[Def]`, `BoolToStr`, `Trim[Left/Right]`, `Upper/LowerCase`, `CompareStr/Text`, `SameText`, `QuotedStr`, `StringReplace`, `IsDelimiter`, `LastDelimiter`, `StrPas`, `Extract*`, `ChangeFileExt`, path delimiter routines, `FileExists`, `DirectoryExists`, `DeleteFile`, `RenameFile`, `CreateDir`, `RemoveDir`, `ForceDirectories`, `Get/SetCurrentDir`, `GetEnvironmentVariable`, `Encode/DecodeDate/Time`, `IsLeapYear`, `DayOfWeek`, `Now`, `Date`, `Time`, `GetTickCount64`, `Sleep`, `EConvertError` |
| `strutils` | `ReverseString`, `Left/Right/MidStr`, `DupeString`, `PadLeft/Right`, `AddChar[R]`, `Starts/Ends/ContainsStr/Text`, `AnsiStarts/Ends/ContainsStr`, `[Ansi]Index/MatchStr/Text`, `PosEx`, `ReplaceStr/Text`, `StuffString`, `Copy2Symb`, `Hex2Dec`, `Dec2Numb`, `IntToBin` |
| `classes` | `TList` (`Add`, `Insert`, `Delete`, `Remove`, `Extract`, `Clear`, `IndexOf`, `First`, `Last`, `Exchange`, `Move`, `Pack`, `Sort`, `Expand`, `Notify`, `Items`, `Count`, `Capacity`, `List`), `TNotifyEvent`, `TStrings` and `TStringList` (`OnChange`, `OnChanging`, `Add`, `AddObject`, `AddPair`, `AddStrings`, `Append`, `Assign`, `Insert`, `InsertObject`, `Delete`, `Clear`, `Exchange`, `Move`, `IndexOf`, `IndexOfName`, `IndexOfObject`, `Find`, `Sort`, `CustomSort`, `Sorted`, `Duplicates`, `CaseSensitive`, `OwnsObjects`, `Strings`, `Objects`, `Names`, `Values`, `ValueFromIndex`, `Text`, `CommaText`, `DelimitedText`, `Delimiter`, `QuoteChar`, `NameValueSeparator`, `StrictDelimiter`, `LoadFromFile`, `SaveToFile`, `BeginUpdate`, `EndUpdate`), `TPersistent`, `EListError`, `EStringListError`, `EFOpenError`, `EFCreateError` |
| `math` | `Sum`, `SumInt`, `Mean`, `MinValue`, `MaxValue`, `MinIntValue`, `MaxIntValue`, `Floor[64]`, `Ceil[64]`, `Power`, `IntPower`, `Log10/2/N`, `Hypot`, `FMod`, `Tan`, `Cotan`, `ArcSin/Cos/Tan2`, `Sinh/Cosh/Tanh`, `DegToRad`, `RadToDeg`, `IsNan`, `IsInfinite`, `Min`, `Max`, `Sign`, `InRange`, `EnsureRange` |

`classes` needs `--mode=objfpc`; the others work in `--mode=tp` too. Overloads exist only in objfpc
(the `double` forms of `Min`, `Max`, `Sign`, `InRange` and `EnsureRange`,
`IntToHex(longint)`, `IntToStr(qword)`, two-parameter `BoolToStr` and `PosEx`); conversion
failures raise `EConvertError` in objfpc and are runtime errors in tp.
Differences from Free Pascal: `TryStrToInt` rejects values outside `longint`
instead of wrapping them; `'.'` and `'e5'` are not valid floats; `math`
domain errors give NaN or infinity instead of exceptions; `Format`, like the
rest, uses 64-bit reals, so `%e` shows 15 digits. `Exception.CreateFmt` is
predeclared with `Exception`. `Sum`, `Mean` and the like take `double` arrays
only, where Free Pascal also has `single` and `extended` forms. With `-fseparate-units`,
programs link the library's objects, which are built for the host in objfpc
mode.

**Inline routines**, as in Free Pascal: `function Sq(x: integer):
integer; inline;` (also methods and record operators). The routine gets
LLVM's `inlinehint`, so it is inlined from `-O1` up when LLVM can (not
recursive calls); `-O0` keeps the calls, for debugging. `{$inline on}` is
the default in `--mode=objfpc`; in tp mode the directive needs it, and
`{$inline off}` makes it have no effect. `inline` cannot go with
`forward`, `external` or `virtual`/`override`/`abstract`. Separately
compiled units are not inlined into their users.

**Record layout** (`{$packrecords N}`, as in Free Pascal; also `{$align N}`,
`{$aN}`, `{$a+}`/`{$align on}` = 4, `{$a-}`/`{$align off}` = 1): the records
declared after it place each field at a multiple of the smaller of its
alignment and N (1, 2, 4, 8, 16 or 32), a variant part at a multiple of the
smaller of N and 16, and round their size to the largest alignment used;
`C`, `DEFAULT` and `NORMAL` are the natural (C) layout. `packed record` is
`{$packrecords 1}`. Accesses to fields that end up less aligned than their
types say so in the generated code; passing one as a `var` parameter or
through `@` gives a pointer that is not (it works on x86-64 and AArch64, as
in Free Pascal). Typed constants of such records cannot hold ansistrings,
pointers and the like yet. Classes keep their natural layout.

**Scoped enumerations** (`{$scopedenums on}`, also `+`; `off`/`-` ends
them), as in Free Pascal: the values of an enumeration declared while it is
on are not in scope, only reached through their type (`TColor.Red`), so
`TShade = (Light, Dark, Red)` and a constant `Red` can coexist. Any
enumeration's values can be qualified (`TPlain.pb`, also without the
switch), wherever a constant can be used: subrange bounds
(`TColor.Green..TColor.Blue`), case and variant labels, sets, array indexes,
typed constants and default parameters. A scoped enumeration declared in a
class does not add its values to the class (`TOuter.TKind.One`).

```pascal
{$scopedenums on}
type
  TColor = (Red, Green, Blue);
  TWarm = TColor.Green..TColor.Blue;
...
  c := TColor.Green;
```

**Hint directives** (not in `--mode=iso`), as in Free Pascal: `deprecated`
(optionally with a message: `deprecated 'use Bar'`), `experimental`,
`platform`, `library` and `unimplemented` after a constant, type, variable,
field, property, routine or method declaration (`procedure Foo; deprecated;`,
`TOld = record ... end deprecated;`, `x: integer platform;`). Each use of the
entity, here or in a unit that uses it, gets a warning (`'Foo' is deprecated:
use Bar`) and a note where it is declared. A directive only on a routine's
implementation has no effect, as in FPC. `-Wno-deprecated` silences these
warnings; `-Werror` makes them errors.

**Calling conventions** (not in `--mode=iso`): `cdecl` and `stdcall` on
routines (also `forward`, `external` and unit interface headings), methods,
interface methods and procedural types (`TCb = function(x: integer):
integer; stdcall;`). pcc calls every routine with the target's C
convention; a `cdecl` routine's procedural values can also be called from C
(see external routines). As in Free Pascal on 64-bit targets, `stdcall` is the default
convention and `cdecl` a different one: a routine's heading and
implementation, an override and the method it overrides (an override
without a directive inherits it), an interface method and its
implementation, and a procedural value and its type must agree on `cdecl`.
Not supported: `stdcall`'s different code on 32-bit x86 targets (`register`,
`pascal` and `safecall` are not recognized either).

**External routines** (C functions; not in `--mode=iso`), as in Free
Pascal: `function strlen(s: pchar): int64; cdecl; external 'c';` declares a
C function (at program or unit level, in a unit's interface too). After
`external`, an optional library and `name 'symbol'` (else the routine's
name, as spelled) are string constants, literal or named (`external libc
name 'abs'`). The library is linked with the program, also when it comes
from a unit: `-lz` for `external 'z'`, `-l:libz.so.1` for a file name.
Parameters and results have C types: integers, chars, reals, pointers,
`var` parameters, and callbacks (a `cdecl` procedural type of such
types). `varargs` (with `cdecl`) declares a variadic function such as
`printf`: further arguments may be ordinals (passed as at least `int`),
reals (`double`), pointers or string constants (C strings). A `cdecl`
routine at program or unit level can be passed to C (`qsort(..., @Compare)`)
and called through `cdecl` procedural variables; so can a `cdecl` external
routine (not a `varargs` one, nor one taking callbacks). Not supported:
records and strings by value, `index`, external variables (`cvar`).

**`rtlproc`** (not in `--mode=iso`), as in Free Pascal: the directive that
marks routines of Free Pascal's run-time library (`procedure
DoDirSeparators(var FileName: PathStr); rtlproc;`) is accepted on routines,
`forward` and interface headings and methods, and has no effect.

**External variables** (not in `--mode=iso`), as in Free Pascal: `errno:
cint; external 'c' name 'errno';` or `optind: cint; cvar; external;`
declares a C variable (the symbol is the name as written unless `name`
gives it; the library, a string constant, is linked as for routines). They
are declared at program or unit level, without an initial value. `errno`
is the C library's per-thread variable, reached through its accessor
(`__errno_location`, `__error` or `_errno`; Free Pascal cannot link it),
and the library `StdCLib` (classic Mac OS) means the C library. Not
supported: `cvar` without `external` (variables for C code to use).

**Untyped parameters** (not in `--mode=iso`), as in Turbo and Free Pascal:
`procedure Clear(var x; size: integer)`, `function Sum(const data; n:
integer)`. Any variable is passed by reference. In the routine, the
parameter is used through a typecast, which is a variable of that type at
its address (`longint(x) := 0`, `TPoint(x).y`, `TBytes(x)[i]`; read-only
for `const` ones), through its address (`@x`), or passed on to another
untyped parameter or to `move` and `fillchar`; anything else is an error
(also `sizeof(x)`, which Free Pascal makes 0). In overloads a typed
parameter fits better. An external routine's untyped parameter is a C
pointer (`function memcmp(const a; const b; n: int64): longint; cdecl;
external 'c';`).

**Out parameters** (`--mode=objfpc`), as in Free Pascal: `procedure Get(out s:
string; out n: integer)` passes variables by reference, like `var`, also
untyped (`out x`) and open arrays. The difference is that an ansistring or
interface in the argument, alone or in a record or array, is released before
the call: first of all, so an argument evaluated in the same call
(`Get(s, length(s))`) already sees it empty, and the routine starts with it
empty. Other values are left as they are. `out` and `var` must agree between a
heading and its implementation, an override and the method it overrides, an
interface method and its implementation, and a procedural value and its type;
overloads cannot differ only by them. `out` is not a reserved word, but in a
parameter list in objfpc mode it is always the modifier, as in Free Pascal.

**Initialized variables** (`--mode=objfpc`), as in Free Pascal:
`var flag: boolean = false;`, one name per declaration, with a constant
value of any type a typed constant accepts (ordinals, reals, chars,
strings, arrays, records). Program and unit variables start with their
values; a routine's are set again whenever it is called (unlike typed
constants, which keep their values between calls).

**Dynamic arrays** (not in `--mode=iso`), as in Free Pascal: `array of T` outside a
parameter list, indexed from 0, with `length` set at run time (`setlength(a,
n)`, or `setlength(m, 2, 3)` for arrays of arrays); elements start zeroed. A
variable holds a reference counted array: `b := a` shares it, and `setlength`
or `copy(a [, start [, count]])` give a variable its own. `length`, `high`,
`low`, `a = nil`, `a = b` (the same array), `assigned`, `sizeof` (a pointer)
and typecasts to and from pointers work. Elements can be of any type but
files, and are released with the array; arrays can be in records, classes and
other arrays, passed by value, `var`, `const` and `out`, to open array
parameters, and returned. `a := [1, 2, 3]` and an argument `[...]` build a new
array. Indexes are checked with range checks on. Two `array of T` of one
element type are the same type. Not supported: `insert`, `delete` and `concat`
on arrays, `TA.Create(...)`, typed constants other than `nil`, types that
contain themselves directly (`T = array of T`).

**Generic routines** (`--mode=objfpc`), as in Free Pascal 3.2.2:
`generic function IfThen<T>(val: boolean; const iftrue, iffalse: T): T;`
declares a template, and `specialize IfThen<integer>(true, 1, 2)` calls the
routine made for those type arguments (`generic procedure` too; several
type parameters, `<A, B>`). Generics are declared at program or unit level
(also in a unit's interface, defined in its implementation, or `forward`)
and coexist with non-generic routines of their name. A specialization is
made once per program or unit: its declaration is parsed again with the
type parameters naming the type arguments and checked like any routine,
so errors in it point into the generic, with a note at the use; it is
compiled as `F<integer>`. Not supported: generic types, methods and
constraints, implicit specialization (`F(1)`, not in FPC 3.2.2 either),
`@specialize F<T>`, and, for a unit compiled separately, generics that use
what the unit does not export (whole-program compilation allows it).

**For-in loops** (not in `--mode=iso`), as in Free Pascal: `for x in c do`
takes `x` through the elements of an array (static, open or dynamic) or a
string, the members of a set in order, the values of an ordinal type (`for
d in TDay`), or the values of an enumerator: `c.GetEnumerator` (a function
without arguments, of a class, an advanced record or an interface) returns
a class or advanced record with `function MoveNext: boolean` and `property
Current`. `x` is a variable, set by an ordinary assignment (conversions and
range checks apply) and not assignable in the loop. A dynamic array or
ansistring is enumerated as it was when the loop started; a static array or
short string is read in place. A class enumerator is freed when the loop
ends, also by `break`, `exit` or an exception. Not supported: interface
enumerators (interfaces have no properties yet), `array of const`, and
Free Pascal's `enumerator` directives and `operator Enumerator`.

**Default parameter values** (`--mode=objfpc`), as in Free Pascal:
`procedure P(a: integer; b: integer = 1; const s: string = 'x');` can be called
`P(0)` or `P(0, 2)`, and a function whose parameters all have defaults can be
used without arguments or parentheses. Value and `const` parameters only, one
name per default, and every parameter after one needs one too. The value is a
constant that fits the parameter: ordinals, reals, strings, sets, `nil`. It is
passed by the caller, as if written in the call, and comes from the heading the
call sees: a routine's or method's implementation may leave defaults out or
repeat them with the same values, and an override has its own. They also work
on `forward` and interface headings, in units, on methods, constructors,
interface methods and procedural types. Overloads count them when choosing,
and a tie is an error. Unlike Free Pascal, a value that does not fit the
parameter's range (`b: byte = 300`) is an error.

**Absolute variables** (not in `--mode=iso`), as in Turbo and Free Pascal:
`var bytes: array[0..3] of byte absolute n;` declares another view of `n`,
with a type of its own and no storage of its own. The target is a variable
(global, local, a parameter of any kind, including an untyped one, a typed
constant, `Self`, the function's name or `Result`), another absolute
variable, a field of a record variable (`absolute r.b`) or an element at a
constant index (`absolute a[2]`). One name per declaration, and no initial
value. Nothing is initialized or released through the view: an ansistring,
interface or file is managed only as the variable it overlays. The sizes
need not match; a view larger than its target reaches past it, as in Free
Pascal. `absolute` is not a reserved word. Not supported: memory addresses
(`absolute $B800`). An absolute variable does not appear in `-g` debug
information.

**Ansistrings** (not in `--mode=iso`), as in Free Pascal: `ansistring` is
a reference-counted string of any length, copied only when written to, and
`{$H+}` (or `{$longstrings on}`) makes plain `string` mean `ansistring` until
`{$H-}`, in that file. `shortstring` is always the 255-character kind, and
`string[N]` stays short. They mix freely with short strings, chars and string
constants in assignments, parameters, `+`, comparisons, `copy`, `pos`,
`concat`, `delete`, `insert`, `length` and `write`/`readln` (`readln` takes
the whole line); assigning one to a short string cuts it to fit. `setlength`
works on both kinds. A pchar assigns to an ansistring, and `pchar(s)` gives
the NUL-terminated characters (never nil); `ansistring(x)` converts
explicitly. `s[i]` is 1..`length(s)`, checked with range checks on. Records,
arrays, class fields, heap records and typed constants can hold them, and they
are released when they go out of scope, also when an exception passes.
Functions can return records that hold them. Not supported: in variant parts
or in `file of`; and an
`Exception.Message` longer than 255 characters (it is cut).

**Array of const** (not in `--mode=iso`), as in Free Pascal: a parameter
`const a: array of const` takes `[1, 'two', 3.0, obj, s]`, a list of values
of different types, or another array of const passed along. Inside, `a[i]` is
a predeclared `TVarRec` (`VType`, compared with `vtInteger`, `vtInt64`,
`vtBoolean`, `vtChar`, `vtExtended`, `vtString`, `vtAnsiString`, `vtPChar`,
`vtPointer`, `vtObject` and `vtClass`, and the fields `VInteger`, `VBoolean`,
`VChar`, `VExtended`, `VString`, `VAnsiString`, `VPChar`, `VPointer`, `VInt64`,
`VObject`, `VClass`), with `low(a)`, `high(a)` and `length(a)`; `string(p)` or
`ansistring(p)` reads a `VAnsiString`. Not on `cdecl` routines (they are C
variadics there; `varargs` is pcc's form).

**Open arrays** (not in `--mode=iso`), as in Free Pascal: a parameter `a:
array of T` (value, `const` or `var`) takes any array whose elements are
exactly `T`, whatever its bounds, an open array passed along, or `[x, y, z]`,
whose elements convert to `T`; string constants also fit `array of char`.
Inside, it is indexed from 0 to `high(a)` (checked with range checks on), with
`low(a)`, `high(a)`, `length(a)` and `sizeof(a)`. A value parameter gets its
own copy, on the stack. An open array can only be indexed and passed on,
not assigned. `slice(a, n)` passes the first `n` elements of an array or open
array to an open array parameter (and is allowed nowhere else; `n` is checked
with range checks on). Not supported: open arrays on `cdecl` routines.

**Method pointers** (`--mode=objfpc`), as in Free Pascal: `TEv = procedure(x:
integer) of object`, also as a function, hold a method and its object. `@obj.M`,
`@M` inside a method (of `Self`), `@TFoo.M` for a class method, or `nil`; a
virtual method is bound when the pointer is made. Records' methods work too, on
the record's address. Call one like a routine (`ev(1)`, `FOnEv(1)` from a method,
`obj.OnEv(1)`, and `ev()`, since objfpc accepts empty parentheses), test it with
`assigned` or `= nil`, compare with `=` and `<>` (the code only, as in Free
Pascal), pass it, return it (`GetEv(x)(1)` calls the result, also as a statement),
keep it in fields and properties. Calling a nil one
is a runtime error with range checks on. The predeclared record `TMethod`
(`Code`, `Data: pointer`) opens one up: `TMethod(ev).Data` reads or, on a
variable, writes the object, `TMethod(ev) := m` and `ev := TEv(m)` convert, and
`@TFoo.M` given to a pointer is the code of `TFoo`'s `M`. Typed constants and
initialized variables of these types, alone or in records and arrays, can only
be `nil`, as in Free Pascal.

**Conditional compilation** (every mode), as in Free Pascal:

```pascal
{$DEFINE TRACE}                  { also -d TRACE / -dTRACE on the command line }
{$IFDEF TRACE} writeln('enter'); {$ENDIF}
{$IF defined(PCC_OBJFPC) and not defined(SLOW)}
  ...
{$ELSEIF defined(PCC_TP)}
  ...
{$ELSE}
  ...
{$ENDIF}                         { or $IFEND }
```

`$DEFINE`, `$UNDEF`, `$IFDEF`, `$IFNDEF`, `$IF` / `$ELSEIF`, `$ELSE`,
`$ENDIF` and `$IFEND`; symbols are case-insensitive. `$IF` expressions
follow Pascal's precedence: integers (decimal, `$hex`), `'strings'`,
`true`, `false`, `defined(NAME)`, `declared(NAME)`, `sizeof(TYPE)` (not in
`--mode=iso`), macros,
`not`, `* div mod and`, `+ - or`, and one comparison (`= <> < > <= >=`) or
`in [a, b, lo..hi]`. `declared(NAME)` is true if NAME is declared at that
point, as in Free Pascal: earlier in the file (a routine's own names only
inside it), predeclared, or in a used unit (its name, and the declarations
of its interface; using `declared` loads the units of the uses clauses
read so far). `sizeof` takes the name of a predefined type only
(`SizeInt`, `PtrInt` and `NativeInt` are predeclared as `int64`, and
`NativeUInt`, `PtrUInt` and `SizeUInt` as `qword`): `$IF` is
evaluated while lexing, so unlike Free Pascal it cannot see declared types or
variables, and a program's own redeclaration of a predefined name is not
noticed. `PCC` is
predefined, and so is one of `PCC_ISO`, `PCC_TP` and `PCC_OBJFPC` (the
mode), along with Free Pascal's symbols for the target: `LINUX` and `UNIX`;
`DARWIN`, `BSD` and `UNIX`; `WINDOWS`, `MSWINDOWS` and `WIN64`; `CPU64` with
`CPUX86_64` and `CPUAMD64`, or `CPUAARCH64`. Each file (program or unit)
starts with only these and the `-d` symbols; an include file shares its includer's symbols, and a conditional
must end in the file that starts it. The text of a branch that is not
compiled is only scanned for comments, strings and conditional
directives. `$IFOPT` is not supported.

**Macros**, as in Free Pascal: after `{$macro on}`, `{$define NAME :=
value}` makes each later use of `NAME` (case-insensitive) stand for the
tokens of `value`, expanded when used (so a macro in another's value takes
its value at that time); `{$undef NAME}` and `{$macro off}` end that. A
macro is also defined for `$ifdef`. In `$IF`, a macro's value is an
integer or a string if its text is such a literal, the value of the macro
it names, or else its text as a string. Expanded tokens keep the locations
of the macro's value (in its `$define`). Without `{$macro on}`, `{$define
NAME := value}` warns and defines `NAME` without a value. Free Pascal
compares strings in `$IF` by length only; pcc compares them as strings.
Not supported: macro values on the command line (`-dNAME:=value`).

**Self-hosting:** pcc is also written in Pascal ([selfhost/](selfhost/),
[SELFHOST.md](SELFHOST.md)); `ninja -C build check-bootstrap` builds it with
itself, and `utils/seed.py` makes and uses a stage-1 *seed* (a binary plus
the compiler's LLVM IR) to bootstrap it without this C++ compiler.

**ISO 7185 conformance:** the Pascal-P5 test `iso7185pat.pas` passes all 793
checks (`utils/iso7185pat.py build/bin/pcc`).

## Benchmarks

```sh
utils/bench.py --pcc build/bin/pcc --fpc          # --base <older pcc> to compare two builds
```

compiles the programs of `bench/` (integer loops, nested routines, large
locals, short strings, ansistrings, sets, classes, exceptions, text output,
real arithmetic) at `-O0`, `-O2` and `-O2` without runtime checks, checks
their output against `bench/<name>.expected`, and prints run time,
executable size and compile time; `--fpc` adds Free Pascal's `-O2`. The
last row is the self-hosted compiler, built at `-O2`, compiling itself.
[optimization.md](optimization.md) lists the optimizations considered and
what has been done.

## Fuzzing

```sh
cmake -S . -B build-fuzz -G Ninja -DPCC_ENABLE_FUZZING=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined"
ninja -C build-fuzz pcc-fuzz
build-fuzz/bin/pcc-fuzz -max_total_time=300 corpus/ tests/run tests/*/Inputs
```

```pascal
program Sum(input, output);
var a, b: integer;
begin
  read(a, b);
  writeln(a, ' + ', b, ' = ', a + b:1);
  writeln('even? ', not odd(a + b))
end.
```

```sh
$ pcc sum.pas && echo "17 25" | ./sum
17 + 25 = 42
even? true
```

## Requirements

- LLVM 19 (dev headers + `FileCheck`), e.g. Debian/Ubuntu:
  `llvm-19-dev llvm-19-tools clang-19`; macOS: `brew install llvm@19`
- CMake ≥ 3.20, Ninja, a C++20 compiler
- `lit` (`pip install lit`)
- At run time, a C compiler driver (`clang` or `cc`) is used as the linker;
  override with `PCC_CC`. The runtime library is found at
  `<pcc dir>/../lib/libpascalrt.a` (override with `PCC_RUNTIME_LIB`), the
  unit library at `<pcc dir>/../lib/pascal` (override with `PCC_UNIT_DIR`).
- Optional: LLVM's own `clang` and `llvm-link` (in the LLVM tools
  directory). With them the build also writes the runtime as LLVM IR,
  `lib/libpascalrt.ll`, next to the library (override with
  `PCC_RUNTIME_IR`). From `-O1`, when compiling for the host, pcc links
  that file into the program's module before optimizing it, so that the
  runtime's small functions (reference counting, string assignment and
  comparison, exception frames, type tests) are inlined; the program still
  links the library, which the calls that are not inlined go to. Without
  the file, or with `-fno-inline-runtime`, every runtime function is
  called.

## Build & test

```sh
cmake -S . -B build -G Ninja            # add -DLLVM_DIR=<prefix>/lib/cmake/llvm if not found
ninja -C build                          # -> build/bin/pcc
ninja -C build check                    # run the lit test suite
```

## Usage

```sh
pcc hello.pas -o hello        # executable (default)
pcc -c hello.pas              # object file
pcc -S hello.pas              # assembly
pcc --emit-llvm hello.pas     # LLVM IR
pcc --emit-tokens hello.pas   # lexer dump
pcc --emit-ast hello.pas      # AST dump
pcc --help                    # all options (-O0..3, -g, --target, --mode=iso|tp|objfpc, ...)
```

### Project files (pcc.json)

A project can keep its options in a `pcc.json` file, so that `pcc` alone
builds it:

```json
{
  "name": "hello",
  "version": "1.0.0",
  "main": "src/hello.pas",
  "output": "bin/hello",
  "mode": "objfpc",
  "unitPaths": ["src/lib"],
  "defines": ["DEBUG"],
  "optimize": 2,
  "warnings": { "unusedVariable": false }
}
```

pcc looks for `pcc.json` in the input file's directory (the current
directory when there is no input file) and then in its parents;
`--project=<file>` names one and `--no-project` ignores it. Its settings are
defaults: options on the command line override them (lists, such as unit
paths and defines, are added after the command line's). Paths in it are
relative to its directory. Without an input file, pcc builds `main` (into
`output`, creating its directory, when building an executable).

| Setting | Value | Command line |
|---|---|---|
| `name`, `version`, `description` | strings (for people and tools) | |
| `main` | the program built when no input file is given | |
| `output` | the executable built from `main` | `-o` |
| `mode` | `"iso"`, `"tp"` or `"objfpc"` | `--mode` |
| `unitPaths`, `libraryPaths` | arrays of directories | `-I`, `-L` |
| `defines`, `libraries` | arrays of names | `-d`, `-l` |
| `optimize` | 0 to 3 | `-O` |
| `debug`, `separateUnits` | `true` / `false` | `-g`, `-fseparate-units` |
| `target` | a target triple | `--target` |
| `errorLimit` | an integer (0: no limit) | `-ferror-limit` |
| `warnings` | `{ "asErrors", "unusedVariable", "deprecated" }`: `true` / `false` | `-Werror`, `-Wno-...` |
| `checks` | `{ "range", "overflow" }`: `true` / `false` | `-fno-...-checks` |

The file is strict JSON (strings up to 255 characters). Mistakes are
reported like source errors (`pcc.json:3:15: error: 'optimize' must be an
integer from 0 to 3`); an unknown setting is a warning.

## Layout

| Path | Contents |
|---|---|
| `src/basic/` | source manager, diagnostics |
| `src/lexer/` | tokens (`TokenKinds.def`) and lexer |
| `src/ast/`, `src/parse/` | AST nodes (`ASTNodes.def`), printer, recursive-descent parser |
| `src/sema/` | semantic checks |
| `src/codegen/` | AST → LLVM IR, optimization, object/asm emission |
| `src/driver/` | command line, pipeline, linking |
| `runtime/` | `libpascalrt` C runtime (ABI in `runtime/include/pascalrt.h`) |
| `rtl/` | the unit library (`sysutils`, `strutils`, `math`, `classes`) |
| `tests/` | lit tests: `lexer/ parser/ sema/ codegen/ driver/`, `run/` (compile + execute + diff), C++ unit tests in `unit/` |
| `bench/` | benchmark programs with their expected output (`utils/bench.py`) |

## Writing tests

Tests are lit files with `RUN:` lines; `%pcc` is the compiler under test and
`FileCheck`/`not` come from LLVM. Example:

```
; RUN: not %pcc %S/Inputs/bad.pas 2>&1 | FileCheck %s
; CHECK: bad.pas:3:5: error: undeclared identifier 'x'
```

C++ unit tests use the tiny `TEST`/`EXPECT_*` macros in `tests/unit/UnitTest.h`.
