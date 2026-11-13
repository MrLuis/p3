{ raise Object at Address[, Frame] (objfpc), as in Free Pascal: the
  exception's address (FPC's CodePointer) and frame are pointers; the
  frame, the address and the object are evaluated in that order (Free
  Pascal's). ExceptAddr, in a handler, is the address of the exception it
  handles (nil outside handlers); a re-raise keeps it. Output is identical
  to Free Pascal 3.2.2's (fpc -Mobjfpc, with SysUtils).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/raiseat.expected
}
program raiseat;
type
  TMarker = class end;
var
  slots: array[1..10] of Integer;
  m: TMarker;
  n: Integer;

function O(s: string): TObject;
begin
  writeln('object ', s);
  O := Exception.Create(s);
end;

function Addr(s: string; k: Integer): Pointer;
begin
  writeln('address ', s);
  Addr := @slots[k];
end;

function Which(p: Pointer): Integer;
var k: Integer;
begin
  Which := 0;
  for k := 1 to 10 do
    if p = Pointer(@slots[k]) then Which := k;
end;

{ raises E at the caller's address, as a helper of a library would }
procedure RaiseAt(E: TObject; Addr, Frame: Pointer);
begin
  raise E at Addr, Frame;
end;

procedure Fail(Code: Integer);
begin
  RaiseAt(Exception.Create('failed'), @slots[Code], nil);
end;

begin
  { the frame, then the address, then the object }
  try
    raise O('1') at Addr('a', 3), Addr('f', 4);
  except
    on E: Exception do writeln(E.Message, ' at ', Which(ExceptAddr));
  end;
  try
    raise O('2') at @slots[5];
  except
    writeln('2 at ', Which(ExceptAddr));
  end;
  { a re-raise keeps the address }
  try
    try
      raise O('3') at @slots[7];
    except
      raise;
    end;
  except
    on E: Exception do writeln(E.Message, ' at ', Which(ExceptAddr));
  end;
  { nested handlers: each sees its own exception's address }
  try
    raise O('4') at @slots[1];
  except
    try
      raise O('5') at @slots[2];
    except
      writeln('inner at ', Which(ExceptAddr));
    end;
    writeln('outer at ', Which(ExceptAddr));
  end;
  { through a helper procedure }
  try
    Fail(9);
  except
    on E: Exception do writeln(E.Message, ' at ', Which(ExceptAddr));
  end;
  { nil and an object as the address }
  try
    raise O('6') at nil;
  except
    writeln('6 at nil ', ExceptAddr = nil);
  end;
  m := TMarker.Create;
  try
    raise O('7') at m, m;
  except
    writeln('7 at m ', ExceptAddr = Pointer(m));
  end;
  m.Free;
  { without at: some address; outside a handler: nil }
  try
    raise O('8');
  except
    writeln('8 has an address ', ExceptAddr <> nil);
  end;
  try
    n := 0;
    n := 5 div n;
  except
    writeln('div has an address ', ExceptAddr <> nil);
  end;
  writeln('outside ', ExceptAddr = nil);
  { re-raising at the handled exception's address }
  try
    try
      raise O('9') at @slots[6];
    except
      on E: Exception do raise Exception.Create('wrapped ' + E.Message) at ExceptAddr;
    end;
  except
    on E: Exception do writeln(E.Message, ' at ', Which(ExceptAddr));
  end;
end.
