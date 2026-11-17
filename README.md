# p3 — a Pascal compiler targeting LLVM

`p3` compiles Pascal (ISO 7185, with optional Turbo/Free Pascal extensions)
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
- pointers, sets, files (text, typed and, outside ISO mode, untyped `file`;
  program parameters bind to command-line arguments; Turbo/FPC `Assign`,
  `Close`, `BlockRead`/`BlockWrite`, `Seek`, `FilePos`, `FileSize` and
  `GetFileHandle`, see below), `goto`, procedural parameters, `pack`/`unpack`
- local variables and function results start from zero (numbers 0,
  pointers and objects `nil`, strings empty), which ISO leaves undefined.
  The elements of local arrays, the fields of local records and the
  characters of local strings are cleared too at `-O0`; from `-O1` they
  are not, unless they hold ansistrings, interfaces or files
  (`-fzero-init` / `-fno-zero-init` choose either way at any level)
- a set takes 4 bytes when its element type's highest value is at most 31
  (as in Free Pascal), 8 bytes when it is at most 63 (Free Pascal: 32) and
  32 bytes otherwise; `x in [...]` is compiled to comparisons or a mask
  test, without building a set
- only what the program reaches is compiled to machine code: routines,
  constants and variables that nothing reachable from the main block
  refers to (the unused routines of a used unit, the methods of a class
  that is never mentioned) are left out, also at `-O0`; a unit compiled
  with `-c` keeps its whole interface. `-fkeep-unused` keeps everything
- `-g` debug info (gdb/lldb), "did you mean" diagnostics, cross-compiling
  objects with `--target`

With `--mode=tp`, Turbo/Free Pascal extensions: units (`uses`, `-I`),
`string`/`string[N]` and string routines, `break continue exit(value) inc
dec`, `shl shr xor`, `write` of enumeration values (the name as declared,
padded on the right to a field width, as in Free Pascal; a value outside
its type is a runtime error), `$hex %bin #13` literals, `//` comments; the integer
types `longint int64 cardinal byte word shortint smallint` (FPC widening
rules) and `qword` (unsigned 64-bit, also named `UInt64`, `NativeUInt`,
`PtrUInt` and `SizeUInt`; see below); `widechar` (also `unicodechar`: a
16-bit character code; a lone code above 255, such as `#$D800`, is a
widechar constant, and a widechar becomes a char or a string as Free Pascal
converts it without a unicode string manager: codes up to 255 as that char,
others as `'?'`, also inside a string literal such as `'a'#$20AC'b'`; no
`widestring`/`unicodestring` yet); typecasts `T(x)`; distinct types `T = type X` (as an alias of `X`:
Free Pascal also tells them apart in overloads); `bitpacked record` and
`bitpacked array` (Free Pascal's layout: ordinal fields and elements take
the bits their values need, least significant first, so `bitpacked array
[0..7] of boolean` is one byte; other fields start at a byte; ordinal
fields and elements have no address, so they cannot be `var` arguments or
used with `@`; fields of types that need alignment, such as `real`,
pointers or plain records, and typed constants of bitpacked types are not
supported yet); `pointer`, `pchar` (also `AnsiChar` and
`PAnsiChar`), `@`, pointer arithmetic (see below), `getmem freemem
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
  procedural value; the implementation may repeat `static`, as in
  `class function TH.Parse(const s: string): T; inline; static;`),
  `class var` sections (one variable shared by the
  class and its descendants).
- **Class properties** (`class property Build: Integer read FBuild write
  SetBuild;`, also in advanced records), as in Free Pascal: the accessors
  are class variables or static class methods (array properties and
  `index` too), and the property is used without an object: through the
  type name (`TApp.Build := 5`), a class reference, an object, a
  descendant, Self, or unqualified in static, class and instance methods.
- **Class constructors and destructors** (`class constructor Create;`,
  `class destructor Destroy;`, also in advanced records), as in Free
  Pascal: at most one of each per type, static class procedures without
  parameters or directives that cannot be called (a constructor or method
  may have the same name). A program or unit runs the class constructors of
  the types it declares (nested ones too), in declaration order, just
  before its initialization part or body; at exit (also after `halt`) the
  class destructors run: the program's, then the units' in reverse order,
  each one's in declaration order. Free Pascal has already closed `Output`
  then; p3 has not, so they can still write.
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
  (`class function F: T; static;`), `class var` sections (static storage,
  not part of the record), a class constructor and destructor, properties
  and overloaded methods.
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

Not supported (reported as such): `stored`, `implements`, properties in
interfaces.

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
  on E: EConvertError do         { a new exception at the same address }
    raise EParse.Create(E.Message) at ExceptAddr;
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
SysUtils, and integer overflow (`{$Q+}`, and `checked` blocks below)
raises `EIntOverflow` (message `Arithmetic overflow`); other runtime errors
(range checks, real division by zero, ...) are not exceptions. Following Free Pascal, `exit`, and
`break`/`continue` out of a `finally` part, are errors, and so are labels
and `goto` in `--mode=objfpc` (Free Pascal without `-Sg`). An unhandled
exception is a runtime error that names the class, where it was raised and
the message (`Runtime error: unhandled exception EParse at p.pas:12:5: not
a digit`); `finally` parts run first. `raise E at Address[, Frame]` gives
the exception an address (pointers, as FPC's `CodePointer` and `Pointer`;
the frame is only evaluated), which an unhandled one is reported at
(`... EParse at $0000000000401A2C: ...`), as in Free Pascal; `ExceptAddr`,
in a handler, is the address of the exception it handles (nil outside
handlers; a re-raise keeps it). FPC's `get_frame`, `get_caller_addr` and
`get_caller_frame` are not supported, nor are casts between integers and
pointers, so the address is a variable's (`@x`), an object, a parameter
passed along, or `ExceptAddr`. Exceptions are `_setjmp`/`_longjmp`
based: each `try` statement pushes a frame on a stack kept by the runtime.
So does a routine with ansistring or interface variables or temporaries,
which it releases when an exception leaves it, unless nothing it calls can
raise one (the runtime's string functions cannot, nor can a routine
compiled before it that was found not to; a call through a procedural
value, of a virtual method or of an external routine can).

Units are compiled into the program by default. They can also be compiled
separately: `p3 --mode=tp -c unit.pas` writes `unit.o` (its interface
variables and routines are the symbols `pas.<unit>$<name>`, its
initialization part `pas.<unit>$$init`), and a program built with
`-fseparate-units` uses those objects (from next to each unit's source)
instead of compiling the units again:

```sh
p3 --mode=tp -c lib/mathutils.pas && p3 --mode=tp -I lib -c shapes.pas
p3 --mode=tp -I lib -fseparate-units main.pas -o main
```

`-L dir` and `-l name` pass library options to the linker.

### Unit library

p3 ships units named and behaving like Free Pascal's, for Linux, macOS and
64-bit Windows: `sysutils`, `strutils`, `math` and `classes` so far. `uses` finds them
after the program's directory and the `-I` directories, so a unit of your own
with the same name wins. They live in `<p3 dir>/../lib/pascal` (override with
`P3_UNIT_DIR`), built from `rtl/`; what differs between systems is in
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

**`{$modeswitch NAME}`** (also `NAME+`, `NAME-`, `NAME on`, `NAME off`), with
Free Pascal 3.2.2's names: `advancedrecords` and `typehelpers` (objfpc),
and `result`, `out` and `defaultparameters`, which work in tp mode too
(and can be turned off in objfpc mode). p3's mode decides `class`,
`objpas`, `properties` and `exceptions`, and always has `cvar`,
`initfinal`, `hintdirective` and `allowinline`: asking for the other state
is a warning. Switches for features p3 lacks (`nestedcomments`,
`objectivec1`, ...) can be turned off but not on (a warning); unknown names
are warnings too. Unlike Free Pascal, p3 also takes them after the start
of a file.

**Variables declared among the statements** (`--mode=objfpc`; as in
Delphi, not Free Pascal): `var x: T;`, `var x, y: T;`, `var x: T := e;`
and `var x := e;`, which gives `x` the type of `e` (see "Inferred types"
below for constants and `[a, b]`). The variable is visible from the next statement to the
end of the statement sequence the declaration is in (`begin ... end`, a
`repeat`, a part of a `try`, the `else` of a `case`), where it may hide an
outer variable of the same name; its value cannot use it. Each time the
declaration runs the variable is set, to its value or to zero. A loop can
declare its variable, visible in the loop only: `for var i := 1 to n do`,
`for var i: byte := ...`, `for var x in list do` (the type of the
elements) and `for var x: T in list do`.

```pascal
for var i := 1 to count do
begin
  var line := ReadLine(input);
  if line = '' then
    continue;
  var words := Split(line);
  total := total + length(words);
end;
```

**Owned variables** (`--mode=objfpc`; p3's own, not Free Pascal's): a
local variable declared `x: TFoo owned` owns its object. The object is
freed when the variable's scope is left, however it is left: for a
variable of a routine's `var` section when the routine ends (its end,
`exit`, an exception), for one declared among the statements when its
statement sequence is left (also by `break` and `continue`); the last
declared variable first, and when the variable is given another object
(`x := nil` frees it now). `move x` gives the object
up, to a function result, an argument or another variable: `x` is nil
afterwards, and using it before it is assigned again is a compile-time
error, also when only some path moved it (one branch of an `if`, an
earlier iteration of a loop, the protected part of a `try`).

```pascal
function Load(const Path: string): TDocument;
begin
  var f: TFileReader owned := TFileReader.Create(Path);
  var doc: TDocument owned := TDocument.Create;
  doc.ReadFrom(f);        { an exception here frees doc, then f }
  Result := move doc;     { the caller owns the document; f is freed }
end;
```

A plain class variable given `x`'s value borrows the object; so does a
value parameter. The object cannot be stored anywhere else (a field, a
global, another owned variable) or returned without `move`, `x` cannot be
passed as a `var` parameter or have its address taken, and `x.Free` is an
error (the object would be freed twice). Only the variable is followed: a
borrowed reference can outlive the object, and a routine nested in `x`'s
can use `x` and assign to it (but not move it) unchecked. If a destructor
raises while the variables are freed, the others are freed all the same
and its exception is raised afterwards, unless an exception is already
leaving the routine, which goes on. `owned` and `move` are not reserved
words. Not supported yet: owned fields, parameters and global variables.

**Template strings** (tp and objfpc; p3's own, not Free Pascal's): a
string between backticks can span lines and take values. `${expr}` puts
the value of an expression in the text, written as `write` writes it, so a
value can be anything `write` takes (integers, reals, characters, booleans,
enumeration values by name, strings, a pchar) and `${expr:width}` and
`${expr:width:digits}` are `write`'s field width and number of decimals.
The values are type-checked like any expression, and the result is an
ansistring.

```pascal
writeln(`Hello ${Name}, you have ${Count} items totalling ${Total:0:2}.`);

var Banner := `
  Report for ${Customer.Name}
    Rows: ${Rows:6}
  `;                              { 'Report for Ann'#10'  Rows:     12' }
```

- Line breaks in the text are kept, each as one line feed (`#10`) whatever
  line ends the source file has.
- The layout is not part of the text: a line break right after the opening
  backtick is dropped, and so is a last line holding only blanks before the
  closing backtick, with its line break. The lines then lose the
  indentation they all share. The closing backtick's line counts for that,
  so moving it to the left keeps some indentation; lines of only blanks do
  not count, and become empty. Text on the opening backtick's line is left
  alone. Blanks and tabs count alike.
- A backtick in the text is written twice, as Pascal doubles a quote, and
  the characters `${` are written `$${`. A `$` before anything else is just
  a `$`.
- An `r` right before the opening backtick makes the string raw: `${` is
  text, so a shell script, JavaScript or a regular expression can be pasted
  as it is (``r`^\d{3}-${1}$` ``). The layout rules still apply.
- A value can be any expression, also one with strings, comments and
  templates of its own: `` `total: ${ `${a} + ${b}` }` ``.
- A template without values, and a raw one, is a string constant like a
  quoted one: it can be a `const` or a typed constant's value, and one
  character long it is a `char`.

**Tagged templates**: a routine's name right before the opening backtick
is called with the text parts and the values, two open arrays, instead of
their being joined. ``Sql`... ${Region} ...` `` is the call
`Sql(['... ', ' ...'], [Region])`: there is always one more text part than
values, and the tag returns whatever it likes. A library can turn the
values into parameters instead of pasting them into the text (no SQL
injection), escape them for HTML, or look a message up by its text.

```pascal
function Sql(const Parts: array of string;
             const Values: array of const): TQuery;

var q := db.Sql`
  SELECT id, name
  FROM customers
  WHERE region = ${Region}
  ORDER BY name
  `;
{ Parts = ['SELECT id, name'#10'FROM customers'#10'WHERE region = ',
           #10'ORDER BY name'],  Values = [Region] }
```

The tag is a function, a procedure (``Log`loaded ${Path}`;`` is a
statement) or a method (`db.Sql`), and overloads are chosen as for any
call. Its values take no field width. Not supported: formats other than
`write`'s (a picture such as `0.00` or `yyyy-mm-dd`), and values of types
`write` cannot write (records, sets, objects): convert them in the
expression. A name `r` used as a tag needs a blank before the backtick.

**Inferred types** (objfpc; p3's own): types the compiler works out from
what is written next to them. Inference is local: from the value on the
right, or from the type the place expects, in reading order.

```pascal
generic function Largest<T>(a, b: T): T;
generic function Total<T>(const Items: array of T; Start: T): T;
generic function Make<T>: T;

n := Largest(3, 7);              { T = Integer, from the arguments }
x := Largest(1, 2.5);            { Integer and Real: T = Real }
t := Total([1, 2, 3], 10);       { T from the elements and from Start }
r := Make();                     { T = the type of r }

const limit := Largest(n, 10) * 2;   { set once; the type of its value }
var names := ['ann', 'bo'];          { array of string }
```

- **Generic calls.** `F(args)` for a generic routine `F` takes each type
  argument from the arguments: a parameter of type `T` gives `T` the type
  of its argument, a parameter `array of T` the type of the elements.
  Several arguments for one `T` must have a common type (see below). A
  type parameter no argument gives is the type wanted of the result, when
  the result is a `T` and the call is the value of an assignment, a typed
  declaration or `exit`. If that is not enough, the error names the type
  parameter and says what to write: `specialize F<...>(...)`, which is
  still accepted everywhere. Named arguments and default values work as
  for other routines.
- **Through lambdas.** A lambda passed for a parameter of a generic
  procedural type (`F: specialize TFunc<T, R>`, with `generic TFunc<T, R>
  = reference to function(const X: T): R`) works both ways: its untyped
  parameters take the types already known (from the other arguments, or
  the class of a generic method's object), and its body gives the type of
  its result: `ints.Map(function(x) => IntToStr(x))` is a `Map<string>`,
  `Apply(function(s) => Length(s), 'hi')` an `Apply<string, integer>`.
  Types written in the lambda give type arguments too. A routine that is not generic and has the same name
  is called instead.
- **`const x := e`** (also `const x = e` and `const x: T := e`) among the
  statements declares a variable that is set there and cannot be changed
  afterwards: assigning to it, passing it as a `var` parameter, `inc`. Its
  value need not be a compile-time constant, and it follows the scope
  rules of `var x := e`.
- **Constants.** Where a type is inferred, a whole number is an `Integer`
  (an `Int64` if it does not fit), a number with a fraction a `Real`
  (p3's `Double`), and quoted text a `string`, also one character:
  `var c := 'a'` is a string; write `var c: Char := 'a'` for a character.
  The value of an expression keeps its type (`var c := s[1]` is a `Char`).
- **`[a, b, c]`** in `var x := [...]` is a dynamic array (`array of T`) of
  the common type of its elements. It stays a set, as ever, when its
  elements are enumeration values, characters or booleans, or when it has
  a range: `[Red, Blue]`, `['a', 'e']`, `[1..9]`. `[]` needs a type.
- **A common type** of two types is that type if they are the same; the
  wider integer type; `Real` for a real and an integer (nothing is lost);
  `string` for strings and characters; else the one the other can be
  assigned to (a class and its descendant: the ancestor). Otherwise it is
  an error: an `Integer` and a `string` have none.
- A variable's type is fixed where it is declared: after `var x := 3`,
  `x := 3.5` is an error. `var x := nil` is an error too.

Not inferred: the type arguments of a generic type from the declared type
of a variable (`specialize TList<T>` is always written). The types of the
parameters and results of named routines, and of fields, are always
written; a lambda's are inferred (see "Lambdas").

**Lambdas** (objfpc; p3's own syntax): `function(params) => expression`
and `procedure(params) => statement` are values of a procedural type,
written where they are used.

```pascal
type
  TFn = function(x: Integer): Integer;
function Apply(f: TFn; v: Integer): Integer;

procedure Run(k: Integer);
var
  total: Integer;
begin
  Writeln(Apply(function(x) => x + k, 1));        { x: Integer, from TFn }
  Each(names, procedure(s) => total := total + Length(s));
  var add := function(a, b: Integer) => a + b;    { returns Integer }
  Writeln(add(2, 3));
  Each(names, procedure(s) =>
    begin
      Writeln(s);
      count := count + 1;
    end);
end;
```

- **Parameters** are written as in a heading, with `,` or `;` between them
  and `var` / `const` as needed. A parameter without a type takes the type
  (and kind) of the parameter of the procedural type the lambda is passed
  or assigned to. Without a type to go by (`var f := function(a, b) => a +
  b`) that is an error: "cannot infer the type of parameter 'a' in this
  lambda: there is no expected type". The lambda must have as many
  parameters as that type.
- **The result** of a `function` lambda is its expression. Its type is
  written after the parameters (`function(x): Real => x`), or is the
  procedural type's, or is the type of the expression (a quoted constant
  is a `string`). A `procedure` lambda's body is one statement, which can
  be a `begin ... end` block.
- **What it can use**: everything in scope where it is written, including
  the parameters and variables of the routine it is in and, in a method,
  the object's members. It runs with that routine's variables, while that
  routine runs (as nested routines do; nothing is copied). So a lambda
  that uses them can be passed as an argument, called, or kept in a local
  variable of that routine, but not stored where it would outlive the
  routine (a global, a field, a `var` parameter, a function result): that
  is an error naming what it uses. A lambda that uses nothing of the
  routines around it (only its parameters, globals and top-level routines)
  can be stored anywhere.
- It is checked where it is used, against the type wanted there:
  assignments, typed declarations, arguments (also of generic types'
  methods, where `T` is the type argument), and conditional expressions.

A lambda cannot use a field through a `with` statement outside it (name
the record instead).

**Lambdas and routines in typed constants**: a typed constant (or an
initialized variable) of a procedural type, plain, `reference to` or `of
object`, can hold `nil`, `@F` of a routine declared at program or unit
level or of a static method, or a lambda, also as elements of arrays and
fields of records: dispatch tables.

```pascal
type
  TOp = record
    Name: string;
    Fn: TFn;                    { function(x: Integer): Integer }
  end;
const
  Twice: TFn = function(x) => 2 * x;
  Ops: array[0..2] of TOp = (
    (Name: 'inc'; Fn: function(x) => x + 1),
    (Name: 'dec'; Fn: function(x) => x - 1),
    (Name: 'id'; Fn: @Id));
```

The value must be a constant: a lambda there may not use the variables of
the routine around it ("typed constant value must be a constant: this
lambda uses 'k' of the routine around it"), nor have an object (an `of
object` lambda in a method), and `@obj.M` is not a constant. A routine's
name without `@` is not a value there (as in assignments).

**Overloads and lambdas**: a lambda passed to an overloaded routine
chooses the overload by its shape, then takes its types from it. It fits
a procedural parameter of the same kind (`function` or `procedure`) with
as many parameters, of the types and kinds (`var`, `const`) it writes,
and of the result type it writes; its body is not used to choose.

```pascal
function Apply(f: TUnary; v: Integer): Integer; overload;   { 1 parameter }
function Apply(f: TBinary; v: Integer): Integer; overload;  { 2 parameters }
function Calc(f: TIntFn): Integer; overload;     { function(x: Integer): Integer }
function Calc(f: TRealFn): Integer; overload;    { function(x: Real): Real }

Apply(function(x) => x * 2, 5);            { TUnary }
Apply(function(a, b) => a * b, 5);         { TBinary }
Calc(function(x: Real) => x / 2);          { TRealFn: the type written }
Calc(function(x) => x);                    { error: ambiguous, write the type }
```

A parameter without a type fits any type, so overloads that differ only
there are ambiguous: write the type. A parameter with a type but no kind
takes the kind wanted, but fits a value parameter best (`procedure(a, b:
Integer)` chooses `procedure(a, b: Integer)` over `procedure(var a, b:
Integer)`). The errors name the lambda's shape: "ambiguous call to
overloaded 'Calc' with arguments (lambda function(?): ?)".

**Method-pointer lambdas**: a lambda wanted as an `of object` type is
called on an object, like `@Self.M`. In a method of a class its object is
that method's Self, and it sees the object's members as the method does;
outside methods its object is nil. It can use its object's members, its
parameters and what is declared at program or unit level, but nothing of
the routines around it, and so can be stored anywhere: event handlers.

```pascal
constructor TForm.Create;
begin
  Ok := TButton.Create;
  Ok.OnClick := procedure(Sender) =>      { TNotifyEvent: of object }
    begin
      Clicks := Clicks + 1;               { the form's field }
      Writeln((Sender as TButton).Caption, ' clicked');
    end;
end;
```

Using a variable or parameter of the routine around it is an error ("this
method-pointer lambda uses 'k' of the routine around it; ..."), as is one
in a record's method or a class method (their Self is not an object).

A lambda that must outlive its routine and use its variables is a closure:

**Closures** (objfpc, Free Pascal's and Delphi's syntax): a `reference to`
procedural type holds a lambda that may outlive the routine it is made in.

```pascal
type
  TIntFn = reference to function(x: Integer): Integer;
  TProc = reference to procedure;
  TCounter = reference to function: Integer;

function MakeAdder(n: Integer): TIntFn;
begin
  Result := function(x) => x + n;       { n lives on after MakeAdder returns }
end;

procedure MakeCounter(out step: TProc; out get: TCounter);
var
  count: Integer;
begin
  count := 0;
  step := procedure => count := count + 1;   { both share count }
  get := function: Integer => count;
end;

add5 := MakeAdder(5);
Writeln(add5(10));                      { 15 }
```

- **Values**: a lambda (a closure where a `reference to` type is wanted),
  `@F` of a routine declared at program or unit level or of a static
  method, `nil`, or another value of the type. A value of a plain
  procedural type and one of a `reference to` type do not convert into each
  other (the plain one may be a nested routine's, on the stack).
- **Sharing**: the variables of the routines around it that a closure uses
  are shared, not copied: the routine and its closures see the same
  variables, and changes after the closure is made are seen by it. Such a
  routine's frame is on the heap, reference counted, and freed (releasing
  its strings and other counted values) when the routine has returned and
  no closure holds it. Routines whose closures use nothing of theirs keep
  their frames on the stack.
- **Counted**: values of a `reference to` type are counted like
  ansistrings: in variables, fields of records and classes, arrays,
  dynamic arrays, parameters and function results. A closure kept in a
  variable of the frame it uses (`fact := function(k) => ... fact(k -
  1)`) holds that frame from inside: set the variable to `nil` to free it.
- **What a closure cannot use** of the routines around it, as they are not
  in the frame or not owned by it: `var`, `out` and `constref` parameters,
  `const` parameters of structured or counted types, open-array and untyped
  parameters, a function's result, the Self of a record method (a class's
  Self is fine), and routines nested in them. Each is an error ("this
  closure uses 'v', a 'var' parameter of 'ByVar', which may not exist when
  the closure runs; copy it to a local variable first"); a copy (below)
  is the way to use their values.

**Checked and unchecked arithmetic** (objfpc; item 9 of the proposals):
`checked ... end` checks the integer arithmetic of its statements for
overflow (`+ - *`, unary minus, `abs`, `div`, `inc`/`dec`), raising
`EIntOverflow`, whatever `{$Q}` says around it; `unchecked ... end` does
not check it: it wraps (two's complement), and so does an integer value
converted to a narrower integer type there (`b := b + 10` for a byte
`b`). A subrange, an enumeration or an array index is still range-checked
(`{$R}`) inside `unchecked`. Conversely, inside `checked` an integer value
converted to a narrower integer type (an assignment, an argument) is
checked whatever `{$R}` says, raising `EIntOverflow` when it does not fit;
a typecast (`Byte(i)`) still truncates.

```pascal
function TotalSize(const Sizes: array of Cardinal): QWord;
begin
  checked
    Result := 0;
    for var s in Sizes do
      Result := Result + s;   { raises EIntOverflow if it wraps }
  end;
end;

unchecked
  hash := (hash xor Ord(c)) * 16777619;   { wrapping is intended here }
end;
```

The blocks nest, and a `{$Q±}` inside one switches again up to its `end`,
where the state before the block comes back. The default for a unit stays
`{$Q±}` and `-fno-overflow-checks` (on by default). `checked` and
`unchecked` are not reserved: a block starts where one of them begins a
statement and is followed by a statement (not by `:=`, `(` or `;`).

`checked(e)` and `unchecked(e)` do the same for one expression, a value
(in parentheses): `Result := checked(a * b);`, `h := unchecked(h *
16777619);`. Where a declaration named `checked` or `unchecked` is visible
(a variable, a function), the name is that declaration's instead.

`AddChecked(a, b)`, `SubChecked(a, b)` and `MulChecked(a, b)` give the
tuple `(a op b, ok)`, never raising: the value wrapped and `ok = False` on
overflow, in the type `a op b` would have (`Int64`, `Cardinal`, ...).

```pascal
var (sum, ok) := AddChecked(total, n);
if not ok then
  Writeln('too large');
```

**Async/await and structured concurrency** (objfpc; item 10 of the
proposals): a routine whose result is `async T` (or `async`, for none) is
async: calling it makes a *task*, a counted reference to a computation on a
stack of its own, which starts at once and runs, here, until it first
waits. `await t` (in an async routine) and `Wait(t)` (anywhere) wait for a
task and give its value; an exception raised in the task is raised again
where it is awaited.

```pascal
uses SysUtils, threading;

function Fetch(id: Integer): async string;
begin
  Sleep(10);                       { waits without blocking the thread }
  Result := 'item ' + IntToStr(id);
end;

function Both: async string;
var
  a, b: async string;
begin
  a := Fetch(1);                   { both run at once }
  b := Fetch(2);
  Result := (await a) + ', ' + (await b);
end;

begin
  Writeln(Wait(Both));
end.
```

`scope [token] ... end` is structured concurrency: `spawn F(x)` (or
`spawn begin ... end`, which may use the routine's variables) starts a
child task inside it, and the scope's `end` waits for all of them, however
the block is left. When a child fails, the scope cancels the others and
raises the first failure. A cancelled task's next wait (an `await`, a
`Sleep`, a channel) raises `EOperationCancelled`.

```pascal
function Pipeline(Token: TCancelToken): async Integer;
var
  jobs: specialize TChannel<Integer>;
begin
  jobs := specialize TChannel<Integer>.Create(4);   { bounded: back-pressure }
  scope Token.WithTimeout(5000)
    spawn begin
      for var j := 1 to 100 do
        await jobs.Send(j);
      jobs.Close;
    end;
    spawn begin
      for await var j in jobs do   { until closed }
        Process(j);
    end;
  end;
  jobs.Free;
end;
```

Also: `WhenAll(tasks)` (a task of the array of their values) and
`WhenAny(tasks)` (a task of the index of the first to end) take a static,
dynamic or open array of tasks; `Pool.Run(F(x))` starts a task on the
thread pool (`P3_POOL_THREADS` workers, by default one per CPU, at least
2); `detached F(x)` or `detached(Handler) F(x)` lets a task run on its own,
its exception going to `Handler(E: Exception)`. A task started any other
way stays on the thread that started it, so code between waits runs where
it began (the main thread, for one started by the main program). A call
whose task is dropped warns "dangling task".

Unit `threading` has the rest: `TCancelToken` (predeclared) gets
`TCancelToken.Create`, `Cancel`, `IsCancelled`, `ThrowIfCancelled`,
`WithTimeout(ms)` and `NewChild`; `TChannel<T>` has `Create(capacity)` (0:
unbounded), `Send`, `Receive`, `TryReceive` (`(value, ok)`), `Close` and
`Count`, and raises `EChannelClosed`. The runtime is thread-safe for this:
reference counts are atomic, exceptions are per thread, and file I/O is
locked.

An async routine's task may outlive its caller, so its parameters are
copied: no `var`, `out`, `constref`, open-array or untyped ones. It is
declared at program or unit level or is a method (not virtual, not a
constructor, destructor or record method); `spawn begin ... end` blocks
are the nested ones. `async`, `await`, `spawn`, `scope`, `detached` and
`Pool` are not reserved: a visible declaration of the name keeps it a
name.

**Data-race checks** (objfpc; item 11 of the proposals): two qualifiers
before a variable's type say how tasks may share it. A `shared` variable
holds a thread-safe value, which many tasks may use at once: an object of
a class derived from the predeclared `TSharedObject` (unit `threading`'s
`TAtomic<T>`, `TLocked<T>` and `TChannel<T>`, or your own class that
synchronizes what it shares), a task or a `TCancelToken`. An `isolated`
variable (of a class, dynamic array or pointer type) belongs to one task:
its value cannot be copied (assigned, passed to a task), only handed over
with `move x`, which leaves `x` empty, and a spawned task cannot use it.

With `{$modeswitch racechecks}` a unit opts in to the rest: a spawned task
(`spawn begin ... end`) may use only `shared` variables of the routines
around it and of the program, and cannot assign them; `const` parameters
and constants that give no access to mutable data are fine too. In a
method, it may use Self's members only when the class is thread-safe or
the field is `shared`. Calling a routine nested in another (which may use
that routine's variables) is an error there.

```pascal
{$modeswitch racechecks}
uses threading;

type
  TStrings = array of string;
  TBytes = array of Byte;
  TCounter = specialize TAtomic<Integer>;
  TCache = specialize TLocked<TStrings>;

var
  counter: shared TCounter;   { Load, Store, Exchange, CompareExchange,
                                Add, Increment, Decrement }
  cache: shared TCache;       { Use runs a procedure with the lock held }
  buffer: isolated TBytes;
  total: Integer;

scope
  spawn begin
    counter.Increment;                                  { ok: atomic }
    cache.Use(procedure(var d: TStrings) => SetLength(d, 10));  { ok: locked }
    total := total + 1;   { error: 'total' is not shared }
    buffer[0] := 1;       { error: isolated data of another task }
  end;
end;
t := Process(move buffer);  { handed over; buffer is now empty }
```

`TLocked<T>.Use`'s procedure must not wait (no `await` or `Sleep`): the
lock is a plain mutex. (`With`, the proposal's name, is a reserved word.)
`shared` and `isolated` are not reserved: they are qualifiers only where a
type follows and no declaration of the word is visible.

**Capture by copy** (p3's own): `function[k, s](x) => e` is a closure
with its own copies of `k` and `s`, taken when it is made. Inside it they
are read-only ("cannot modify 'k', a copy made by this lambda"); what it
uses without listing it, it shares as closures do. Each time the lambda is
evaluated it copies anew, so closures made in a loop keep the values of
their own iteration:

```pascal
for i := 0 to n - 1 do
  fs[i] := function[i]: Integer => i * 10;   { 0, 10, 20, ... }

function Snapshot(var total: Integer): TFn;
begin
  Snapshot := function[total]: Integer => total;   { the value now }
end;
```

What it copies is a variable, a parameter (also `var` and `const` ones,
whose values are copied) or a field of the object (`[Name]` in a method:
its value now, which outlives the object). Not open-array, untyped or file
values. A lambda that copies must be a closure: a `reference to` type is
wanted where it is written, also when it chooses an overload.

`[=]` copies everything the lambda uses: every variable and parameter of
the routines around it that its body names (also in lambdas inside it).
Globals and the object's members (through Self) are shared, as without
copies; names the lambda declares itself (its parameters) are its own.

```pascal
for i := 0 to n - 1 do
  fs[i] := function[=]: Integer => i * scale + global;  { i, scale copied }
```

A `reference to` type's result may be a method pointer, a short string or
a record, as an `of object` type's. Not supported: `reference to ... of
object` (a method-pointer lambda is one already) and calling conventions,
comparing values with `=` (use `Assigned`).

**Conditional expressions** (objfpc; p3's own): `if c then a else b` is a
value, `a` where `c` holds and `b` otherwise. Only the branch chosen is
evaluated, so `if n = 0 then 0 else total div n` is safe. Both branches
are needed; they nest (`if x < 0 then -1 else if x > 0 then 1 else 0`).

```pascal
sign  := if n < 0 then -1 else 1;
label := if count = 1 then 'item' else 'items';
Writeln(if ok then 'yes' else 'no');
```

Its type is the type wanted of it (in an assignment, a typed declaration,
`exit`), which both branches must fit; elsewhere it is the common type of
the branches, as for the elements of `[a, b]`: `if c then 1 else 2.5` is a
`Real`, an `Integer` and a `string` are an error. It is a value, not a
variable: it cannot be assigned to or passed to a `var` parameter. It can
only be written among the statements of a routine or program, not in a
constant.

**Tuples** (objfpc; p3's own, not Free Pascal's): several values as one,
without declaring a record. A tuple type lists its element types between
parentheses, `(Integer, string)`, and can name them, `(Min: Integer, Max:
Integer)`; a tuple value lists expressions, `(lo, hi)`. A function can
return one, and its elements can be given to variables in one statement:

```pascal
function MinMax(const A: array of Integer): (Min: Integer, Max: Integer);
begin
  Result := (A[0], A[0]);
  ...
end;

var (lo, hi) := MinMax(values);     { two new variables }
var r := MinMax(values);
Writeln(r.Min, ' to ', r.Max);
(a, b) := (b, a + b);               { both values first, then both stores }
(q, rem) := DivMod(17, 5);          { existing variables, fields, a[i] ... }
```

- **Types.** A tuple type is written wherever a type is, also as a
  function's result or a parameter's type. Two tuple types written alike
  (the same element types and names, in order) are the same type; tuples
  whose element types match are assigned to each other whatever the names.
  Elements without a name are `Item1`, `Item2`, ...; a tuple is otherwise
  a record (`t.Min := 0`, `with t do`, elements of any type but files,
  reference-counted ones included).
- **Values.** Written for a tuple type (an assignment, an argument, a
  result, `exit((a, b))`), each element is converted as an assignment to
  that element would (`(1, 2)` fits `(Real, Byte)`). With no type to go
  by (`var t := (1, 'a')`), each element has the type a variable declared
  with it would have (`'a'` there is a string; in `(c, n) := ('a', 1)` the
  targets have types and it stays a character), and that is also what chooses among overloaded
  routines. A tuple variable's initial value is `(1, 'a')`, or
  with names `(Min: 1; Max: 2)` as for a record.
- **Destructuring.** `(a, b) := e` assigns the elements of the tuple `e` to
  the targets, each checked and converted like an assignment (anything
  assignable: variables, fields, array elements, properties). `e` is
  evaluated once, before any target changes, so `(a, b) := (b, a)` swaps.
  `var (a, b) := e` declares the variables, with the element types.
- **Enumerations.** `(A, B)` is also how an enumeration is written. In a
  type declaration a list of plain names is a tuple type only when every
  name is the name of a type; `(Red, Green)` stays an enumeration.

Not supported: comparing tuples with `=`, writing one with `write`,
nested patterns (`((a, b), c) := e`), `_` to skip an element, tuples in
constant expressions, and `for var (k, v) in`. Tuples are meant for small,
local uses: a value that is part of a public interface reads better as a
named record.

**Pointer arithmetic** (tp and objfpc, as in Free Pascal): `p + n`, `n + p`
and `p - n` move a pointer by n of what it points to (by n bytes for an
untyped `pointer`, and so for `@x`, which is untyped); `p - q` is the
number of them between two pointers of the same type (an `int64`);
`inc(p[, n])` and `dec(p[, n])` move a pointer variable; `p[i]` is
`(p + i)^`, a variable, for any typed pointer; `<`, `<=`, `>` and `>=`
order pointers by address. A parenthesized expression takes `^`, `[i]` and
`.field` (`(p + 1)^ := 0`). Nothing is checked: there are no bounds.
`write` and `writeln` take a `pchar` (any pointer to `char`): the
characters up to the NUL, nothing for `nil`.

```pascal
function CLength(s: pchar): longint;
var
  e: pchar;
begin
  e := s;
  while e^ <> #0 do
    inc(e);
  CLength := e - s;
end;
```

**Files named with `Assign`** (tp and objfpc, as in Free Pascal; also
`AssignFile` and `CloseFile` in objfpc): `Assign(f, name)` names a text, typed
or untyped (`file`) file, and `Close` closes it. A typed or untyped file
named this way works as in Turbo Pascal: `Reset` opens it for reading and
writing, `Rewrite` creates it, and `Seek`, `FilePos`, `FileSize` and `Eof`
work in records. `Reset(f, n)` / `Rewrite(f, n)` give an untyped file
records of n bytes (128 if not given); `BlockRead(f, buf, count[, done])`
and `BlockWrite` move whole records, and without `done` reading fewer is a
runtime error. An empty name is standard input or output. `Flush(f)`
writes out what a text file open for writing has buffered (`Flush(output)`
before output that another reader must see now: `output` is buffered when
it is not a terminal). I/O errors are
runtime errors (no `{$I-}` or `IOResult` yet). Program-parameter and
internal typed files keep ISO's buffer variable (`f^`, `get`, `put`).
`GetFileHandle(f)` gives the operating system's handle of any file variable
(text, typed or untyped), the file descriptor, as a `THandle` (`longint`,
as in Free Pascal on Unix): `input` is 0, `output` 1, and a file that is
not open -1 (Free Pascal 3.2.2 has no `GetFileHandle`; its
`FileRec(f).Handle` keeps a closed file's descriptor).

**`{$push}` / `{$pop}`**, as in Free Pascal: `{$pop}` restores the local
switches the matching `{$push}` saved (nested, at most 21 levels): `{$H}`,
`{$R}`, `{$Q}` (also back to the command line's default when no switch
preceded the `{$push}`), the record alignment (`{$packrecords}`, `{$align}`,
`{$A}`), `{$scopedenums}`, `{$inline}` and `{$interfaces}`. Mode switches
and `{$macro}` are not local, as in FPC. A `{$pop}` without a `{$push}` is
an error.

**C operators** (`+=`, `-=`, `*=`, `/=`), as in Free Pascal: `x += e` is
`x := x + (e)` (likewise `-`, `*`, `/`), for any type the operator works
on (numbers, strings, sets). As in FPC, `x` is read a second time, so a
call in an index such as `a[f()] += 1` runs twice; inside function `f`,
`f += e` adds to the result (it does not call `f`); a property cannot be
the target. They are on by default in `--mode=tp` and `--mode=objfpc` (as
in FPC, in every mode) and off in `--mode=iso`; `{$coperators on|off}`
(also `+`/`-`) switches them, and like FPC, `{$pop}` does not restore it.

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
integer; stdcall;`). p3 calls every routine with the target's C
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
compiled as `F<integer>`. A call without `specialize` infers the type
arguments (see "Inferred types"). Not supported: constraints, `@specialize F<T>`, and, for a unit compiled separately, generics that use
what the unit does not export (whole-program compilation allows it).

**Generic types** (`--mode=objfpc`), as in Free Pascal: `generic TName<T>
= ...` declares a template for a class, a record (with or without
methods), an interface, an array or a procedural type, and `specialize
TName<Integer>` is the type made of it for those type arguments.

```pascal
type
  generic TList<T> = class
  private
    FItems: array of T;
  public
    procedure Add(const Item: T);
    function Count: Integer;
    property Items[i: Integer]: T read GetItem; default;
  end;
  generic TPair<K, V> = record
    Key: K;
    Value: V;
  end;

procedure TList.Add(const Item: T);      { a template too }
begin
  ...
end;

type
  TIntList = specialize TList<Integer>;
var
  names: specialize TList<string>;
  p: specialize TPair<string, Real>;
begin
  names := specialize TList<string>.Create;
```

- A specialization is the same type wherever it is written with the same
  type arguments, and a different type for other arguments. It is named
  `TList<integer>` in messages.
- `specialize TName<...>` is written wherever a type is, also as a
  parameter or result type, inside other generics (`specialize TPair<Integer,
  T>`), as a class's ancestor through a name (`TLoud = class(TIntList)`),
  and in an expression before a member (`specialize TList<string>.Create`).
- The implementations of a generic class's or record's methods are written
  with the generic's name, without type parameters (`TList.Add`). Inside
  the generic its own name means the specialization being made (`Next:
  TNode`).
- Everything classes can do works in a generic one: constructors, virtual
  methods and overriding, properties, class methods and class variables
  (each specialization has its own), interfaces.
- A specialization is made once per program or unit: the generic's tokens,
  and its methods', are parsed again with the type parameters naming the
  type arguments and checked like any declaration. An error in it points
  into the generic, with a note where the specialization was asked for.

Generics are declared at program or unit level, also in a unit's interface
with the methods in its implementation. Not supported: constraints
(`<T: TObject>`), nested generic types, Delphi's `TList<Integer>` without `specialize`, and for a unit
compiled separately (`-fseparate-units`) generics whose methods use what
the unit does not export; each separately compiled module that uses a
specialization also has its own copy of it.

**Generic methods** (`--mode=objfpc`), as in Free Pascal 3.2: a class
declares `generic function M<T>(...)` (or `generic procedure`, `generic
class function`), implemented as `generic function TFoo.M<T>(...)`, and
`obj.specialize M<Integer>(x)` calls it. As for generic routines, p3
also infers the type arguments from the arguments, `obj.M(x)`; inside a
method `M(x)` and `specialize M<T>(x)` call it on Self. Generic classes
can have generic methods too:

```pascal
type
  generic TList<T> = class
    ...
    generic function Map<R>(F: specialize TFunc<T, R>): specialize TList<R>;
  end;

strs := ints.Map(function(x) => IntToStr(x));   { Map<string>: x is an Integer }
total := ints.Fold(0, function(acc, x) => acc + x);
```

Each specialization (`TList<integer>.Map<ansistring>`) is a method of the
class of its own, made from the implementation, so a generic method cannot
be virtual, override or abstract, a constructor, a destructor or an
operator. Its implementation must come before its first use, and is
checked for each specialization (its declaration gives the shape: type
parameters, parameter kinds). Type helpers can have generic methods too;
advanced records cannot yet.

**Generic type helpers** (`--mode=objfpc`, `{$modeswitch typehelpers}`;
p3's own): `generic TH<T> = type helper for array of T` extends every
dynamic array, `T` being its element type, and a helper `for specialize
TGen<T>` (a record helper for a generic record) every matching
specialization. The helper is specialized where a value uses it, for the
types the match gives; with generic methods and inference through lambdas
this gives sequence operations on arrays:

```pascal
type
  generic TArrayHelper<T> = type helper for array of T
    function Count: Integer;
    function Filter(F: specialize TPred<T>): specialize TArray<T>;
    generic function Map<R>(F: specialize TFunc<T, R>): specialize TArray<R>;
  end;

evens := ints.Filter(function(x) => not Odd(x));      { any array of Integer }
labels := ints.Map(function(x) => 'n' + IntToStr(x));  { an array of string }
```

As in Free Pascal, one helper applies to a type: an exact helper (`type
helper for array of Byte`, now also written with `array of`) comes first,
then the generic ones, the last declared first, which hides the earlier
ones' methods. The extended type must be built from all the type
parameters (`array of T`, `specialize TGen<T, string>`); a helper for a
bare `T` (every type) is an error. Classes and interfaces have no type
helpers.

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
name per default. A parameter after one with a default need not have one
(unlike Free Pascal): it is then given by name, or by position with all those
before it. The value is a
constant that fits the parameter: ordinals, reals, strings, sets, `nil`. It is
passed by the caller, as if written in the call, and comes from the heading the
call sees: a routine's or method's implementation may leave defaults out or
repeat them with the same values, and an override has its own. They also work
on `forward` and interface headings, in units, on methods, constructors,
interface methods and procedural types. Unlike Free Pascal, a value that does
not fit the parameter's range (`b: byte = 300`) is an error.

**Named arguments** (objfpc; p3's own, not Free Pascal's): `Name := value`
among the arguments of a call is the argument for the parameter `Name`, so
defaults in the middle of a list can be skipped and a call says what its
values mean.

```pascal
procedure Connect(Host: string; Port: Integer = 5432;
                  Timeout: Integer = 30; UseTls: Boolean = True);

Connect('db.local');
Connect('db.local', Timeout := 5);              { skips Port }
Connect(Host := 'db.local', UseTls := False);
```

- Arguments by position come first and are for the first parameters; named
  ones follow, in any order. Each parameter is given once, and one that is
  not given must have a default value.
- They work wherever the parameters have names: routines, methods,
  constructors, `inherited` calls, interface methods (the names and
  defaults of the interface's heading) and procedural variables (those of
  the procedural type). Not the standard routines (`writeln`, `length`,
  ...) nor typecasts. `var` and `out` parameters can be named too.
- The arguments are evaluated in the order of the parameters, not in the
  order written.
- Renaming a parameter breaks the callers that name it: parameter names
  are part of a routine's interface.

**Overloads with defaults**: the arguments written decide first, by their
conversions, as before. Among the routines that fit equally well, the one
that leaves the fewest parameters to their default values is called, so a
routine that takes exactly the arguments written beats one that needs
defaults (`P(1)` calls `P(a: Integer)`, not `P(a: Integer; b: Integer = 0)`;
Free Pascal 3.2.2 rejects that call as ambiguous, as p3 did). A tie that
remains is
an error that lists the candidates, never a silent choice. A named argument
only fits the routines that have a parameter of that name, which can tell
overloads apart: `Show(3, Text := 'x')`.

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
variadics there; `varargs` is p3's form).

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
{$IF defined(P3_OBJFPC) and not defined(SLOW)}
  ...
{$ELSEIF defined(P3_TP)}
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
noticed. `P3` is
predefined, and so is one of `P3_ISO`, `P3_TP` and `P3_OBJFPC` (the
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
compares strings in `$IF` by length only; p3 compares them as strings.
Not supported: macro values on the command line (`-dNAME:=value`).

**Self-hosting:** p3 is also written in Pascal ([selfhost/](selfhost/),
[SELFHOST.md](SELFHOST.md)); `ninja -C build check-bootstrap` builds it with
itself, and `utils/seed.py` makes and uses a stage-1 *seed* (a binary plus
the compiler's LLVM IR) to bootstrap it without this C++ compiler.

**ISO 7185 conformance:** the Pascal-P5 test `iso7185pat.pas` passes all 793
checks (`utils/iso7185pat.py build/bin/p3`).

## Benchmarks

```sh
utils/bench.py --p3 build/bin/p3 --fpc          # --base <older p3> to compare two builds
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
cmake -S . -B build-fuzz -G Ninja -DP3_ENABLE_FUZZING=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined"
ninja -C build-fuzz p3-fuzz
build-fuzz/bin/p3-fuzz -max_total_time=300 corpus/ tests/run tests/*/Inputs
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
$ p3 sum.pas && echo "17 25" | ./sum
17 + 25 = 42
even? true
```

## Requirements

- LLVM 19 (dev headers + `FileCheck`), e.g. Debian/Ubuntu:
  `llvm-19-dev llvm-19-tools clang-19`; macOS: `brew install llvm@19`
- CMake ≥ 3.20, Ninja, a C++20 compiler
- `lit` (`pip install lit`)
- At run time, a C compiler driver (`clang` or `cc`) is used as the linker;
  override with `P3_CC`. The runtime library is found at
  `<p3 dir>/../lib/libpascalrt.a` (override with `P3_RUNTIME_LIB`), the
  unit library at `<p3 dir>/../lib/pascal` (override with `P3_UNIT_DIR`).
- Optional: LLVM's own `clang` and `llvm-link` (in the LLVM tools
  directory). With them the build also writes the runtime as LLVM IR,
  `lib/libpascalrt.ll`, next to the library (override with
  `P3_RUNTIME_IR`). From `-O1`, when compiling for the host, p3 links
  that file into the program's module before optimizing it, so that the
  runtime's small functions (reference counting, string assignment and
  comparison, exception frames, type tests) are inlined; the program still
  links the library, which the calls that are not inlined go to. Without
  the file, or with `-fno-inline-runtime`, every runtime function is
  called.

## Build & test

```sh
cmake -S . -B build -G Ninja            # add -DLLVM_DIR=<prefix>/lib/cmake/llvm if not found
ninja -C build                          # -> build/bin/p3
ninja -C build check                    # run the lit test suite
```

## Usage

```sh
p3 hello.pas -o hello        # executable (default)
p3 -c hello.pas              # object file
p3 -S hello.pas              # assembly
p3 --emit-llvm hello.pas     # LLVM IR
p3 --emit-tokens hello.pas   # lexer dump
p3 --emit-ast hello.pas      # AST dump
p3 --help                    # all options (-O0..3, -g, --target, --mode=iso|tp|objfpc, ...)
```

### Project files (p3.json)

A project can keep its options in a `p3.json` file, so that `p3` alone
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

p3 looks for `p3.json` in the input file's directory (the current
directory when there is no input file) and then in its parents;
`--project=<file>` names one and `--no-project` ignores it. Its settings are
defaults: options on the command line override them (lists, such as unit
paths and defines, are added after the command line's). Paths in it are
relative to its directory. Without an input file, p3 builds `main` (into
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
reported like source errors (`p3.json:3:15: error: 'optimize' must be an
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
| `examples/` | projects built with the self-hosted compiler (`examples/p3`, `examples/new <name>`); see [examples/README.md](examples/README.md) |

## Writing tests

Tests are lit files with `RUN:` lines; `%p3` is the compiler under test and
`FileCheck`/`not` come from LLVM. Example:

```
; RUN: not %p3 %S/Inputs/bad.pas 2>&1 | FileCheck %s
; CHECK: bad.pas:3:5: error: undeclared identifier 'x'
```

C++ unit tests use the tiny `TEST`/`EXPECT_*` macros in `tests/unit/UnitTest.h`.
