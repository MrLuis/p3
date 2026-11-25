{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
program d07_tables;
{ Dialect check R2 (procedural types), R3 (exit with a value, case else),
  R4 ($I include), R5 (low/high), R6 (typed-constant arrays of records). }
{$I d07_tables.inc}
type
  TVisit = procedure(k: TTokKind);
  TMap = function(x: longint): longint;
  TPass = record
    name: string[10];
    run: TMap;
  end;

function lookup(const s: string): TTokKind;
var
  i: longint;
begin
  for i := low(keywords) to high(keywords) do
    if keywords[i].spelling = s then
      exit(keywords[i].kind);
  lookup := tkIdent;
end;

procedure show(k: TTokKind);
begin
  write(kindNames[k], ' ');
end;

function twice(x: longint): longint;
begin
  twice := 2 * x;
end;

function square(x: longint): longint;
begin
  square := x * x;
end;

var
  visit: TVisit;
  passes: array[1..2] of TPass;
  i, v: longint;
  k: TTokKind;
begin
  visit := @show;
  visit(lookup('end'));
  visit(lookup('foo'));
  writeln;
  for k := low(TTokKind) to high(TTokKind) do
    case k of
      tkIdent: write('I');
      tkBegin..tkEnd: write('B');
    else
      write('?');
    end;
  writeln;
  passes[1].name := 'twice';
  passes[1].run := @twice;
  passes[2].name := 'square';
  passes[2].run := @square;
  v := 3;
  for i := low(passes) to high(passes) do
  begin
    v := passes[i].run(v);
    writeln(passes[i].name, ' -> ', v);
  end;
  visit := nil;
  writeln(assigned(visit), ' ', assigned(passes[1].run));
end.
