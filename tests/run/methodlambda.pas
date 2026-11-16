{ Method-pointer lambdas (objfpc): a lambda wanted as an `of object` type
  is called on an object: in a method of a class, the Self of that method
  (it sees the object's members, as the method does); outside methods,
  nil. It uses nothing of the routines around it, so like @Self.M it can
  be stored anywhere: event handlers.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/methodlambda.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/methodlambda.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/methodlambda.expected
}
{$H+}
program methodlambda;

type
  TNotify = procedure(Sender: TObject) of object;
  TCalc = function(x: Integer): Integer of object;
  TLog = procedure(const s: string) of object;
  TPlain = procedure(const s: string);
  TMaker = reference to function: TCalc;

  TButton = class
    Caption: string;
    OnClick: TNotify;
    procedure Click;
  end;

  TForm = class
    Clicks: Integer;
    Name: string;
    Ok: TButton;
    Scale: TCalc;
    constructor Create;
    destructor Destroy; override;
    function Factor: Integer;
    procedure Each(const items: array of Integer; f: TCalc);
  end;

  TCounter = class
    Total: Integer;
    Log: TLog;
    procedure Run(const names: array of string);
    function Twice: TCalc;
    function Maker: TMaker;
  end;

  generic TBox<T> = class
    Value: T;
    Show: TLog;
    procedure Init;
  end;

procedure TButton.Click;
begin
  if Assigned(OnClick) then
    OnClick(Self);
end;

constructor TForm.Create;
begin
  Name := 'main';
  Ok := TButton.Create;
  Ok.Caption := 'OK';
  Ok.OnClick := procedure(Sender) =>
    begin
      Clicks := Clicks + 1;
      Writeln(Name, ': ', (Sender as TButton).Caption, ' clicked ', Clicks);
    end;
  Scale := function(x) => x * Factor;
end;

destructor TForm.Destroy;
begin
  Ok.Free;
  inherited;
end;

function TForm.Factor: Integer;
begin
  Factor := Clicks + 10;
end;

procedure TForm.Each(const items: array of Integer; f: TCalc);
var
  i: Integer;
begin
  for i := 0 to High(items) do
    Write(f(items[i]), ' ');
  Writeln;
end;

procedure Each(const names: array of string; Visit: TPlain);
var
  i: Integer;
begin
  for i := 0 to High(names) do
    Visit(names[i]);
end;

{ made in a plain lambda: its object is the method's Self }
procedure TCounter.Run(const names: array of string);
begin
  Each(names, procedure(s) =>
    begin
      Log := procedure(t) =>
        begin
          Total := Total + Length(t);
          { a plain lambda in it: its parameter, its object }
          Each([t, t], procedure(u) => Total := Total + Length(u) - Length(t));
          Writeln('log ', t, ' ', Self.Total);
        end;
      Log(s);
    end);
end;

function TCounter.Twice: TCalc;
begin
  Twice := function(x) => 2 * x + Total;
end;

{ made in a closure, which keeps the method's frame (its Self) }
function TCounter.Maker: TMaker;
begin
  Maker := function: TCalc => function(x: Integer): Integer => x + Total;
end;

procedure TBox.Init;
begin
  Show := procedure(s) => Writeln(s, Value);
end;

var
  form: TForm;
  c: TCounter;
  b: specialize TBox<Integer>;
  g: TCalc;
  m: TMaker;
  n: Integer;
begin
  form := TForm.Create;
  form.Ok.Click;
  form.Ok.Click;
  Writeln(form.Scale(3));
  { outside methods: its object is nil }
  n := 5;
  form.Each([1, 2], function(x: Integer): Integer => x + n);
  form.Free;

  c := TCounter.Create;
  c.Run(['ab', 'cde']);
  Writeln(c.Total);
  g := c.Twice;
  Writeln(g(1));
  m := c.Maker;
  g := m();
  m := nil;
  Writeln(g(100));
  c.Free;

  b := specialize TBox<Integer>.Create;
  b.Value := 7;
  b.Init;
  b.Show('value ');
  b.Free;
end.
