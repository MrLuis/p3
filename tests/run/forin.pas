{ for ... in (not in --mode=iso), as in Free Pascal: the elements of arrays
  (static, open, dynamic) and strings, the members of a set in order, the
  values of an ordinal type, and the values of an enumerator
  (GetEnumerator, MoveNext, Current) of a class, record or interface. A
  dynamic array or ansistring is held during the loop; a static array or
  short string is read in place; a class enumerator is freed after the loop,
  also on break, exit and exceptions. Output is identical to Free Pascal
  3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/forin.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/forin.expected
  RUN: %p3 --mode=objfpc -O3 -fno-range-checks -fno-overflow-checks %s -o %t.O3
  RUN: %t.O3 | diff - %S/forin.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/forin.expected
}
program forin;
{$H+}
{$modeswitch advancedrecords}
uses sysutils;

type
  TColor = (red, green, blue, cyan);
  TSmall = 2..5;
  TColors = set of TColor;
  TRow = array[1..2] of longint;
  TNames = array of string;
  TPt = record
    x, y: longint;
    tag: string;
  end;

  { a class enumerator, freed after the loop }
  TCounter = class
    p, n: longint;
    destructor Destroy; override;
    function MoveNext: boolean;
    function GetCur: longint;
    property Current: longint read GetCur;
  end;
  TBag = class
    size: longint;
    constructor Create(n: longint);
    function GetEnumerator: TCounter;
  end;

  { a record enumerator over a record }
  TLetters = record
    p: longint;
    s: string;
    function MoveNext: boolean;
    function GetCur: char;
    property Current: char read GetCur;
  end;
  TWord = record
    text: string;
    function GetEnumerator: TLetters;
  end;

  { an interface whose GetEnumerator gives a class enumerator }
  IBag = interface
    function GetEnumerator: TCounter;
  end;
  TIBag = class(TInterfacedObject, IBag)
    function GetEnumerator: TCounter;
    destructor Destroy; override;
  end;

destructor TCounter.Destroy;
begin
  writeln('  (enumerator freed)');
  inherited;
end;

function TCounter.MoveNext: boolean;
begin
  inc(p);
  result := p <= n;
end;

function TCounter.GetCur: longint;
begin
  result := p * 10;
end;

constructor TBag.Create(n: longint);
begin
  size := n;
end;

function TBag.GetEnumerator: TCounter;
begin
  result := TCounter.Create;
  result.n := size;
end;

function TLetters.MoveNext: boolean;
begin
  inc(p);
  result := p <= length(s);
end;

function TLetters.GetCur: char;
begin
  result := upcase(s[p]);
end;

function TWord.GetEnumerator: TLetters;
begin
  result.p := 0;
  result.s := text;
end;

function TIBag.GetEnumerator: TCounter;
begin
  result := TCounter.Create;
  result.n := 2;
end;

destructor TIBag.Destroy;
begin
  writeln('  (bag released)');
  inherited;
end;

procedure openArr(const a: array of longint);
var
  x: longint;
begin
  for x in a do
    write(x, ' ');
  writeln('| ', length(a));
end;

procedure openNames(const a: array of string);
var
  s: string;
begin
  for s in a do
    write(s, '/');
  writeln;
end;

function Names: TNames;
begin
  setlength(result, 3);
  result[0] := 'ann';
  result[1] := 'bob';
  result[2] := 'cy';
end;

{ exit from inside the loop: the enumerator is freed first }
function FirstOver(bag: TBag; limit: longint): longint;
var
  i: longint;
begin
  for i in bag do
    if i > limit then
      exit(i);
  result := -1;
end;

var
  sa: array[1..3] of longint = (1, 2, 3);
  da: array of longint;
  pts: array of TPt;
  pt: TPt;
  m: array[1..2] of TRow = ((1, 2), (3, 4));
  row: TRow;
  s: string;
  ss: shortstring;
  c: char;
  i: longint;
  i64: int64;
  d: double;
  b: byte;
  bo: boolean;
  col: TColor;
  cs: TColors;
  sm: TSmall;
  bag: TBag;
  w: TWord;
  ib: IBag;

begin
  writeln('arrays');
  for i in sa do
    write(i, ' ');
  writeln;
  setlength(da, 3);
  da[0] := 7;
  da[1] := 8;
  da[2] := 9;
  for i in da do
    write(i, ' ');
  writeln;
  for i64 in sa do
    write(i64 * 1000000000, ' ');
  writeln;
  for d in da do
    write(d:0:1, ' ');
  writeln;
  openArr([4, 5, 6]);
  openArr([]);
  openNames(['x', 'yy', 'zzz']);
  for s in Names do
    write(s, ' ');
  writeln;
  for row in m do
    write(row[1], row[2], ' ');
  writeln;
  setlength(pts, 2);
  pts[0].x := 1;
  pts[0].tag := 'first';
  pts[1].x := 2;
  pts[1].tag := 'second';
  for pt in pts do
    write(pt.x, ':', pt.tag, ' ');
  writeln;
  da := nil;
  for i in da do
    write('never');
  writeln('empty array');

  writeln('strings');
  s := 'xyz';
  for c in s do
    write(c, '.');
  writeln;
  ss := 'short';
  for c in ss do
    write(c);
  writeln;
  for c in 'lit' do
    write(c);
  writeln;
  s := '';
  for c in s do
    write('never');
  writeln('empty string');

  writeln('sets and types');
  cs := [blue, red];
  for col in cs do
    write(ord(col));
  writeln;
  for col in [green, cyan] do
    write(ord(col));
  writeln;
  for col in TColor do
    write(ord(col));
  writeln;
  for sm in TSmall do
    write(sm);
  writeln;
  for i in TSmall do
    write(i);
  writeln;
  for b in [1, 5, 200] do
    write(b, ' ');
  writeln;
  for bo in boolean do
    write(bo, ' ');
  writeln;
  for c in ['a'..'d', 'x'] do
    write(c);
  writeln;

  writeln('enumerators');
  bag := TBag.Create(3);
  for i in bag do
    write(i, ' ');
  writeln('done');
  for i in bag do
  begin
    if i = 20 then
      continue;
    write(i, ' ');
  end;
  writeln('done');
  w.text := 'word';
  for c in w do
    write(c);
  writeln;
  writeln(FirstOver(bag, 15));
  ib := TIBag.Create;
  for i in ib do
    write(i, ' ');
  writeln('done');
  ib := nil;

  writeln('changes in the loop');
  setlength(da, 3);
  da[0] := 7;
  da[1] := 8;
  da[2] := 9;
  for i in da do
  begin
    setlength(da, 1);
    write(i, ' ');
  end;
  writeln(length(da));
  s := 'abc';
  for c in s do
  begin
    s := 'z';
    write(c);
  end;
  writeln(s);
  sa[1] := 1;
  for i in sa do
  begin
    sa[3] := 99;
    write(i, ' ');
  end;
  writeln;
  cs := [red, blue];
  for col in cs do
  begin
    cs := [green];
    write(ord(col));
  end;
  writeln;

  writeln('break and exceptions');
  for i in bag do
  begin
    write(i);
    break;
  end;
  writeln;
  for c in 'abcdef' do
  begin
    if c = 'd' then
      break;
    write(c);
  end;
  writeln;
  try
    for i in bag do
      raise Exception.Create('boom');
  except
    on e: Exception do
      writeln('caught ', e.Message);
  end;
  for i in sa do
    for c in 'ab' do
      write(i, c, ' ');
  writeln;
  writeln('control variable after the loop: ', i);
  bag.Free;
end.
