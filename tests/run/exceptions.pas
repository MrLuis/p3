{ Exceptions (--mode=objfpc): raise, try..except with on-handlers and
  else, try..finally, re-raise, Exception (Message), exit / break /
  continue through finally parts and handlers, exceptions in constructors,
  across routines and methods, EDivByZero from integer div and mod. Output is identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc), which needs the `uses sysutils`.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/exceptions.expected %t.out
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/exceptions.expected %t.O2.out
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/exceptions.expected %t.g.out
}
program exceptions;

uses sysutils;

type
  { An exception class that reports its end. }
  ETraced = class(Exception)
    code: integer;
    constructor Create(const msg: string; c: integer);
    destructor Destroy; override;
  end;
  ENotFound = class(ETraced);
  EOther = class(Exception);

  { Not an Exception: any object can be raised. }
  TToken = class
    n: integer;
    constructor Create(an: integer);
    destructor Destroy; override;
  end;

  { Its constructor fails for negative sizes. }
  TBuffer = class
    size: integer;
    constructor Create(asize: integer);
    destructor Destroy; override;
  end;

  TAccount = class
  private
    FBalance: integer;
  public
    procedure Withdraw(amount: integer);
    property Balance: integer read FBalance;
  end;

constructor ETraced.Create(const msg: string; c: integer);
begin
  inherited Create(msg);
  code := c;
end;

destructor ETraced.Destroy;
begin
  writeln('  (', ClassName, ' ', code, ' destroyed)');
  inherited Destroy;
end;

constructor TToken.Create(an: integer);
begin
  n := an;
end;

destructor TToken.Destroy;
begin
  writeln('  (token ', n, ' destroyed)');
  inherited Destroy;
end;

constructor TBuffer.Create(asize: integer);
begin
  writeln('  TBuffer.Create(', asize, ')');
  if asize < 0 then
    raise ETraced.Create('negative size', asize);
  size := asize;
end;

destructor TBuffer.Destroy;
begin
  writeln('  TBuffer.Destroy (size ', size, ')');
  inherited Destroy;
end;

{ Free Pascal's IntToStr, for the messages below. }
function IntToStr(i: integer): string;
var
  s: string;
begin
  s := '';
  repeat
    s := chr(ord('0') + i mod 10) + s;
    i := i div 10;
  until i = 0;
  IntToStr := s;
end;

procedure TAccount.Withdraw(amount: integer);
begin
  if amount > FBalance then
    raise EOther.Create('insufficient funds: ' + IntToStr(FBalance));
  FBalance := FBalance - amount;
end;

var
  g: integer;
  log: string;

procedure Basics;
var
  e2: Exception;
begin
  writeln('basics');
  try
    raise Exception.Create('boom');
  except
    on E: Exception do
      writeln('  caught ', E.ClassName, ': ', E.Message);
  end;
  try
    raise ETraced.Create('traced', 1);
  except
    on E: EOther do writeln('  wrong handler');
    on E: ETraced do writeln('  caught ', E.ClassName, ' code ', E.code);
    on E: Exception do writeln('  too late');
  end;
  try
    raise ENotFound.Create('missing', 2);
  except
    on ETraced do writeln('  an ETraced, unnamed');
  end;
  try
    raise TToken.Create(3);
  except
    on T: TToken do writeln('  token ', T.n);
  end;
  try
    raise EOther.Create('x');
  except
    writeln('  bare except');
  end;
  try
    raise TToken.Create(4);
  except
    on E: Exception do writeln('  not an Exception');
  else
    writeln('  else part');
  end;
  try
    writeln('  nothing raised');
  except
    writeln('  not reached');
  end;
  e2 := Exception.Create('first');
  e2.Message := 'second';
  writeln('  message ', e2.Message);
  e2.Free;
end;

procedure Thrower(depth: integer);
begin
  if depth = 0 then
    raise ETraced.Create('deep', 10);
  try
    Thrower(depth - 1);
  finally
    writeln('  unwinding ', depth);
  end;
end;

procedure Propagation;
begin
  writeln('propagation');
  try
    try
      Thrower(3);
    except
      on E: EOther do writeln('  wrong');
    end;
    writeln('  not reached');
  except
    on E: ETraced do writeln('  outer got ', E.Message, ' ', E.code);
  end;
end;

procedure Reraise;
begin
  writeln('reraise');
  try
    try
      raise ETraced.Create('again', 20);
    except
      on E: ETraced do
      begin
        writeln('  inner ', E.code);
        raise;
      end;
    end;
  except
    on E: ETraced do writeln('  outer ', E.code);
  end;
  { a new exception in a handler: the handled one ends first }
  try
    try
      raise ETraced.Create('old', 21);
    except
      on E: ETraced do
      begin
        writeln('  replacing ', E.code);
        raise ETraced.Create('new', 22);
      end;
    end;
  except
    on E: ETraced do writeln('  got ', E.code);
  end;
  { re-raise from the else part }
  try
    try
      raise TToken.Create(23);
    except
      on E: Exception do writeln('  no');
    else
      begin
        writeln('  else re-raises');
        raise;
      end;
    end;
  except
    on T: TToken do writeln('  token ', T.n, ' again');
  end;
end;

procedure FinallyParts;
begin
  writeln('finally');
  try
    writeln('  body');
  finally
    writeln('  finally after normal end');
  end;
  try
    try
      writeln('  body raises');
      raise ETraced.Create('f', 30);
    finally
      writeln('  finally after raise');
    end;
  except
    on E: ETraced do writeln('  handled ', E.code);
  end;
  try
    try
      try
        raise ETraced.Create('f2', 31);
      finally
        writeln('  inner finally');
      end;
    finally
      writeln('  outer finally');
    end;
  except
    on E: ETraced do writeln('  handled ', E.code);
  end;
  { a raise in a finally part replaces the pending exception }
  try
    try
      raise TToken.Create(32);
    finally
      writeln('  finally raises');
      raise ETraced.Create('f3', 33);
    end;
  except
    on E: ETraced do writeln('  handled ', E.code);
  end;
end;

function ExitThroughFinally(n: integer): integer;
begin
  ExitThroughFinally := -1;
  try
    try
      if n = 1 then
        exit(100);
      if n = 2 then
        raise ETraced.Create('e', 40);
      ExitThroughFinally := 0;
    finally
      writeln('  inner finally, n = ', n);
    end;
  finally
    writeln('  outer finally, n = ', n);
  end;
  writeln('  after the try statements');
end;

function ExitFromHandler: integer;
begin
  try
    raise ETraced.Create('h', 41);
  except
    on E: ETraced do
    begin
      writeln('  handler exits');
      exit(E.code);
    end;
  end;
  ExitFromHandler := 0;
end;

procedure Jumps;
var
  i, j: integer;
begin
  writeln('jumps');
  writeln('  result ', ExitThroughFinally(0));
  writeln('  result ', ExitThroughFinally(1));
  try
    writeln('  result ', ExitThroughFinally(2));
  except
    on E: ETraced do writeln('  caught ', E.code);
  end;
  writeln('  result ', ExitFromHandler);
  for i := 1 to 4 do
    try
      if i = 2 then
        continue;
      if i = 4 then
        break;
      writeln('  loop ', i);
    finally
      writeln('  loop finally ', i);
    end;
  for i := 1 to 3 do
  begin
    try
      raise TToken.Create(i);
    except
      on T: TToken do
      begin
        if T.n = 2 then
          continue;
        if T.n = 3 then
          break;
        writeln('  handled token ', T.n);
      end;
    end;
    writeln('  after handler ', i);
  end;
  { a loop inside the protected statements: break stays inside }
  try
    for j := 1 to 5 do
      if j = 3 then
        break;
    writeln('  j = ', j);
  finally
    writeln('  finally after the loop');
  end;
  { break through two finally parts and a handler }
  i := 0;
  while true do
  begin
    i := i + 1;
    try
      try
        raise TToken.Create(50 + i);
      except
        on T: TToken do
          try
            if i = 2 then
              break;
          finally
            writeln('  innermost finally ', i);
          end;
      end;
    finally
      writeln('  outer finally ', i);
    end;
  end;
  writeln('  left the loop at ', i);
end;

procedure Variables;
var
  local: integer;

  procedure Bump;
  begin
    local := local + 1;
    g := g + 1;
    if local > 20 then
      raise ETraced.Create('bump', local);
  end;

begin
  writeln('variables');
  local := 0;
  g := 0;
  try
    Bump;
    Bump;
    local := local * 10;
    Bump;
    Bump;
  except
    on E: ETraced do
      writeln('  local ', local, ' g ', g, ' code ', E.code);
  end;
  local := 0;
  try
    local := 1;
    g := 41;
    log := 'set in try';
    raise TToken.Create(60);
  except
    on T: TToken do
    begin
      g := g + local;
      writeln('  ', log, ': local ', local, ' g ', g);
    end;
  end;
end;

procedure Constructors;
var
  b: TBuffer;
begin
  writeln('constructors');
  b := nil;
  try
    b := TBuffer.Create(-5);
    writeln('  not reached');
  except
    on E: ETraced do writeln('  failed: ', E.Message, ' ', E.code);
  end;
  writeln('  b assigned: ', b <> nil);
  b := TBuffer.Create(7);
  writeln('  size ', b.size);
  b.Free;
end;

procedure Methods;
var
  a: TAccount;
  k: integer;
begin
  writeln('methods');
  a := TAccount.Create;
  for k := 1 to 3 do
    try
      a.Withdraw(-10);
      a.Withdraw(15);
      writeln('  balance ', a.Balance);
    except
      on E: EOther do writeln('  ', E.Message);
    end;
  a.Free;
end;

{ Many protected statements in a row: frames are pushed and popped. }
procedure Stress;
var
  i, caught: integer;
begin
  writeln('stress');
  caught := 0;
  for i := 1 to 10000 do
    try
      try
        if i mod 3 = 0 then
          raise TObject.Create;
      finally
        g := i;
      end;
    except
      on TObject do
        caught := caught + 1;
    end;
  writeln('  caught ', caught, ', last ', g);
end;

procedure DivZero;
var
  a, b, k, failures: integer;
  big, zero: int64;
  q: cardinal;
begin
  writeln('division by zero');
  a := 7;
  b := 0;
  try
    writeln(a div b);
  except
    on E: EDivByZero do
      writeln('  ', E.ClassName, ': ', E.Message, ' ', E is EIntError, ' ',
              E is EExternal, ' ', E is Exception);
  end;
  try
    writeln(a mod b);
  except
    on E: EIntError do writeln('  mod: ', E.ClassName);
  end;
  big := 1;
  zero := 0;
  try
    writeln(big div zero);
  except
    on E: EDivByZero do writeln('  int64: ', E.Message);
  end;
  q := 5;
  try
    writeln(q div cardinal(b));
  except
    on E: Exception do writeln('  cardinal: ', E.ClassName);
  end;
  { an ordinary exception: re-raised, it reaches the outer handler }
  try
    try
      a := a div b;
    except
      on EDivByZero do
      begin
        writeln('  inner');
        raise;
      end;
    end;
  except
    on E: EDivByZero do writeln('  outer: ', E.Message);
  end;
  writeln('  a = ', a);
  failures := 0;
  for k := -2 to 2 do
    try
      try
        a := 12 div k;
        writeln('  12 div ', k, ' = ', a);
      finally
        b := k;
      end;
    except
      on EDivByZero do
        failures := failures + 1;
    end;
  writeln('  failures ', failures, ', b = ', b);
end;

procedure Silent(n: integer);
var
  t: TToken;
begin
  t := TToken.Create(n);
  try
    raise t;
  except
    on X: TToken do
      if X = t then
        writeln('  the same object');
  end;
end;

begin
  Basics;
  Propagation;
  Reraise;
  FinallyParts;
  Jumps;
  Variables;
  Constructors;
  Methods;
  Stress;
  DivZero;
  writeln('objects');
  Silent(70);
  writeln('done');
end.
