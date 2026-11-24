{ Task stack size (unit threading): 1 MB by default; SetTaskStackSize sets
  it before the first task or iterator (clamped to 16 KB, rounded up to
  whole pages), and returns False once one has started; P3_TASK_STACK_SIZE
  sets it from the environment (bytes, or with a k or m suffix), a value
  that is not a size warning and leaving the default. Tasks and an iterator
  run on the small stacks: recursion within them, results, awaits.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | FileCheck %s
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | FileCheck %s
  RUN: env P3_TASK_STACK_SIZE=32k %t env | FileCheck %s --check-prefix=ENV
  RUN: env P3_TASK_STACK_SIZE=2M %t env | FileCheck %s --check-prefix=ENVM
  RUN: env P3_TASK_STACK_SIZE=lots %t env 2>&1 | FileCheck %s --check-prefix=BAD
  RUN: env P3_TASK_STACK_GUARD=0 %t env | FileCheck %s --check-prefix=NOGUARD
  RUN: %t noguard | FileCheck %s --check-prefix=SLAB
  RUN: not %t overflow 2>&1 | FileCheck %s --check-prefix=OVF
  Guard pages off: a slab's stacks lie in one mapping, an unused gap below
  each, and a task that switches out while it overflows its stack (into
  the gap) is stopped.
  }
{$mode objfpc}{$H+}
program taskstack;
uses sysutils, threading;

{ CHECK: default: 1048576
  CHECK-NEXT: set 64k: TRUE 65536
  CHECK-NEXT: set 100: TRUE 16384
  CHECK-NEXT: set 70000: TRUE rounded=TRUE
  CHECK-NEXT: set 64k again: TRUE 65536
  CHECK-NEXT: tasks: 4 x 500 deep = 2000
  CHECK-NEXT: iterator: 15
  CHECK-NEXT: after start: FALSE 65536
  ENV: env: 32768 guard=TRUE
  ENVM: env: 2097152 guard=TRUE
  BAD: P3_TASK_STACK_SIZE is not a size
  BAD: env: 1048576
  NOGUARD: env: 1048576 guard=FALSE
  SLAB: guard: TRUE
  SLAB-NEXT: set guard off: TRUE FALSE
  SLAB-NEXT: tasks: 200 parked, 200 done
  SLAB-NEXT: after start: FALSE FALSE
  OVF: overflowed its stack
}

{ 500 frames of 32 bytes (64 on Windows, with its shadow space): well
  within 64 KB }
function Depth(n: Integer): Integer;
begin
  if n = 0 then
    Result := 0
  else
    Result := Depth(n - 1) + 1;
end;

function Work(n: Integer): async Integer;
begin
  Sleep(1);   { a park and a wake on this stack }
  Result := Depth(n);
end;

function Parked(ms: Integer): async Integer;
begin
  Sleep(ms);
  Result := 1;
end;

{ 72 KB of locals on a 64 KB stack: their bottom 8 KB land below the
  stack, in the gap above the stack below, and it switches out from there }
function Hog: async Integer;
var
  big: array[0..9215] of Int64;
  k: Integer;
begin
  for k := 0 to High(big) do
    big[k] := k;
  Sleep(1);   { switches out, its stack pointer below its stack }
  Result := big[7];
end;

function Count(hi: Integer): iterator Integer;
begin
  for var i := 1 to hi do
    yield i;
end;

var
  t: array[1..4] of async Integer;
  many: array[1..200] of async Integer;
  a, b: async Integer;
  total, i, s: Integer;
  ok: Boolean;
begin
  if ParamStr(1) = 'env' then
  begin
    Writeln('env: ', TaskStackSize, ' guard=', TaskStackGuard);
    exit;
  end;
  if ParamStr(1) = 'noguard' then
  begin
    Writeln('guard: ', TaskStackGuard);
    ok := SetTaskStackGuard(false);
    Writeln('set guard off: ', ok, ' ', TaskStackGuard);
    SetTaskStackSize(64 * 1024);
    for i := 1 to 200 do
      many[i] := Parked(20);   { 200 stacks parked at once, side by side }
    total := 0;
    for i := 1 to 200 do
      total := total + Wait(many[i]);
    Writeln('tasks: 200 parked, ', total, ' done');
    ok := SetTaskStackGuard(true);
    Writeln('after start: ', ok, ' ', TaskStackGuard);
    exit;
  end;
  if ParamStr(1) = 'overflow' then
  begin
    SetTaskStackGuard(false);
    SetTaskStackSize(64 * 1024);
    a := Parked(1000);   { the first stack of the slab, parked }
    b := Hog;            { the next one up: its overflow is caught }
    Writeln(Wait(b) + Wait(a));
    exit;
  end;
  Writeln('default: ', TaskStackSize);
  ok := SetTaskStackSize(64 * 1024);
  Writeln('set 64k: ', ok, ' ', TaskStackSize);
  ok := SetTaskStackSize(100);
  Writeln('set 100: ', ok, ' ', TaskStackSize);
  ok := SetTaskStackSize(70000);
  Writeln('set 70000: ', ok, ' rounded=',
          (TaskStackSize >= 70000) and (TaskStackSize mod 4096 = 0));
  ok := SetTaskStackSize(64 * 1024);
  Writeln('set 64k again: ', ok, ' ', TaskStackSize);
  for i := 1 to 4 do
    t[i] := Pool.Run(Work(500));
  total := 0;
  for i := 1 to 4 do
    total := total + Wait(t[i]);
  Writeln('tasks: 4 x 500 deep = ', total);
  s := 0;
  for var v in Count(5) do
    s := s + v;
  Writeln('iterator: ', s);
  ok := SetTaskStackSize(128 * 1024);
  Writeln('after start: ', ok, ' ', TaskStackSize);
end.
