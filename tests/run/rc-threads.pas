{ Reference counts once threads have started (optimization3.md item 6):
  until the runtime starts its first thread, counts change with plain
  operations; from then on atomically. An ansistring and a dynamic array
  made before the thread pool starts are then copied and dropped by eight
  pool tasks and by the main thread at once: a lost update would free
  them early (wrong lengths, a crash) or never.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rc-threads.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rc-threads.expected
}
{$H+}
program rcthreads;

uses SysUtils, threading;

var
  text: string;
  nums: array of longint;

{ Copies and drops both values n times. }
function Churn(n: longint): int64;
var
  i: longint;
  a, b: string;
  d: array of longint;
begin
  Result := 0;
  for i := 1 to n do
  begin
    a := text;
    b := a;
    d := nums;
    Result := Result + length(b) + d[i mod 4];
    a := '';
    d := nil;
  end;
end;

function ChurnTask(n: longint): async int64;
begin
  Result := Churn(n);
end;

var
  ts: array of async int64;
  r: array of int64;
  i: longint;
  total: int64;
begin
  { made while only the main thread runs: plain counts }
  text := 'shared ' + IntToStr(42);
  SetLength(nums, 4);
  for i := 0 to 3 do
    nums[i] := i + 1;
  SetLength(ts, 8);
  for i := 0 to 7 do
    ts[i] := Pool.Run(ChurnTask(200000));
  total := Churn(200000); { the main thread at the same time }
  r := Wait(WhenAll(ts));
  for i := 0 to high(r) do
    total := total + r[i];
  Writeln(total);
  { still alive, with one reference each (ours) }
  Writeln(text, ' ', length(nums), ' ', nums[3]);
  text := '';
  nums := nil;
  Writeln('done');
end.
