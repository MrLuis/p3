{ Open arrays: value, const and var parameters, static arrays of any bounds,
  array constructors with conversions, forwarding, nested routines, managed
  elements, methods, overloads. Output identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/openarrays.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/openarrays.expected %t.O2.out
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/openarrays.expected %t.g.out
}
program openarrays;
{$H+}
{$modeswitch advancedrecords}
uses sysutils;
type
  TSm = set of byte;
  TRec = record
    s: string;
    n: longint;
  end;
  T2 = array[1..2] of longint;
  PI = ^longint;
  TColor = (red, green, blue);
  TBox = class
    v: longint;
    function Sum(const a: array of longint): longint;
    procedure Fill(var a: array of longint); virtual;
    class function Hi(const a: array of string): longint;
    constructor Make(const a: array of longint);
  end;
  TKid = class(TBox)
    procedure Fill(var a: array of longint); override;
  end;
  TAdv = record
    n: longint;
    function Count(const a: array of double): longint;
  end;
function Sum(const a: array of longint): longint;
var i: longint;
begin
  Result := 0;
  for i := low(a) to high(a) do Result := Result + a[i];
end;
function SumD(const a: array of double): double;
var i: longint;
begin
  Result := 0;
  for i := 0 to high(a) do Result := Result + a[i];
end;
function Total(const a: array of longint): longint;
begin Total := Sum(a); end;
procedure Info(const a: array of longint);
begin writeln(low(a), ' ', high(a), ' ', length(a), ' ', sizeof(a)); end;
procedure Dbl(var a: array of longint);
var i: longint;
begin for i := 0 to high(a) do a[i] := a[i] * 2; end;
procedure ByVal(a: array of longint);
var i: longint;
begin
  for i := 0 to high(a) do a[i] := a[i] + 100;
  if length(a) > 0 then writeln(Sum(a), ' ', a[0]) else writeln('empty');
end;
procedure Forward2(const a: array of longint);
begin writeln(Sum(a)); Info(a); end;
procedure ValToVar(a: array of longint);
begin Dbl(a); writeln(a[0]); end;
procedure VarToVar(var a: array of longint);
begin Dbl(a); Info(a); end;
procedure Strs(const a: array of string);
var i: longint;
begin for i := 0 to high(a) do write('<', a[i], '>', length(a[i])); writeln; end;
procedure StrsVal(a: array of string);
var i: longint;
begin
  for i := 0 to high(a) do a[i] := a[i] + '!';
  Strs(a);
end;
procedure StrsVar(var a: array of string);
var i: longint;
begin for i := 0 to high(a) do a[i] := UpperCase(a[i]) + IntToStr(i); end;
procedure Shorts(const a: array of shortstring);
begin writeln(a[0], '|', a[1], '|', sizeof(a)); end;
procedure ShortsVal(a: array of shortstring);
begin a[0] := a[0] + 'x'; writeln(a[0]); end;
procedure Chars(const a: array of char);
var i: longint;
begin write(high(a), ':'); for i := 0 to high(a) do write(a[i]); writeln; end;
procedure Recs(const a: array of TRec);
var i: longint;
begin for i := 0 to high(a) do writeln(a[i].s, a[i].n); end;
procedure RecsVal(a: array of TRec);
begin a[0].s := a[0].s + '?'; a[0].n := 99; Recs(a); end;
procedure RecsVar(var a: array of TRec);
begin a[0].s := a[0].s + '&'; end;
procedure Nested(const a: array of T2);
begin writeln(high(a), ' ', a[1][2], ' ', a[1, 1]); end;
procedure Ptrs(const a: array of PI);
begin writeln(a[0] = nil, a[1]^); end;
procedure Objs(const a: array of TObject);
begin writeln(a[0].ClassName, ' ', a[1] = nil); end;
procedure Cols(const a: array of TColor);
begin writeln(ord(a[0]), ord(a[1])); end;
procedure Bools(const a: array of boolean);
begin writeln(a[0], a[1]); end;
procedure Bytes(const a: array of byte);
begin writeln(a[0] + a[1]); end;
procedure I64s(const a: array of int64);
begin writeln(a[0] + a[1]); end;
procedure Sets(const a: array of TSm);
begin writeln(3 in a[0], 3 in a[1]); end;
function First(const a: array of string): string;
begin if length(a) = 0 then First := 'none' else First := a[0]; end;
function Boom(a: array of string): string;
begin a[0] := a[0] + 'x'; raise Exception.Create(a[0]); Boom := ''; end;
procedure Nest(const a: array of longint);
  function Inner(i: longint): longint;
  begin Inner := a[i] * 10; end;
  procedure Inner2(b: array of longint);
  begin b[0] := Inner(1); writeln(b[0], a[0]); end;
begin writeln(Inner(0)); Inner2(a); end;
procedure Mv(var a: array of longint; const b: array of longint);
begin move(b[0], a[0], sizeof(b)); fillchar(a[1], sizeof(longint), 0); end;
function Over(const a: array of longint): string; overload;
begin Over := 'ints'; end;
function Over(const a: array of string): string; overload;
begin Over := 'strs'; end;
function Over(const a: array of const): string; overload;
begin Over := 'const'; end;
function Over2(const a: array of string): string; overload;
begin Over2 := 'strs'; end;
function Over2(s: TSm): string; overload;
begin Over2 := 'set'; end;
function Over3(const a: array of longint): string; overload;
begin Over3 := 'ints'; end;
function Over3(const a: array of double): string; overload;
begin Over3 := 'dbls'; end;
function Over3(x: longint): string; overload;
begin Over3 := 'one'; end;
function TBox.Sum(const a: array of longint): longint;
begin Sum := Total(a) + v; end;
procedure TBox.Fill(var a: array of longint);
begin a[0] := 1; end;
class function TBox.Hi(const a: array of string): longint;
begin Hi := high(a); end;
constructor TBox.Make(const a: array of longint);
begin v := length(a); end;
procedure TKid.Fill(var a: array of longint);
begin inherited Fill(a); a[1] := 2; end;
function TAdv.Count(const a: array of double): longint;
begin Count := length(a) + n; end;
var
  v3: array[0..2] of longint = (1, 2, 3);
  v5: array[-2..2] of longint = (1, 2, 3, 4, 5);
  e: array[1..1] of longint;
  m: array[0..1, 0..2] of longint;
  d: array[1..2] of double;
  sa: array[1..2] of string;
  ssa: array[0..1] of shortstring;
  ra: array[1..2] of TRec;
  ca: array[1..3] of char;
  pa: packed array[1..2] of char;
  t2a: array[0..1] of T2;
  n: longint = 7;
  r: double = 1.25;
  s: string = 'var';
  ss: shortstring = 'sh';
  c: char = 'c';
  x: longint;
  rc: TRec;
  o: TBox;
  ad: TAdv;
begin
  writeln(Sum(v3), Sum(v5), Sum([]), Sum([1]), Sum([1, 2, n]), Sum([n * 2, n + 1, Sum(v3)]));
  Info(v3); Info(v5); Info([]); Info([1, 2]);
  e[1] := 4; Info(e); writeln(Sum(e));
  m[1, 2] := 9; Info(m[1]); writeln(Sum(m[1]));
  Dbl(v3); writeln(v3[0], v3[2]); Dbl(v5); writeln(v5[-2], v5[2]);
  ByVal(v3); writeln(v3[0]);
  ByVal([1, 2]); ByVal([]) ;
  Forward2(v3); ValToVar(v3); writeln(v3[0]); VarToVar(v3); writeln(v3[0]);
  writeln(SumD([1, n, 2.5, r]):0:2); d[1] := 0.5; d[2] := 0.25; writeln(SumD(d):0:2);
  sa[1] := 'one'; sa[2] := 'two' + s;
  Strs(sa); StrsVal(sa); Strs(sa); StrsVar(sa); Strs(sa);
  Strs([]); Strs(['a']); Strs([s, 'lit', c, 'c', ss, s + s, First(sa), IntToStr(n)]);
  StrsVal([s, s + 'x']); writeln(s);
  writeln(First([]), First(sa), First(['p', 'q']));
  ssa[0] := 'a0'; ssa[1] := 'a1'; Shorts(ssa); Shorts(['x', s]); Shorts([c, 'long' + s]); ShortsVal(ssa); writeln(ssa[0]);
  ca[1] := 'x'; ca[2] := 'y'; ca[3] := 'z'; Chars(ca); pa[1] := 'p'; pa[2] := 'q'; Chars(pa); Chars('hello'); Chars('h'); Chars([]); Chars(['u', c]);
  ra[1].s := 'r1'; ra[1].n := 1; ra[2].s := 'r2' + s; ra[2].n := 2;
  Recs(ra); RecsVal(ra); Recs(ra); RecsVar(ra); Recs(ra); rc.s := 'rc'; rc.n := 5; Recs([rc, ra[2]]); RecsVal([rc]); writeln(rc.s, rc.n);
  t2a[1][2] := 4; t2a[1][1] := 3; Nested(t2a);
  x := 5; Ptrs([nil, @x]);
  o := TBox.Create; Objs([o, nil]); writeln(o.Sum([1, 2]), TBox.Hi(['a', 'b']), o.Hi([])); o.Free;
  o := TKid.Make([1, 2, 3]); writeln(o.v, o.Sum(v3)); e[1] := 0; o.Fill(v3); writeln(v3[0], v3[1]); o.Free;
  ad.n := 2; writeln(ad.Count([1.5, 2]));
  Cols([blue, red]); Bools([true, 1 > 2]); Bytes([1, n]); I64s([1, n]); Sets([[1, 3], []]);
  try writeln(Boom([s])); except on ex: Exception do writeln(ex.Message); end;
  Nest(v3);
  Mv(v3, [7, 8, 9]); writeln(v3[0], v3[1], v3[2]);
  writeln(Over([1, 2]), Over(['a', s]), Over([1, 'a']), Over(v3), Over(sa));
  writeln(Over2([1, 2]), Over2(['a']));
  writeln(Over3([1, 2]), Over3([1.5]), Over3(5), Over3(d), Over3(v5));
  for x := 1 to 3 do Strs([IntToStr(x), s + IntToStr(x)]);
end.
