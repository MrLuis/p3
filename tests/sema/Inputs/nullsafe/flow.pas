{$H+}
program flow;
{$nullsafe on}
type
  TCustomer = class
    Name: string;
    Next: TCustomer?;
    Boss: TCustomer;
    constructor Create(const AName: string; ABoss: TCustomer);
    constructor CreateLazy(const AName: string);
  end;

constructor TCustomer.Create(const AName: string; ABoss: TCustomer);
begin
  Name := AName;
  Boss := ABoss;
end;

constructor TCustomer.CreateLazy(const AName: string);
begin
  Name := AName;
end;

function Find(const N: string): TCustomer?;
begin
  Result := nil;
end;

function Make: TCustomer;
begin
end;

function Pick(c: TCustomer?): TCustomer;
begin
  if c = nil then
    exit;
  Result := c;
end;

procedure Use(c: TCustomer);
begin
  Writeln(c.Name);
end;

procedure Test;
var
  A: TCustomer;
  B: TCustomer?;
  C: TCustomer;
begin
  B := Find('x');
  A := B;                        // error: B may be nil
  if B <> nil then A := B;       // ok
  Writeln(B?.Name ?? 'unknown');
  Writeln(B.Name);               // error
  Use(B);                        // error
  if Assigned(B) and (B.Name <> '') then Use(B);
  if B = nil then
    B := Find('y');
  Writeln(C.Name);               // error: not assigned
  C := A;
  Writeln(C.Name);
  B := nil;
  Writeln(B.Name);               // error: B is nil
  while B <> nil do
  begin
    Writeln(B.Name);
    B := B.Next;
  end;
  B := Find('z');
  if B <> nil then
    while True do
    begin
      Writeln(B.Name);           // error: set nil later in the loop
      B := B.Next;
    end;
  with B do Writeln(Name);       // error
end;

begin
  Test;
end.
