{ Record helpers (objfpc, with the advancedrecords modeswitch), as in Free
  Pascal: `TH = record helper for TRec` adds methods, properties, constants
  and static class methods to a record type, as Free Pascal's sysutils does
  with `TGuidHelper = record helper for TGUID` (TGUID and PGUID are
  predeclared, as in Free Pascal's system unit). Self is the record, by
  reference: a method can change a variable; on a value (a function's
  result) it works on a temporary. Static methods and constants are used
  through the record type (TGUID.Empty). A helper's methods see the
  record's fields and methods, and a helper's member hides the record's
  own. The helper declared last for a record is used; it can extend an
  earlier one. Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/recordhelpers.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/recordhelpers.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/recordhelpers.expected
}
program recordhelpers;
{$H+}
{$modeswitch advancedrecords}

uses sysutils;

type
  TGuidHelper = record helper for TGUID
  private
    function GetVersion: longint;
  public
    const Braces = '{}';
    class function Empty: TGUID; static;
    class function Create(d1: longword; d2, d3: word; b: byte): TGUID;
      overload; static;
    class function Create(d1: longword): TGUID; overload; static;
    function IsEmpty: boolean;
    function ToString(SkipBrackets: boolean = false): string;
    procedure Clear;
    procedure Bump(by: longint);
    property Version: longint read GetVersion;
  end;

  { An advanced record with methods of its own }
  TPoint = record
    x, y: longint;
    function Sum: longint;
    function Describe: string;
  end;

  TPointHelper = record helper for TPoint
    function Describe: string; { hides TPoint.Describe }
    function Twice: TPoint;
    procedure Swap;
    function Product: longint;
  end;

  { A later helper extends the earlier one and replaces it for TPoint }
  TPointHelper2 = record helper(TPointHelper) for TPoint
    function Norm1: longint;
  end;

class function TGuidHelper.Empty: TGUID;
var
  i: longint;
begin
  Result.Data1 := 0;
  Result.Data2 := 0;
  Result.Data3 := 0;
  for i := 0 to 7 do
    Result.Data4[i] := 0;
end;

class function TGuidHelper.Create(d1: longword; d2, d3: word; b: byte): TGUID;
var
  i: longint;
begin
  Result.Data1 := d1;
  Result.Data2 := d2;
  Result.Data3 := d3;
  for i := 0 to 7 do
    Result.Data4[i] := b + i;
end;

class function TGuidHelper.Create(d1: longword): TGUID;
begin
  Result := Create(d1, $1234, $4abc, $10);
end;

function TGuidHelper.IsEmpty: boolean;
var
  i: longint;
begin
  IsEmpty := (Data1 = 0) and (Data2 = 0) and (Data3 = 0);
  for i := 0 to 7 do
    if Data4[i] <> 0 then
      IsEmpty := false;
end;

function TGuidHelper.GetVersion: longint;
begin
  GetVersion := Data3 shr 12;
end;

function TGuidHelper.ToString(SkipBrackets: boolean): string;
var
  i: longint;
begin
  Result := IntToHex(Data1, 8) + '-' + IntToHex(Data2, 4) + '-' +
    IntToHex(Self.Data3, 4) + '-' + IntToHex(Data4[0], 2) +
    IntToHex(Data4[1], 2) + '-';
  for i := 2 to 7 do
    Result := Result + IntToHex(Data4[i], 2);
  if not SkipBrackets then
    Result := Braces[1] + Result + Braces[2];
end;

procedure TGuidHelper.Clear;
begin
  Self := TGUID.Empty;
end;

procedure TGuidHelper.Bump(by: longint);
begin
  Data1 := Data1 + by;
  Inc(Self.Data4[7]);
end;

function TPoint.Sum: longint;
begin
  Sum := x + y;
end;

function TPoint.Describe: string;
begin
  Describe := 'record';
end;

function TPointHelper.Describe: string;
begin
  Describe := '(' + IntToStr(x) + ', ' + IntToStr(y) + ') sum ' +
    IntToStr(Sum);
end;

function TPointHelper.Twice: TPoint;
begin
  Result.x := 2 * x;
  Result.y := 2 * y;
end;

procedure TPointHelper.Swap;
var
  t: longint;
begin
  t := x;
  x := y;
  y := t;
end;

function TPointHelper.Product: longint;
begin
  Product := x * y;
end;

function TPointHelper2.Norm1: longint;
begin
  Norm1 := abs(x) + abs(y);
end;

function MakePoint(x, y: longint): TPoint;
begin
  Result.x := x;
  Result.y := y;
end;

var
  g, h: TGUID;
  pg: PGUID;
  p: TPoint;
  ps: array[1..2] of TPoint;
begin
  g := TGUID.Create($DEADBEEF, $0102, $4304, $A0);
  writeln(g.ToString);
  writeln(g.ToString(true));
  writeln(g.IsEmpty, ' ', g.Version);
  g.Bump(1);
  writeln(g.ToString);
  h := g;
  g.Clear;
  writeln(g.IsEmpty, ' ', h.IsEmpty);
  writeln(TGUID.Empty.ToString);
  writeln(TGUID.Create(7).ToString(true), ' ', TGUID.Create(7).Version);
  writeln(TGUID.Braces, ' ', length(TGUID.Braces));
  { the predeclared TGUID: 16 bytes, its variants overlaid }
  writeln(SizeOf(TGUID), ' ', SizeOf(h.node), ' ', h.D1 = h.Data1, ' ',
    h.time_low = h.Data1, ' ', h.time_hi_and_version = h.D3, ' ',
    h.clock_seq_hi_and_reserved, ' ', h.clock_seq_low, ' ', h.node[5]);
  pg := @h;
  pg^.D4[0] := $FF;
  writeln(pg^.ToString, ' ', h.clock_seq_hi_and_reserved);

  p.x := 3;
  p.y := -4;
  writeln(p.Describe);
  p.Swap;
  writeln(p.Describe, ' ', p.Product, ' ', p.Norm1);
  writeln(p.Twice.Describe, ' ', p.Twice.Twice.Norm1);
  writeln(MakePoint(5, 6).Product, ' ', MakePoint(1, 2).Sum);
  ps[2] := MakePoint(9, 8);
  ps[2].Swap;
  with ps[2] do
    writeln(Describe, ' ', Product);
end.
