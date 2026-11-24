{ p3's Threading: what async code uses besides the language's own words
  (async, await, spawn, scope, detached, Wait, WhenAll, WhenAny,
  Pool.Run): the methods of TCancelToken; TChannel<T>, a typed queue
  between tasks; and TAtomic<T> and TLocked<T>, values tasks share (the
  thread-safe classes, derived from the predeclared TSharedObject, that
  `shared` variables hold). Needs --mode=objfpc. }
unit threading;

{$H+}
{$modeswitch typehelpers}

interface

{$IFNDEF P3_OBJFPC}
uses threading_needs_mode_objfpc;
{$ELSE}

type
  { Send to, or Receive from, a channel closed (and empty). }
  EChannelClosed = class(Exception);

  { TCancelToken (predeclared): a cancellation token. Tasks observe the
    token of the scope they run in (`scope Token ... end`); waiting in a
    cancelled one raises EOperationCancelled. }
  TCancelTokenHelper = type helper for TCancelToken
    { a new token, not cancelled }
    class function Create: TCancelToken; static;
    procedure Cancel;
    function IsCancelled: Boolean;
    { raises EOperationCancelled if cancelled }
    procedure ThrowIfCancelled;
    { a token cancelled with this one, and after Ms milliseconds }
    function WithTimeout(Ms: Integer): TCancelToken;
    { a token cancelled with this one (and when cancelled itself) }
    function NewChild: TCancelToken;
  end;

  { A typed queue between tasks: Send waits while a bounded channel is
    full (back-pressure), Receive while it is empty; Close tells receivers
    that no more values come (for await reads until then). }
  generic TChannel<T> = class(TSharedObject)
  private
    FMutex, FNotEmpty, FNotFull: Pointer;
    FItems: array of T;
    FHead, FCount, FCapacity: Integer;
    FClosed: Boolean;
    procedure Grow;
    function TakeLocked: T;
  public
    { Capacity 0: unbounded }
    constructor Create(Capacity: Integer = 0);
    destructor Destroy; override;
    function Send(const Value: T): async;
    function Receive: async T;
    { (value, True), or (a default value, False) once closed and empty }
    function TryReceive: async (T, Boolean);
    procedure Close;
    function Count: Integer;
  end;

  { A value read and changed by many tasks, one at a time: Integer and
    Int64 use atomic operations, any other type holds a lock for each
    method (Increment, Decrement and Add need a numeric T). }
  generic TAtomic<T> = class(TSharedObject)
  private
    FMutex: Pointer;
    FValue: T;
  public
    constructor Create(const Value: T);
    destructor Destroy; override;
    function Load: T;
    procedure Store(const Value: T);
    { the value before }
    function Exchange(const Value: T): T;
    { Value stored if the value was Expected; whether it was }
    function CompareExchange(const Expected, Value: T): Boolean;
    { the value after }
    function Add(const Delta: T): T;
    function Increment: T;
    function Decrement: T;
  end;

  { A value tasks share behind a lock: Use runs a procedure on it with the
    lock held (it must not wait: no await or Sleep in it). }
  generic TLocked<T> = class(TSharedObject)
  public type
    TUse = reference to procedure(var Value: T);
  private
    FMutex: Pointer;
    FValue: T;
  public
    constructor Create(const Value: T);
    destructor Destroy; override;
    procedure Use(const Action: TUse);
    { a copy of the value }
    function Load: T;
    procedure Store(const Value: T);
  end;

{ The size of each task's stack (and an iterator's), in bytes: 1 MB unless
  the environment's P3_TASK_STACK_SIZE (bytes, or with a k or m suffix, as
  in 64k) or SetTaskStackSize chose another; at least 16 KB, rounded up to
  whole pages, with a guard page below. A stack reserves that much address
  space while its task lives, but only the pages it touches use memory: a
  server holding one task per connection lowers it to hold more of them.
  Code that runs in a task (and what it calls) must fit: big local arrays,
  deep recursion and some C routines (name resolution) want room. }
function TaskStackSize: Int64;
{ Sets the stack size for every task and iterator — only before the first
  one starts (the stacks are made and reused at one size): True if it was
  set, False once it is too late (the size stays). }
function SetTaskStackSize(Bytes: Int64): Boolean;
{ Whether task stacks have a guard page below them (True by default;
  P3_TASK_STACK_GUARD=0 turns them off). Stacks are carved from shared
  slabs; a guard page splits a slab into two kernel mappings per stack, so
  a process holds at most about vm.max_map_count / 2 parked tasks (Linux:
  ~32k) — without guards a whole slab is one mapping. An overflow then
  is not stopped by a fault: an unused 16 KB gap lies below each stack, and
  a task that switches out with its stack pointer below its stack stops the
  program with an error. An overflow that returned before the task switched
  out goes unseen (harmless within the gap; a bigger one may damage the
  stack below). }
function TaskStackGuard: Boolean;
{ Guard pages on or off — only before the first task or iterator starts:
  True if it was set, False once it is too late. }
function SetTaskStackGuard(On: Boolean): Boolean;

{ The runtime's primitives (async.c). }
function pas_task_stack_guard: longint; cdecl; external name '__pas_task_stack_guard';
function pas_task_set_stack_guard(on: longint): longint; cdecl;
  external name '__pas_task_set_stack_guard';
function pas_task_stack_size: Int64; cdecl; external name '__pas_task_stack_size';
function pas_task_set_stack_size(bytes: Int64): longint; cdecl;
  external name '__pas_task_set_stack_size';
function pas_mutex_new: Pointer; cdecl; external name '__pas_mutex_new';
procedure pas_mutex_free(m: Pointer); cdecl; external name '__pas_mutex_free';
procedure pas_mutex_lock(m: Pointer); cdecl; external name '__pas_mutex_lock';
procedure pas_mutex_unlock(m: Pointer); cdecl; external name '__pas_mutex_unlock';
function pas_waitq_new: Pointer; cdecl; external name '__pas_waitq_new';
procedure pas_waitq_free(q: Pointer); cdecl; external name '__pas_waitq_free';
procedure pas_waitq_wait(q, m: Pointer); cdecl; external name '__pas_waitq_wait';
procedure pas_waitq_signal(q: Pointer); cdecl; external name '__pas_waitq_signal';
procedure pas_waitq_broadcast(q: Pointer); cdecl; external name '__pas_waitq_broadcast';
procedure pas_token_make(parent: TCancelToken; ms: longint; var dst: TCancelToken);
  cdecl; external name '__pas_token_make';
procedure pas_token_cancel(k: TCancelToken); cdecl; external name '__pas_token_cancel';
function pas_token_cancelled(k: TCancelToken): longint; cdecl;
  external name '__pas_token_cancelled';
procedure pas_token_check(k: TCancelToken); cdecl; external name '__pas_token_check';

{$ENDIF}

implementation

{$IFDEF P3_OBJFPC}

function TaskStackSize: Int64;
begin
  Result := pas_task_stack_size;
end;

function SetTaskStackSize(Bytes: Int64): Boolean;
begin
  Result := pas_task_set_stack_size(Bytes) <> 0;
end;

function TaskStackGuard: Boolean;
begin
  Result := pas_task_stack_guard <> 0;
end;

function SetTaskStackGuard(On: Boolean): Boolean;
begin
  Result := pas_task_set_stack_guard(Ord(On)) <> 0;
end;

class function TCancelTokenHelper.Create: TCancelToken;
begin
  Result := nil;
  pas_token_make(nil, -1, Result);
end;

procedure TCancelTokenHelper.Cancel;
begin
  pas_token_cancel(Self);
end;

function TCancelTokenHelper.IsCancelled: Boolean;
begin
  Result := pas_token_cancelled(Self) <> 0;
end;

procedure TCancelTokenHelper.ThrowIfCancelled;
begin
  pas_token_check(Self);
end;

function TCancelTokenHelper.WithTimeout(Ms: Integer): TCancelToken;
begin
  if Ms < 0 then
    Ms := 0;
  Result := nil;
  pas_token_make(Self, Ms, Result);
end;

function TCancelTokenHelper.NewChild: TCancelToken;
begin
  Result := nil;
  pas_token_make(Self, -1, Result);
end;

constructor TChannel.Create(Capacity: Integer);
begin
  FMutex := pas_mutex_new;
  FNotEmpty := pas_waitq_new;
  FNotFull := pas_waitq_new;
  if Capacity < 0 then
    Capacity := 0;
  FCapacity := Capacity;
  if Capacity > 0 then
    SetLength(FItems, Capacity)
  else
    SetLength(FItems, 16);
end;

destructor TChannel.Destroy;
begin
  pas_waitq_free(FNotEmpty);
  pas_waitq_free(FNotFull);
  pas_mutex_free(FMutex);
  inherited;
end;

{ (unbounded, full: twice the room, the values in order from 0) }
procedure TChannel.Grow;
var
  Items: array of T;
  I: Integer;
begin
  SetLength(Items, 2 * Length(FItems));
  for I := 0 to FCount - 1 do
    Items[I] := FItems[(FHead + I) mod Length(FItems)];
  FItems := Items;
  FHead := 0;
end;

function TChannel.TakeLocked: T;
var
  Empty: T;
begin
  Result := FItems[FHead];
  FItems[FHead] := Empty; { (no reference left behind) }
  FHead := (FHead + 1) mod Length(FItems);
  Dec(FCount);
  pas_waitq_signal(FNotFull);
end;

function TChannel.Send(const Value: T): async;
begin
  pas_mutex_lock(FMutex);
  try
    while (FCapacity > 0) and (FCount = FCapacity) and not FClosed do
      pas_waitq_wait(FNotFull, FMutex);
    if FClosed then
      raise EChannelClosed.Create('send on a closed channel');
    if FCount = Length(FItems) then
      Grow;
    FItems[(FHead + FCount) mod Length(FItems)] := Value;
    Inc(FCount);
    pas_waitq_signal(FNotEmpty);
  finally
    pas_mutex_unlock(FMutex);
  end;
end;

function TChannel.Receive: async T;
begin
  pas_mutex_lock(FMutex);
  try
    while (FCount = 0) and not FClosed do
      pas_waitq_wait(FNotEmpty, FMutex);
    if FCount = 0 then
      raise EChannelClosed.Create('receive from a closed channel');
    Result := TakeLocked;
  finally
    pas_mutex_unlock(FMutex);
  end;
end;

function TChannel.TryReceive: async (T, Boolean);
var
  Value: T;
  Got: Boolean;
begin
  Got := False;
  pas_mutex_lock(FMutex);
  try
    while (FCount = 0) and not FClosed do
      pas_waitq_wait(FNotEmpty, FMutex);
    if FCount > 0 then
    begin
      Value := TakeLocked;
      Got := True;
    end;
  finally
    pas_mutex_unlock(FMutex);
  end;
  Result := (Value, Got);
end;

{ Atomic operations for TAtomic<Integer> and TAtomic<Int64>: the overload
  whose type is the value's does it with the runtime's atomics and gives
  True; for any other type the untyped overload is chosen, which gives
  False, and TAtomic takes its lock (the same way as sequences' KeyHash). }
function atomic_load32(p: Pointer): LongInt; cdecl; external name '__pas_atomic_load32';
procedure atomic_store32(p: Pointer; v: LongInt); cdecl; external name '__pas_atomic_store32';
function atomic_xchg32(p: Pointer; v: LongInt): LongInt; cdecl; external name '__pas_atomic_xchg32';
function atomic_cas32(p: Pointer; expected, v: LongInt): LongInt; cdecl; external name '__pas_atomic_cas32';
function atomic_add32(p: Pointer; d: LongInt): LongInt; cdecl; external name '__pas_atomic_add32';
function atomic_load64(p: Pointer): Int64; cdecl; external name '__pas_atomic_load64';
procedure atomic_store64(p: Pointer; v: Int64); cdecl; external name '__pas_atomic_store64';
function atomic_xchg64(p: Pointer; v: Int64): Int64; cdecl; external name '__pas_atomic_xchg64';
function atomic_cas64(p: Pointer; expected, v: Int64): LongInt; cdecl; external name '__pas_atomic_cas64';
function atomic_add64(p: Pointer; d: Int64): Int64; cdecl; external name '__pas_atomic_add64';

function FastLoad(var X: Integer; var R: Integer): Boolean; overload;
begin
  R := atomic_load32(@X);
  Result := True;
end;

function FastLoad(var X: Int64; var R: Int64): Boolean; overload;
begin
  R := atomic_load64(@X);
  Result := True;
end;

function FastLoad(var X; var R): Boolean; overload;
begin
  Result := False;
end;

function FastStore(var X: Integer; V: Integer): Boolean; overload;
begin
  atomic_store32(@X, V);
  Result := True;
end;

function FastStore(var X: Int64; V: Int64): Boolean; overload;
begin
  atomic_store64(@X, V);
  Result := True;
end;

function FastStore(var X; const V): Boolean; overload;
begin
  Result := False;
end;

function FastExchange(var X: Integer; V: Integer; var R: Integer): Boolean; overload;
begin
  R := atomic_xchg32(@X, V);
  Result := True;
end;

function FastExchange(var X: Int64; V: Int64; var R: Int64): Boolean; overload;
begin
  R := atomic_xchg64(@X, V);
  Result := True;
end;

function FastExchange(var X; const V; var R): Boolean; overload;
begin
  Result := False;
end;

{ Done: whether it was handled; Swapped: whether the value was Expected }
function FastCompareExchange(var X: Integer; Expected, V: Integer; var Swapped: Boolean): Boolean; overload;
begin
  Swapped := atomic_cas32(@X, Expected, V) <> 0;
  Result := True;
end;

function FastCompareExchange(var X: Int64; Expected, V: Int64; var Swapped: Boolean): Boolean; overload;
begin
  Swapped := atomic_cas64(@X, Expected, V) <> 0;
  Result := True;
end;

function FastCompareExchange(var X; const Expected, V; var Swapped: Boolean): Boolean; overload;
begin
  Result := False;
end;

function FastAdd(var X: Integer; D: Integer; var R: Integer): Boolean; overload;
begin
  R := atomic_add32(@X, D);
  Result := True;
end;

function FastAdd(var X: Int64; D: Int64; var R: Int64): Boolean; overload;
begin
  R := atomic_add64(@X, D);
  Result := True;
end;

function FastAdd(var X; const D; var R): Boolean; overload;
begin
  Result := False;
end;

{ one more / one less }
function FastStep(var X: Integer; Up: Boolean; var R: Integer): Boolean; overload;
begin
  if Up then R := atomic_add32(@X, 1) else R := atomic_add32(@X, -1);
  Result := True;
end;

function FastStep(var X: Int64; Up: Boolean; var R: Int64): Boolean; overload;
begin
  if Up then R := atomic_add64(@X, 1) else R := atomic_add64(@X, -1);
  Result := True;
end;

function FastStep(var X; Up: Boolean; var R): Boolean; overload;
begin
  Result := False;
end;

constructor TAtomic.Create(const Value: T);
begin
  FMutex := pas_mutex_new;
  FValue := Value;
end;

destructor TAtomic.Destroy;
begin
  pas_mutex_free(FMutex);
  inherited;
end;

function TAtomic.Load: T;
begin
  if FastLoad(FValue, Result) then
    exit;
  pas_mutex_lock(FMutex);
  Result := FValue;
  pas_mutex_unlock(FMutex);
end;

procedure TAtomic.Store(const Value: T);
begin
  if FastStore(FValue, Value) then
    exit;
  pas_mutex_lock(FMutex);
  FValue := Value;
  pas_mutex_unlock(FMutex);
end;

function TAtomic.Exchange(const Value: T): T;
begin
  if FastExchange(FValue, Value, Result) then
    exit;
  pas_mutex_lock(FMutex);
  Result := FValue;
  FValue := Value;
  pas_mutex_unlock(FMutex);
end;

function TAtomic.CompareExchange(const Expected, Value: T): Boolean;
begin
  if FastCompareExchange(FValue, Expected, Value, Result) then
    exit;
  pas_mutex_lock(FMutex);
  Result := FValue = Expected;
  if Result then
    FValue := Value;
  pas_mutex_unlock(FMutex);
end;

function TAtomic.Add(const Delta: T): T;
begin
  if FastAdd(FValue, Delta, Result) then
    exit;
  pas_mutex_lock(FMutex);
  FValue := FValue + Delta;
  Result := FValue;
  pas_mutex_unlock(FMutex);
end;

function TAtomic.Increment: T;
begin
  if FastStep(FValue, True, Result) then
    exit;
  pas_mutex_lock(FMutex);
  FValue := FValue + 1;
  Result := FValue;
  pas_mutex_unlock(FMutex);
end;

function TAtomic.Decrement: T;
begin
  if FastStep(FValue, False, Result) then
    exit;
  pas_mutex_lock(FMutex);
  FValue := FValue - 1;
  Result := FValue;
  pas_mutex_unlock(FMutex);
end;

constructor TLocked.Create(const Value: T);
begin
  FMutex := pas_mutex_new;
  FValue := Value;
end;

destructor TLocked.Destroy;
begin
  pas_mutex_free(FMutex);
  inherited;
end;

procedure TLocked.Use(const Action: TUse);
begin
  pas_mutex_lock(FMutex);
  try
    Action(FValue);
  finally
    pas_mutex_unlock(FMutex);
  end;
end;

function TLocked.Load: T;
begin
  pas_mutex_lock(FMutex);
  Result := FValue;
  pas_mutex_unlock(FMutex);
end;

procedure TLocked.Store(const Value: T);
begin
  pas_mutex_lock(FMutex);
  FValue := Value;
  pas_mutex_unlock(FMutex);
end;

procedure TChannel.Close;
begin
  pas_mutex_lock(FMutex);
  FClosed := True;
  pas_waitq_broadcast(FNotEmpty);
  pas_waitq_broadcast(FNotFull);
  pas_mutex_unlock(FMutex);
end;

function TChannel.Count: Integer;
begin
  pas_mutex_lock(FMutex);
  Result := FCount;
  pas_mutex_unlock(FMutex);
end;

{$ENDIF}

end.
