{ Advanced records ($modeswitch advancedrecords, --mode=objfpc): fields
  with visibility, methods (Self is the record, by reference), a
  constructor, static class methods, properties with field and method
  accessors, overloaded methods, methods called on variables, array
  elements, fields, function results and through `with`; class operators
  (binary and unary, comparisons with <> derived from =, `in`, `:=`
  conversions both ways, Explicit casts) and the implicit conversions they
  give to assignments, arguments, results and other operators' operands.
  Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/advrecords.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/advrecords.expected %t.O2.out
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/advrecords.expected %t.g.out
}
program advrecords;

{$modeswitch advancedrecords}

type
  TVec = record
  private
    FX, FY: integer;
    function GetLen2: integer;
    procedure SetY(v: integer);
  public
    Tag: char;
    constructor Create(ax, ay: integer);
    procedure Scale(k: integer); overload;
    procedure Scale(kx, ky: integer); overload;
    procedure Reset;
    function Str: string;
    function Plus(const o: TVec): TVec;
    class function Zero: TVec; static;
    class function Count: integer; static;
    class operator +(const a, b: TVec): TVec;
    class operator -(const a, b: TVec): TVec;
    class operator -(const a: TVec): TVec;
    class operator *(const a: TVec; k: integer): TVec;
    class operator *(k: integer; const a: TVec): TVec;
    class operator =(const a, b: TVec): boolean;
    class operator <(const a, b: TVec): boolean;
    class operator in(k: integer; const a: TVec): boolean;
    class operator :=(k: integer): TVec;
    class operator :=(const a: TVec): string;
    class operator Explicit(const a: TVec): integer;
    property X: integer read FX write FX;
    property Y: integer read FY write SetY;
    property Len2: integer read GetLen2;
  end;

  TFlags = record
    Bits: integer;
    class operator and(const a, b: TFlags): TFlags;
    class operator or(const a, b: TFlags): TFlags;
    class operator not(const a: TFlags): TFlags;
    class operator div(const a: TFlags; k: integer): TFlags;
    class operator mod(const a: TFlags; k: integer): TFlags;
    class operator :=(b: integer): TFlags;
    function Show: string;
  end;

  TLine = record
    A, B: TVec;
    Name: string;
    function Length2: integer;
  end;

var
  made: integer;

function Digit(i: integer): string;
begin
  if i < 0 then
    Digit := '-' + chr(ord('0') - i)
  else
    Digit := chr(ord('0') + i);
end;

constructor TVec.Create(ax, ay: integer);
begin
  FX := ax;
  FY := ay;
  Tag := 'c';
  made := made + 1;
end;

function TVec.GetLen2: integer;
begin
  Result := FX * FX + FY * FY;
end;

procedure TVec.SetY(v: integer);
begin
  writeln('  SetY ', v);
  FY := v;
end;

procedure TVec.Scale(k: integer);
begin
  Scale(k, k);
end;

procedure TVec.Scale(kx, ky: integer);
begin
  FX := FX * kx;
  Self.FY := Self.FY * ky;
end;

procedure TVec.Reset;
begin
  Self := Zero;
end;

function TVec.Str: string;
begin
  Str := '(' + Digit(FX) + ',' + Digit(FY) + ')';
end;

function TVec.Plus(const o: TVec): TVec;
begin
  Result := Self + o;
end;

class function TVec.Zero: TVec;
begin
  Result.FX := 0;
  Result.FY := 0;
  Result.Tag := 'z';
end;

class function TVec.Count: integer;
begin
  Count := made;
end;

class operator TVec.+(const a, b: TVec): TVec;
begin
  Result := TVec.Create(a.FX + b.FX, a.FY + b.FY);
end;

class operator TVec.-(const a, b: TVec): TVec;
begin
  Result := TVec.Create(a.FX - b.FX, a.FY - b.FY);
end;

class operator TVec.-(const a: TVec): TVec;
begin
  Result := TVec.Create(-a.FX, -a.FY);
end;

class operator TVec.*(const a: TVec; k: integer): TVec;
begin
  Result := a;
  Result.Scale(k);
end;

class operator TVec.*(k: integer; const a: TVec): TVec;
begin
  Result := a * (k + 1);
end;

class operator TVec.=(const a, b: TVec): boolean;
begin
  Result := (a.FX = b.FX) and (a.FY = b.FY);
end;

class operator TVec.<(const a, b: TVec): boolean;
begin
  Result := a.Len2 < b.Len2;
end;

class operator TVec.in(k: integer; const a: TVec): boolean;
begin
  Result := (k = a.FX) or (k = a.FY);
end;

class operator TVec.:=(k: integer): TVec;
begin
  Result := TVec.Create(k, k);
end;

class operator TVec.:=(const a: TVec): string;
begin
  Result := 'vec' + a.Str;
end;

class operator TVec.Explicit(const a: TVec): integer;
begin
  Result := a.FX * 10 + a.FY;
end;

class operator TFlags.and(const a, b: TFlags): TFlags;
begin
  Result.Bits := a.Bits and b.Bits;
end;

class operator TFlags.or(const a, b: TFlags): TFlags;
begin
  Result.Bits := a.Bits or b.Bits;
end;

class operator TFlags.not(const a: TFlags): TFlags;
begin
  Result.Bits := a.Bits xor 7;
end;

class operator TFlags.div(const a: TFlags; k: integer): TFlags;
begin
  Result.Bits := a.Bits div k;
end;

class operator TFlags.mod(const a: TFlags; k: integer): TFlags;
begin
  Result.Bits := a.Bits mod k;
end;

class operator TFlags.:=(b: integer): TFlags;
begin
  Result.Bits := b;
end;

function TFlags.Show: string;
begin
  Show := Digit(Bits);
end;

function TLine.Length2: integer;
begin
  Length2 := (B - A).Len2;
end;

function Make(x, y: integer): TVec;
begin
  Make := TVec.Create(x, y);
end;

function Twice(const v: TVec): TVec;
begin
  Twice := v + v;
end;

function AsVec(k: integer): TVec;
begin
  exit(k);                        { converted by := }
end;

procedure Show(const label_: string; v: TVec);
begin
  writeln(label_, ' ', v.Str, ' ', v.Tag);
end;

var
  v, w: TVec;
  vs: array[1..3] of TVec;
  line: TLine;
  f, g: TFlags;
  s: string;
  i: integer;

begin
  writeln('methods');
  v := TVec.Create(1, 2);
  writeln('  ', v.Str, ' ', v.X, ' ', v.Y, ' ', v.Len2, ' ', v.Tag);
  v.Scale(2);
  v.Scale(1, 3);
  writeln('  ', v.Str);
  v.X := 5;
  v.Y := 1;
  writeln('  ', v.Str, ' count ', TVec.Count);
  w := v;
  w.Reset;
  writeln('  reset ', w.Str, ' ', w.Tag, ' ', v.Str);
  writeln('  ', Make(3, 4).Len2, ' ', Make(1, 1).Plus(Make(2, 2)).Str,
          ' ', TVec.Zero.Str);
  with v do
  begin
    Scale(2);
    writeln('  with ', Str, ' ', X, ' ', Len2);
  end;

  writeln('arrays and fields');
  for i := 1 to 3 do
    vs[i] := TVec.Create(i, i * 2);
  vs[2].Scale(3);
  writeln('  ', vs[1].Str, ' ', vs[2].Str, ' ', vs[3].Str);
  line.A := TVec.Create(1, 1);
  line.B := TVec.Create(4, 5);
  line.Name := 'l';
  line.A.Scale(2);
  writeln('  ', line.Name, ' ', line.A.Str, ' ', line.Length2);

  writeln('operators');
  v := TVec.Create(1, 2);
  w := TVec.Create(3, 1);
  writeln('  ', (v + w).Str, ' ', (v - w).Str, ' ', (-v).Str, ' ',
          (v * 3).Str, ' ', (2 * v).Str);
  writeln('  ', v = w, ' ', v <> w, ' ', v = TVec.Create(1, 2), ' ',
          v < w, ' ', w < v);
  writeln('  ', 2 in v, ' ', 5 in v, ' ', Twice(w).Str);

  writeln('conversions');
  v := 7;
  writeln('  ', v.Str);
  Show('  arg', 3);
  w := v + 1;
  writeln('  ', w.Str, ' ', (1 + w).Str);
  s := v;
  writeln('  ', s);
  writeln('  ', integer(TVec.Create(4, 2)), ' ', AsVec(9).Str);

  writeln('flags');
  f := 6;
  g := 3;
  writeln('  ', (f and g).Show, ' ', (f or g).Show, ' ', (not f).Show, ' ',
          (f div 4).Show, ' ', (f mod 4).Show);
  writeln('made ', TVec.Count);
end.
