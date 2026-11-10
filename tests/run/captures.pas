{
  RUN: %pcc --mode=objfpc %s -o %t && %t | diff - %S/captures.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.o2 && %t.o2 | diff - %S/captures.expected
  RUN: %pcc --mode=objfpc -O2 -fzero-init -g %s -o %t.g && %t.g | diff - %S/captures.expected
}
{ Variables used by nested routines live in their routine's frame record,
  the others in allocas (registers when optimized). Every way a nested
  routine reaches an enclosing routine's variable, and variables that
  must keep their values when an exception arrives. Output is identical
  to Free Pascal 3.2.2's (fpc -Mobjfpc) except for the `fresh` line: Free
  Pascal leaves local variables undefined, pcc starts them from zero. }
program captures;
{$H+}
uses sysutils;

type
  TPoint = record x, y: integer; end;
  TBase = class
    FTotal: integer;
    procedure Add(n: integer); virtual;
    function Describe(const tag: string): string; virtual;
  end;
  TDerived = class(TBase)
    FName: string;
    procedure Add(n: integer); override;
    function Describe(const tag: string): string; override;
    function Sum(const a: array of integer): integer;
  end;

procedure TBase.Add(n: integer);
begin
  FTotal := FTotal + n;
end;

function TBase.Describe(const tag: string): string;
begin
  Result := tag + '=' + IntToStr(FTotal);
end;

{ a nested routine of a method: fields through the method's Self, an
  inherited call, the method's parameter }
procedure TDerived.Add(n: integer);
  procedure Twice;
  begin
    inherited Add(n);
    FTotal := FTotal + n;
  end;
begin
  Twice;
end;

function TDerived.Describe(const tag: string): string;
  function Inner: string;
  begin
    Inner := inherited Describe(tag) + '/' + FName;
  end;
begin
  Result := Inner;
end;

function TDerived.Sum(const a: array of integer): integer;
var i: integer;
  procedure Step;
  begin
    Result := Result + a[i] + FTotal;
  end;
begin
  Result := 0;
  for i := 0 to high(a) do
    Step;
end;

{ parameters (value, var, const), locals, the result by the function's
  name, three levels deep }
function Levels(a: integer; var b: integer; const c: string): integer;
var
  mine, shared: integer;
  p: TPoint;
  s: string;
  procedure L2(k: integer);
  var own: integer;
    procedure L3;
    begin
      shared := shared + a + k + own;
      b := b + 1;
      p.x := p.x + length(c);
      s := s + c;
      Levels := shared;
    end;
  begin
    own := k * 2;
    L3;
    with p do
      y := y + own;
  end;
begin
  shared := 0;
  p.x := 0;
  p.y := 0;
  s := '';
  for mine := 1 to 3 do
    L2(mine);
  writeln('levels: ', shared, ' ', b, ' ', p.x, ' ', p.y, ' ', s, ' ', mine);
end;

{ a recursive nested routine and a routine that only passes the link on }
function Fib(n: integer): integer;
var calls: integer;
  function Go(k: integer): integer;
    function Count: integer;
    begin
      calls := calls + 1;
      Count := calls;
    end;
  begin
    Count;
    if k < 2 then
      Go := k
    else
      Go := Go(k - 1) + Go(k - 2);
  end;
begin
  calls := 0;
  Fib := Go(n);
  writeln('fib calls: ', calls);
end;

{ no try statement here: the exception passes through, and the string
  locals are released; the counters are in registers }
function Risky(n: integer): string;
var i, acc: integer; s: string;
begin
  acc := 0;
  s := '';
  for i := 1 to n do
  begin
    acc := acc + i;
    s := s + IntToStr(i);
    if acc > 20 then
      raise Exception.Create('too much at ' + IntToStr(i));
  end;
  Risky := s + ':' + IntToStr(acc);
end;

{ a try statement: every variable keeps the value it had when the
  exception was raised }
procedure Guarded;
var i, acc, last: integer; s: string; p: TPoint;
  procedure Bump;
  begin
    acc := acc + 1;
  end;
begin
  acc := 0;
  last := 0;
  p.x := 0;
  s := '';
  try
    for i := 1 to 10 do
    begin
      last := i;
      p.x := p.x + i;
      s := s + 'x';
      Bump;
      if i = 4 then
        s := Risky(100);
    end;
  except
    on E: Exception do
      writeln('guarded: ', E.Message, ' last=', last, ' acc=', acc, ' p.x=', p.x,
              ' s=', s);
  end;
  try
    last := -1;
    try
      last := 7;
      raise Exception.Create('inner');
    finally
      writeln('finally: last=', last);
      last := last + 1;
    end;
  except
    writeln('after finally: last=', last);
  end;
end;

{ locals start from zero, strings empty }
procedure Fresh;
var n: integer; r: real; b: boolean; s: shortstring; a: ansistring; o: TObject;
begin
  writeln('fresh: ', n, ' ', r:0:1, ' ', b, ' [', s, '] [', a, '] ', o = nil);
end;

var
  d: TDerived;
  v: integer;
begin
  v := 10;
  writeln(Levels(5, v, 'ab'));
  writeln(Fib(10));
  writeln(Risky(5));
  Guarded;
  Fresh;
  d := TDerived.Create;
  d.FName := 'd';
  d.Add(4);
  writeln(d.Describe('total'));
  writeln(d.Sum([1, 2, 3]));
  d.Free;
end.
