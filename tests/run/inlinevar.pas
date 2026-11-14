{ Variables declared among the statements (objfpc, as in Delphi):
  var x: T;  var x: T := e;  var x := e (the type of e);  for var i := ...;
  for var x in ... . A variable is visible to the end of the statement
  sequence (or loop) it is declared in, may hide an outer one, and is set
  (to its value, or to zero) each time the declaration runs. An owned one
  is freed when its sequence is left, however it is left.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/inlinevar.expected
  RUN: %pcc --mode=objfpc -O2 -fno-range-checks %s -o %t.fast
  RUN: %t.fast | diff - %S/inlinevar.expected
}
{$H+}
program inlinevar;

type
  TColor = (red, green, blue);
  TPoint = record
    x, y: integer;
  end;
  TNode = class
    name: string;
    constructor Create(const n: string);
    destructor Destroy; override;
  end;
  ENope = class(Exception) end;

constructor TNode.Create(const n: string);
begin
  name := n;
  writeln('  create ', name);
end;

destructor TNode.Destroy;
begin
  writeln('  destroy ', name);
  inherited Destroy;
end;

function Origin: TPoint;
begin
  Result.x := 3;
  Result.y := 4;
end;

{ the types of values }
procedure Inferred;
var
  list: array of integer;
begin
  var i := 42;
  var big := 5000000000;
  var r := 2.5;
  var c := 'x';
  var s := 'hello';
  var b := i > 40;
  var col := blue;
  var pt := Origin;
  var st := [red, blue];
  var p := @i;
  var n := TNode.Create('inferred');
  setlength(list, 3);
  var copyOfList := list;
  copyOfList[1] := 7;
  s := s + ', world';
  writeln('  ', i, ' ', big, ' ', r:0:2, ' ', c, ' ', s, ' ', b, ' ', col, ' ',
          pt.x + pt.y, ' ', green in st, ' ', p = @i, ' ', n.name, ' ',
          list[1], ' ', sizeof(i), ' ', sizeof(big), ' ', length(s));
  n.Free;
end;

{ written types; several names; the value converted to the type }
procedure Typed;
begin
  var a, b: integer;
  var w: word := 65535;
  var x: real := 2;
  var s: string := 'c';
  var t: string[3] := 'abcdef';
  var q: int64 := a + 1;
  b := a + 10;
  writeln('  ', a, ' ', b, ' ', w, ' ', x:0:1, ' ', s, ' ', t, ' ', q);
end;

{ visible to the end of its sequence; an inner one hides an outer one; the
  value cannot use the variable it declares }
procedure Scopes;
var
  x: integer;
begin
  x := 1;
  begin
    var y := x + 1;
    var x := 'inner ' + chr(ord('0') + y);
    writeln('  ', x);
    begin
      var x := x + '!';
      writeln('  ', x);
    end;
    writeln('  ', x);
  end;
  writeln('  ', x);
  if x = 1 then
  begin
    var only := 10;
    x := x + only;
  end
  else
  begin
    var only := 'other';
    writeln('  ', only);
  end;
  repeat
    var once := x * 2;
    x := once;
  until x > 20;
  writeln('  ', x);
  case x of
    1: ;
  else
    var inElse := x - 22;
    writeln('  ', inElse);
  end;
  try
    var inTry := 5;
    raise ENope.Create('nope');
  except
    var inExcept := 6;
    writeln('  ', inExcept);
  end;
end;

{ set each time the declaration runs }
procedure EachTime;
var
  k: integer;
begin
  for k := 1 to 3 do
  begin
    var count: integer;
    var text: string;
    var acc := k * 10;
    count := count + k;
    text := text + 'x';
    acc := acc + count;
    writeln('  ', count, ' ', text, ' ', acc);
  end;
end;

{ loop variables }
procedure Loops;
var
  names: array[1..3] of string = ('a', 'bb', 'ccc');
  total: integer;
begin
  total := 0;
  for var i := 1 to 4 do
    total := total + i;
  for var i: byte := 3 downto 1 do
    total := total * 10 + i;
  writeln('  ', total);
  write(' ');
  for var s in names do
    write(' ', s, length(s));
  for var ch in 'xyz' do
    write(' ', ord(ch));
  for var col in TColor do
    write(' ', col);
  for var n: int64 in [3, 5] do
    write(' ', n * 1000000000000);
  for var e in [blue, red] do
    write(' ', ord(e));
  writeln;
  write(' ');
  for var i := 1 to 2 do
    for var j := i to 2 do
    begin
      var k := i * 10 + j;
      write(' ', k);
    end;
  writeln;
end;

{ owned: freed when the sequence is left, the last declared first }
procedure OwnedBlocks(fail: boolean);
begin
  var a: TNode owned := TNode.Create('a');
  var later: TNode owned;
  begin
    var b: TNode owned := TNode.Create('b');
    var c: TNode owned := TNode.Create('c');
    writeln('  in block');
  end;
  writeln('  after block');
  later := TNode.Create('later');
  for var i := 1 to 3 do
  begin
    var n: TNode owned := TNode.Create('loop');
    if i = 1 then
      continue;
    if i = 2 then
      break;
    writeln('  not reached');
  end;
  writeln('  after loop');
  if fail then
    raise ENope.Create('failed');
  writeln('  end');
end;

function Make(const name: string): TNode;
begin
  var n: TNode owned := TNode.Create(name);
  if name = '' then
    exit(nil);
  Result := move n;
end;

begin
  writeln('inferred');
  Inferred;
  writeln('typed');
  Typed;
  writeln('scopes');
  Scopes;
  writeln('each time');
  EachTime;
  writeln('loops');
  Loops;
  writeln('owned');
  OwnedBlocks(false);
  try
    OwnedBlocks(true);
  except
    on e: ENope do
      writeln('  caught ', e.Message);
  end;
  writeln('make');
  var made := Make('made');
  var none := Make('');
  writeln('  ', made.name, ' ', none = nil);
  made.Free;
  { in the program's statements too }
  var last: TNode owned := TNode.Create('main');
  for var i := 1 to 2 do
    write('  ', i);
  writeln;
  writeln('done');
end.
