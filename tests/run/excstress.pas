{
  RUN: %pcc --mode=objfpc %s -o %t && %t | diff - %S/excstress.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.o2 && %t.o2 | diff - %S/excstress.expected
  RUN: %pcc --mode=objfpc -O3 -fno-range-checks -fno-overflow-checks %s -o %t.o3 && %t.o3 | diff - %S/excstress.expected
  RUN: %pcc --mode=objfpc -O2 -fno-inline-runtime %s -o %t.ni && %t.ni | diff - %S/excstress.expected
}
{ Exceptions while managed values are alive: ansistrings and COM interfaces
  in variables, parameters, results and temporaries, in routines with and
  without try statements and in the main program. What an exception's
  landing reads must be what was stored before the raise, also when the
  runtime's functions are inlined (-O2). Every object is released exactly
  once (the counts at the end). Output is identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc) except for the last line: Free Pascal keeps the main
  program's last interface temporary until the program ends (destroyed 99). }
program excstress;
{$H+}
uses sysutils;

type
  ICounter = interface
    function Value: integer;
  end;
  TCounter = class(TInterfacedObject, ICounter)
    FValue: integer;
    constructor Create(v: integer);
    destructor Destroy; override;
    function Value: integer;
  end;
  EStop = class(Exception);

var
  created, destroyed: integer;

constructor TCounter.Create(v: integer);
begin
  FValue := v;
  created := created + 1;
end;

destructor TCounter.Destroy;
begin
  destroyed := destroyed + 1;
  inherited Destroy;
end;

function TCounter.Value: integer;
begin
  Value := FValue;
end;

function Make(v: integer): ICounter;
begin
  Make := TCounter.Create(v);
end;

function Num(n: integer): string;
begin
  Num := IntToStr(n);
end;

{ no try statement: strings and interfaces are released on the way out }
function Risky(n: integer; const tag: string): string;
var
  s: string;
  c: ICounter;
  i: integer;
begin
  s := tag;
  for i := 1 to n do
  begin
    c := Make(i);
    s := s + Num(c.Value) + ',';
    if Make(i * 2).Value = 8 then
      raise EStop.Create('stop at ' + Num(i) + ' with ' + s);
  end;
  Risky := s + 'done';
end;

{ a value parameter and a result alive when the callee raises }
function Pass(s: string; c: ICounter): string;
begin
  s := s + '/' + Num(c.Value);
  Pass := Risky(c.Value, s);
end;

{ try statements: the variables keep the values they had at the raise }
function Guarded(n: integer): string;
var
  s, last: string;
  c: ICounter;
  i, count: integer;
begin
  s := '';
  last := 'none';
  count := 0;
  for i := 1 to n do
    try
      c := Make(i);
      last := Num(i);
      s := s + Pass('p' + last, c) + ';';
      count := count + 1;
    except
      on E: EStop do
        s := s + '[' + E.Message + ' last=' + last + ' count=' + Num(count) + ']';
    end;
  Guarded := s;
end;

function Finally_(n: integer): string;
var s: string; c: ICounter;
begin
  s := 'f';
  try
    try
      c := Make(n);
      s := s + Risky(n, 'x');
    finally
      s := s + '|cleanup ' + Num(c.Value);
    end;
  except
    on E: Exception do
      s := s + '|' + E.Message;
  end;
  Finally_ := s;
end;

{ a routine that cannot raise, with managed temporaries, called in a loop }
function Plain(n: integer): string;
begin
  Plain := Num(n) + '-' + Num(n + 1);
end;

var
  i: integer;
  total: string;
  keep: ICounter;
begin
  writeln(Guarded(6));
  writeln(Finally_(3));
  writeln(Finally_(5));
  { temporaries of the main program, raised through and reused in a loop }
  total := '';
  for i := 1 to 6 do
    try
      keep := Make(100 + i);
      total := total + Plain(i) + ' ' + Risky(i, Num(keep.Value) + ':') + ' ';
    except
      on E: EStop do
        total := total + '<' + E.Message + '> ';
    end;
  writeln(total);
  writeln(length(total));
  keep := nil;
  writeln('created ', created, ' destroyed ', destroyed);
end.
