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

  { A value read and changed by many tasks, one at a time: each method
    holds a lock (Increment, Decrement and Add need a numeric T). }
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

{ The runtime's primitives (async.c). }
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
  pas_mutex_lock(FMutex);
  Result := FValue;
  pas_mutex_unlock(FMutex);
end;

procedure TAtomic.Store(const Value: T);
begin
  pas_mutex_lock(FMutex);
  FValue := Value;
  pas_mutex_unlock(FMutex);
end;

function TAtomic.Exchange(const Value: T): T;
begin
  pas_mutex_lock(FMutex);
  Result := FValue;
  FValue := Value;
  pas_mutex_unlock(FMutex);
end;

function TAtomic.CompareExchange(const Expected, Value: T): Boolean;
begin
  pas_mutex_lock(FMutex);
  Result := FValue = Expected;
  if Result then
    FValue := Value;
  pas_mutex_unlock(FMutex);
end;

function TAtomic.Add(const Delta: T): T;
begin
  pas_mutex_lock(FMutex);
  FValue := FValue + Delta;
  Result := FValue;
  pas_mutex_unlock(FMutex);
end;

function TAtomic.Increment: T;
begin
  pas_mutex_lock(FMutex);
  FValue := FValue + 1;
  Result := FValue;
  pas_mutex_unlock(FMutex);
end;

function TAtomic.Decrement: T;
begin
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
