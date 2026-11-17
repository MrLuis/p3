{ Scoped checked arithmetic (objfpc): `checked ... end` checks its integer
  arithmetic for overflow (an EIntOverflow exception) whatever $Q says;
  `unchecked ... end` wraps, also when an integer value is converted to a
  narrower integer type (two's complement: the low bits). AddChecked,
  SubChecked and MulChecked give (value, ok) instead of raising.
  checked(e) and unchecked(e) do the same for one expression.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/checked.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/checked.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/checked.expected
}
{$H+}
program checked;

{$Q-}
function TotalSize(const Sizes: array of Cardinal): QWord;
begin
  checked
    Result := 0;
    for var s in Sizes do
      Result := Result + s;   { raises EIntOverflow if it wraps }
  end;
end;

function Wrapping(a, b: Integer): Integer;
begin
  Result := a + b; { $Q-: wraps }
end;

function CheckedSum(a, b: Integer): Integer;
begin
  checked
    Result := a + b;
  end;
end;

function CheckedMul(a, b: Integer): Integer;
begin
  CheckedMul := checked(a * b);
end;
{$Q+}


{ FNV-1a: the wrapping is intended }
function Hash(const s: string): Cardinal;
var
  i: Integer;
begin
  Result := 2166136261;
  unchecked
    for i := 1 to Length(s) do
      Result := (Result xor Ord(s[i])) * 16777619;
  end;
end;

procedure Exprs;
var
  x: Integer;
  b: Byte;
  h: Cardinal;
begin
  try
    Writeln(CheckedMul(3, 4));
    Writeln(CheckedMul(MaxInt, 2));
  except
    on E: EIntOverflow do
      Writeln('checked(): ', E.Message);
  end;
  x := MaxInt;
  x := checked(x - 1) - 1;
  b := 250;
  b := unchecked(b + 10);
  h := 4000000000;
  h := unchecked(h * 3);
  Writeln(x, ' ', b, ' ', h, ' ', checked(High(Integer) - 1));
end;

{ a declaration of that name: unchecked(x) calls it }
function unchecked(x: Integer): Integer;
begin
  unchecked := x + 1000;
end;

var
  x, y: Integer;
  b: Byte;
  i64: Int64;
  r: (Integer, Boolean);
  checked: Integer;   { the word is not reserved }
begin
  Writeln(TotalSize([1, 2, 3]));
  Writeln(Wrapping(MaxInt, 1));
  Writeln(Hash('abc'));

  try
    Writeln(CheckedSum(MaxInt, 1));
  except
    on E: EIntOverflow do
      Writeln('checked: ', E.ClassName, ': ', E.Message);
  end;

  x := MaxInt;
  unchecked
    x := x + 1;
    y := -x;           { -(-2147483648) wraps to itself }
    Inc(x, -1);        { back to MaxInt }
    b := 250;
    b := b + 10;       { 260 -> 4 }
    checked            { nested: checked again }
      try
        x := x + 1;
      except
        on E: EIntOverflow do
          Writeln('nested checked: ', E.Message);
      end;
    end;
    {$Q+}              (* a directive inside switches again *)
    try
      x := x + 1;
    except
      on E: EIntOverflow do
        Writeln('$Q+ inside: ', E.Message);
    end;
  end;
  Writeln(x, ' ', y, ' ', b);

  { outside the blocks ($Q+): an exception in objfpc }
  try
    x := MaxInt;
    x := x * 2;
  except
    on E: EIntError do
      Writeln('outside: ', E.ClassName);
  end;

  r := AddChecked(MaxInt, 1);
  Writeln(r.Item1, ' ', r.Item2);
  var (v, ok) := MulChecked(1000, 1000);
  Writeln(v, ' ', ok);
  var (d, dok) := SubChecked(Cardinal(1), Cardinal(2));
  Writeln(d, ' ', dok);
  i64 := High(Int64);
  var (w, wok) := AddChecked(i64, 1);
  Writeln(w, ' ', wok);

  Exprs;
  Writeln(unchecked(1));

  checked := 5;
  Writeln(checked);
end.
