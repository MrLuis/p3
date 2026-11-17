{ async/await (objfpc): async routines return tasks, which run on their
  own stacks; await waits for one in an async routine, Wait elsewhere; an
  exception comes back to whoever waits. A task runs synchronously until it
  first waits. scope ... end waits for the tasks spawned in it and, when one
  fails, cancels the others and raises the first failure; WhenAll and
  WhenAny wait for arrays of tasks; Pool.Run starts a task on the thread
  pool; detached lets one run on its own, its failure to a handler.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/async.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/async.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/async.expected
}
{$H+}
program async;

uses SysUtils, threading;

type
  TCounter = class
    Total: Integer;
    function Add(n: Integer): async Integer;
    class function Named(const s: string): async string;
  end;

  TPair = record
    A, B: Integer;
    S: string;
  end;

function TCounter.Add(n: Integer): async Integer;
begin
  Sleep(1);
  Total := Total + n;
  Result := Total;
end;

class function TCounter.Named(const s: string): async string;
begin
  Result := ClassName + '.' + s;
end;

function Twice(x: Integer): async Integer;
begin
  Sleep(10);
  Result := 2 * x;
end;

function Greet(const name: string): async string;
begin
  Sleep(5);
  Result := 'hello, ' + name;
end;

function MakePair(a: Integer; s: string): async TPair;
begin
  Result.A := a;
  Result.B := await Twice(a);
  Result.S := s + s;
end;

function Sum(n: Integer): async Integer;
var
  a, b: async Integer;
begin
  a := Twice(n);
  b := Twice(n + 1);
  Result := (await a) + (await b);
end;

procedure Fails: async;
begin
  Sleep(5);
  raise Exception.Create('task failed');
end;

function Both: async;
begin
  try
    await Fails;
  except
    on E: Exception do
      Writeln('caught in task: ', E.Message);
  end;
end;

{ runs here until its first wait }
procedure Eager: async;
begin
  Writeln('eager: before its first wait');
  Sleep(5);
  Writeln('eager: after');
end;

function Thrice(x: Integer): async Integer;
begin
  Result := 3 * x;
end;

function Square(x: Integer): async Integer;
begin
  Sleep(10 * (5 - x));
  Result := x * x;
end;

function Slow(ms: Integer): async Integer;
begin
  Sleep(ms);
  Result := ms;
end;

procedure Boom(ms: Integer): async;
begin
  Sleep(ms);
  raise Exception.Create('boom after ' + IntToStr(ms));
end;

procedure LogError(E: Exception);
begin
  Writeln('detached failed: ', E.Message);
end;

function All: async Integer;
var
  tasks: array of async Integer;
  i, total: Integer;
begin
  SetLength(tasks, 4);
  scope
    for i := 0 to 3 do
      tasks[i] := spawn Square(i + 1);
  end;
  total := 0;
  for i := 0 to 3 do
    total := total + await tasks[i];
  Result := total;
end;

function Shared: async Integer;
var
  count: Integer;
begin
  count := 0;
  scope
    spawn begin
      Sleep(5);
      count := count + 1;
    end;
    spawn begin
      Sleep(10);
      count := count + 10;
    end;
  end;
  Result := count;
end;

{ a failing child cancels its siblings; the scope raises its failure }
function Failing: async;
begin
  scope
    spawn Slow(5000);
    spawn Boom(20);
  end;
end;

{ a token cancelled from outside cancels the scope's tasks }
function Cancellable(k: TCancelToken): async Integer;
begin
  Result := 0;
  scope k
    spawn Slow(5000);
    spawn begin
      Sleep(20);
      k.Cancel;
    end;
  end;
  Result := 1;
end;

var
  t: async Integer;
  s: async string;
  e: async;
  ts: array[0..2] of async Integer;
  r: array of Integer;
  i: Integer;
  c: TCounter;
  p: TPair;
  k: TCancelToken;
  failures: Integer;
begin
  t := Twice(21);
  Writeln(Wait(t));
  s := Greet('world');
  Writeln(Wait(s));
  Writeln(Wait(Sum(10)));
  p := Wait(MakePair(4, 'ab'));
  Writeln(p.A, ' ', p.B, ' ', p.S);
  Wait(Both);
  try
    Wait(Fails);
  except
    on E: Exception do
      Writeln('caught in main: ', E.Message);
  end;
  t := nil;
  Writeln('main: starting Eager');
  e := Eager;
  Writeln('main: Eager is waiting');
  Wait(e);
  Writeln('main: Eager done');

  c := TCounter.Create;
  Writeln(Wait(c.Add(5)), ' ', Wait(c.Add(7)));
  Writeln(Wait(TCounter.Named('x')));
  c.Free;

  Writeln('all: ', Wait(All));
  Writeln('shared: ', Wait(Shared));
  try
    Wait(Failing);
  except
    on E: Exception do
      Writeln('scope: ', E.ClassName, ': ', E.Message);
  end;
  k := TCancelToken.Create;
  try
    Writeln(Wait(Cancellable(k)));
  except
    on E: EOperationCancelled do
      Writeln('cancelled: ', k.IsCancelled);
  end;

  ts[0] := Slow(30);
  ts[1] := Slow(10);
  ts[2] := Pool.Run(Slow(20));
  r := Wait(WhenAll(ts));
  for i := 0 to High(r) do
    Write(r[i], ' ');
  Writeln;
  ts[0] := Slow(300);
  ts[1] := Slow(5);
  ts[2] := Slow(400);
  Writeln('first: ', Wait(WhenAny(ts)));

  { many tasks on the pool at once }
  failures := 0;
  for i := 1 to 200 do
  begin
    t := Pool.Run(Thrice(i));
    if Wait(t) <> 3 * i then
      Inc(failures);
  end;
  Writeln('pool failures: ', failures);

  detached(LogError) Boom(10);
  detached(procedure(E: Exception) => Writeln('lambda handler: ', E.Message))
    Boom(30);
  Sleep(600);
  scope nil
  end;
  Writeln('done');
end.
