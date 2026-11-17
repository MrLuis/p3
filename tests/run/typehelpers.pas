{ Type helpers (objfpc, with the typehelpers modeswitch), as in Free Pascal:
  `TH = type helper for T` adds methods, properties, constants and static
  class methods to T's values (ansistrings, shortstrings, integers, enums,
  booleans, reals, records, dynamic arrays). Self is the value, by
  reference: a method can change a variable; on a value (a constant, a
  function's result) it works on a temporary. Static methods are called
  through the extended type (AnsiString.Join). The helper declared last
  for a type is used; it can extend an earlier one. A record helper's
  methods see the record's fields. Output is identical to Free Pascal
  3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/typehelpers.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/typehelpers.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/typehelpers.expected
}
program typehelpers;
{$H+}
{$modeswitch typehelpers}
{$modeswitch advancedrecords}
uses sysutils;

type
  TColor = (red, green, blue);
  TPoint = record
    x, y: longint;
  end;
  TBytes = array of byte;

  TAnsiStringHelper = type helper for AnsiString
  private
    function GetCount: SizeInt;
  public
    const Empty = '';
    function ToUpper: AnsiString;
    function Contains(const Value: AnsiString): Boolean;
    procedure Append(const Value: AnsiString);
    procedure Clear; inline;
    class function Join(const a, b: AnsiString): AnsiString; static;
    property Count: SizeInt read GetCount;
  end;

  TIntHelper = type helper for longint
    function ToString: AnsiString;
    function Twice: longint; inline;
    procedure Inc;
    procedure Add(n: longint); overload;
    procedure Add(const s: AnsiString); overload;
    class function Parse(const s: AnsiString): longint; static;
  end;

  TColorHelper = type helper for TColor
    function Name: AnsiString;
    function Next: TColor;
  end;

  TPointHelper = type helper for TPoint
    function Sum: longint;
    procedure Move(dx, dy: longint);
    function Text: AnsiString;
  end;

  TShortHelper = type helper for ShortString
    function Twice: ShortString;
  end;

  TBytesHelper = type helper for TBytes
    function Total: longint;
    procedure Push(b: byte);
  end;

  TDoubleHelper = type helper for double
    function Half: double;
  end;

  TBoolHelper = type helper for boolean
    function YesNo: AnsiString;
  end;

  { the last helper declared for a type is the one used; it can extend
    an earlier one }
  TIntHelper2 = type helper(TIntHelper) for longint
    function Squared: longint;
  end;

function TAnsiStringHelper.GetCount: SizeInt;
begin
  result := Length(Self);
end;

function TAnsiStringHelper.ToUpper: AnsiString;
begin
  result := UpperCase(Self);
end;

function TAnsiStringHelper.Contains(const Value: AnsiString): Boolean;
begin
  result := Pos(Value, Self) > 0;
end;

procedure TAnsiStringHelper.Append(const Value: AnsiString);
begin
  Self := Self + Value;
end;

procedure TAnsiStringHelper.Clear;
begin
  Self := Empty;
end;

class function TAnsiStringHelper.Join(const a, b: AnsiString): AnsiString;
begin
  result := a + ',' + b;
end;

function TIntHelper.ToString: AnsiString;
begin
  result := IntToStr(Self);
end;

function TIntHelper.Twice: longint;
begin
  result := Self * 2;
end;

procedure TIntHelper.Inc;
begin
  Self := Self + 1;
end;

procedure TIntHelper.Add(n: longint);
begin
  Self := Self + n;
end;

procedure TIntHelper.Add(const s: AnsiString);
begin
  Add(StrToInt(s));
end;

class function TIntHelper.Parse(const s: AnsiString): longint;
begin
  result := StrToInt(s);
end;

function TIntHelper2.Squared: longint;
begin
  result := Self * Self;
end;

function TColorHelper.Name: AnsiString;
begin
  case Self of
    red: result := 'red';
    green: result := 'green';
  else
    result := 'blue';
  end;
end;

function TColorHelper.Next: TColor;
begin
  if Self = High(TColor) then
    result := Low(TColor)
  else
    result := Succ(Self);
end;

function TPointHelper.Sum: longint;
begin
  result := x + y;
end;

procedure TPointHelper.Move(dx, dy: longint);
begin
  x := x + dx;
  Self.y := Self.y + dy;
end;

function TPointHelper.Text: AnsiString;
begin
  result := '(' + x.ToString + ',' + y.ToString + ')';
end;

function TShortHelper.Twice: ShortString;
begin
  result := Self + Self;
end;

function TBytesHelper.Total: longint;
var
  b: byte;
begin
  result := 0;
  for b in Self do
    result := result + b;
end;

procedure TBytesHelper.Push(b: byte);
begin
  SetLength(Self, Length(Self) + 1);
  Self[High(Self)] := b;
end;

function TDoubleHelper.Half: double;
begin
  result := Self / 2;
end;

function TBoolHelper.YesNo: AnsiString;
begin
  if Self then
    result := 'yes'
  else
    result := 'no';
end;

function MakeName: AnsiString;
begin
  result := 'made';
end;

procedure ShowConst(const s: AnsiString; n: longint);
begin
  writeln(s.ToUpper, ' ', s.Count, ' ', n.Twice);
  n.Inc; { a value parameter: a local copy }
  writeln(n);
end;

const
  Greeting: AnsiString = 'hi';

var
  s: AnsiString;
  ss: ShortString;
  i: longint;
  c: TColor;
  p: TPoint;
  pts: array[1..2] of TPoint;
  bs: TBytes;
  d: double;
  names: array of AnsiString;
  ok: boolean;

begin
  writeln('strings');
  s := 'hello';
  writeln(s.ToUpper, ' ', s.Contains('ell'), ' ', s.Count);
  s.Append(' world');
  writeln(s);
  writeln(s.ToUpper.Contains('WORLD'), ' ', s.ToUpper.Count);
  writeln(AnsiString.Join('c', 'd'), ' [', AnsiString.Empty, ']');
  writeln('lit'.ToUpper, ' ', 'xyz'.Count);
  writeln(MakeName.ToUpper, ' ', Greeting.ToUpper);
  s.Clear;
  writeln('[', s, '] ', s.Count);
  SetLength(names, 2);
  names[0] := 'a';
  names[1] := 'bb';
  names[1].Append('c');
  writeln(names[1], ' ', names[1].Count);
  ShowConst('const', 4);

  writeln('integers');
  i := 20;
  writeln(i.ToString, ' ', i.Twice, ' ', i.Squared);
  i.Inc;
  writeln(i);
  i.Add(5);
  i.Add('100');
  writeln(i, ' ', longint.Parse('42') + 1);
  writeln(i.Twice.Twice);

  writeln('enums, records, arrays, reals');
  c := green;
  writeln(c.Name, ' ', c.Next.Name, ' ', blue.Next.Name);
  p.x := 1;
  p.y := 2;
  writeln(p.Sum, ' ', p.Text);
  p.Move(10, 20);
  writeln(p.Text);
  pts[2] := p;
  pts[2].Move(1, 1);
  writeln(pts[2].Text, ' ', p.Text);
  bs := nil;
  bs.Push(3);
  bs.Push(4);
  writeln(Length(bs), ' ', bs.Total);
  ss := 'ab';
  writeln(ss.Twice);
  d := 5;
  writeln(d.Half:0:2);
  ok := i > 0;
  writeln(ok.YesNo, ' ', false.YesNo);
end.
