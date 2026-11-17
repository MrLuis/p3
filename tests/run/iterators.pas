{ Iterators (objfpc; item 14 of the proposals): a routine whose result is
  iterator T yields its values one at a time (yield e, yield break, yield
  from it), its body running only when the next value is asked for (for-in,
  IteratorNext / IteratorValue). Stopping early runs its finally parts.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/iterators.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/iterators.expected
}
{$H+}
program iterators;
uses SysUtils;

function Fibonacci: iterator UInt64;
begin
  var a: UInt64 := 0;
  var b: UInt64 := 1;
  while True do
  begin
    yield a;
    (a, b) := (b, a + b);
  end;
end;

function Count(lo, hi: Integer): iterator Integer;
var i: Integer;
begin
  for i := lo to hi do
    yield i;
end;

function Words(const s: string): iterator string;
var w: string;
begin
  w := '';
  try
    for var c in s do
      if c = ' ' then
      begin
        if w <> '' then yield w;
        w := '';
      end
      else
        w := w + c;
    if w <> '' then yield w;
  finally
    Writeln('[words done]');
  end;
end;

function Both: iterator Integer;
begin
  yield 0;
  yield from Count(1, 3);
  yield break;
  yield 99;
end;

function Boom: iterator Integer;
begin
  yield 1;
  raise Exception.Create('boom');
end;


type
  TNode = class
    Name: string;
    Children: array of TNode;
    constructor Create(const AName: string; const AChildren: array of TNode);
    destructor Destroy; override;
    function Walk: iterator TNode;
  end;

constructor TNode.Create(const AName: string; const AChildren: array of TNode);
var i: Integer;
begin
  Name := AName;
  SetLength(Children, Length(AChildren));
  for i := 0 to High(AChildren) do Children[i] := AChildren[i];
end;

destructor TNode.Destroy;
var c: TNode;
begin
  for c in Children do c.Free;
  inherited;
end;

function TNode.Walk: iterator TNode;
begin
  yield Self;
  for var c in Children do
    yield from c.Walk;
end;

type TStrings = array of string;

function Pairs(names: TStrings): iterator (Index: Integer, Value: string);
var i: Integer;
begin
  for i := 0 to High(names) do
    yield (i, names[i]);
end;

function Squares(n: Integer): iterator Integer;
var i: Integer;
begin
  for i := 1 to n do yield i * i;
end;

{ an iterator that consumes another }
function Evens(it: iterator Integer): iterator Integer;
begin
  for var x in it do
    if x mod 2 = 0 then yield x;
end;

function Safe: iterator string;
begin
  try
    yield 'a';
    raise Exception.Create('inside');
  except
    on E: Exception do yield 'handled ' + E.Message;
  end;
  yield 'z';
end;

function Make(const prefix: string): iterator string;
begin
  yield prefix + '1';
  yield prefix + '2';
end;

type
  TPerson = record
    Name: string;
    Tags: array of string;
  end;

generic function Times<T>(x: T; n: Integer): iterator T;
var i: Integer;
begin
  for i := 1 to n do yield x;
end;

function People: iterator TPerson;
var p: TPerson;
begin
  p.Name := 'ann';
  SetLength(p.Tags, 1);
  p.Tags[0] := 'a';
  yield p;
  p.Name := 'bob' + p.Name;
  yield p;
end;

procedure Test3;
var q: TPerson;
begin
  for var s in specialize Times<string>('ab', 3) do Write(s, ' ');
  Writeln;
  for var k in specialize Times<Integer>(7, 2) do Write(k, ' ');
  Writeln;
  for q in People do Write(q.Name, ':', Length(q.Tags), ' ');
  Writeln;
end;

procedure Test;
var
  n: UInt64;
  k: Integer;
  it: iterator Integer;
begin
  k := 0;
  for n in Fibonacci do
  begin
    Write(n, ' ');
    Inc(k);
    if k = 10 then break;
  end;
  Writeln;
  for var i in Count(1, 5) do Write(i, ' ');
  Writeln;
  for var w in Words('the quick  brown fox') do
  begin
    Write(w, '|');
    if w = 'brown' then break;
  end;
  Writeln;
  for var i in Both do Write(i, ' ');
  Writeln;
  it := Count(10, 12);
  while IteratorNext(it) do Write(IteratorValue(it), ' ');
  Writeln(IteratorNext(it));
  try
    for var i in Boom do Writeln('got ', i);
  except
    on E: Exception do Writeln('caught ', E.Message);
  end;
end;

procedure Test2;
var
  root: TNode;
  n: TNode;
  keep: iterator string;
  never: iterator string;
begin
  root := TNode.Create('root', [TNode.Create('a', [TNode.Create('a1', []), TNode.Create('a2', [])]),
                                TNode.Create('b', [])]);
  for n in root.Walk do Write(n.Name, ' ');
  Writeln;
  root.Free;
  for var p in Pairs(['x', 'y', 'z']) do Write(p.Index, '=', p.Value, ' ');
  Writeln;
  for var e in Evens(Squares(10)) do Write(e, ' ');
  Writeln;
  for var s in Safe do Write(s, '; ');
  Writeln;
  keep := Make(Copy('kkkk', 1, 3));
  never := Make('never');
  never := nil;
  for var s in keep do Write(s, ' ');
  Writeln;
end;

begin
  Test;
  Test2;
  Test3;
end.
