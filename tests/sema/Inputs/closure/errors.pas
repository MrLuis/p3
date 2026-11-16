{$H+}
{$modeswitch advancedrecords}
program errors;
type
  TFn = reference to function(x: Integer): Integer;
  TPlain = function(x: Integer): Integer;
  TProc = reference to procedure;
  TRec = record
    a: Integer;
    function Get: TFn;
  end;

function TRec.Get: TFn;
begin
  Result := function(x) => x + a;
end;

function Id(x: Integer): Integer;
begin
  Id := x;
end;

function ByVar(var v: Integer; out o: Integer): TProc;
begin
  o := 0;
  Result := procedure => v := o;
end;

function ByConst(const s: string; constref n: Integer;
                 const items: array of Integer): TProc;
begin
  Result := procedure => Writeln(s, n, Length(items));
end;

function Total: Integer;
var
  p: TProc;
begin
  p := procedure => Total := 1;
  p();
end;

function Nested(n: Integer): TFn;

  function Helper(x: Integer): Integer;
  begin
    Helper := x + n;
  end;

begin
  Result := function(x) => Helper(x);
  Result := @Helper;
end;

procedure Mix;
var
  r: TFn;
  p: TPlain;
  k: Integer;
begin
  k := 1;
  p := @Id;
  r := p;
  p := r;
  r := @Id;
  p := function(x) => x + k;
  r := p;
end;

procedure WithOutside;
var
  rec: TRec;
  p: TPlain;
begin
  with rec do
    p := function(x) => x + a;
end;

var
  t: reference to procedure(a: Integer);
begin
  t := procedure(a) => Writeln(a);
end.
