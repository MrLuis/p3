{ Functions returning records that hold ansistrings: results by Result, name
  and exit(value), through methods, virtual methods, class methods,
  properties and operators; temporaries in loops, conditions and discarded
  calls; recursion, exceptions, self-assignment. Output identical to Free
  Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/ansistring-records.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/ansistring-records.expected %t.O2.out
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/ansistring-records.expected %t.g.out
}
program ansistringrecords;
{$H+}
{$modeswitch advancedrecords}
uses sysutils;
type
  TInner = record
    a: string;
    n: longint;
  end;
  TPerson = record
    name: string;
    age: longint;
    tags: array[1..2] of string;
    inner: TInner;
  end;
  TPlain = record
    x, y: longint;
  end;
  TBase = class
    FP: TPerson;
    function Get(const n: string): TPerson; virtual;
    function GetP: TPerson;
    procedure SetP(const v: TPerson);
    class function Make(const n: string): TPerson;
    property P: TPerson read GetP write SetP;
    property Raw: TPerson read FP write FP;
  end;
  TKid = class(TBase)
    function Get(const n: string): TPerson; override;
  end;
  TAdv = record
    s: string;
    class operator +(const a, b: TAdv): TAdv;
    class operator :=(const a: string): TAdv;
    function Twice: TAdv;
  end;
class operator TAdv.+(const a, b: TAdv): TAdv;
begin Result.s := a.s + b.s; end;
class operator TAdv.:=(const a: string): TAdv;
begin Result.s := '[' + a + ']'; end;
function TAdv.Twice: TAdv;
begin Result.s := s + s; end;
function Mk(const n: string; a: longint): TPerson;
begin
  Result.name := n + '!';
  Result.age := a;
  Result.tags[1] := n + '1';
  Result.tags[2] := n + '2';
  Result.inner.a := 'in' + n;
  Result.inner.n := a * 2;
end;
function ByName(const n: string): TPerson;
begin
  ByName := Mk('', 0);
  Result.name := n;
  Result.age := 1;
end;
function ViaExit(n: longint): TPerson;
var t: TPerson;
begin
  t := Mk('local', n);
  if n = 0 then exit(t);
  if n = 1 then exit(Mk('direct', n));
  t.name := t.name + '-end';
  ViaExit := t;
end;
function Chain(const p: TPerson): TPerson;
begin Result := p; Result.name := Result.name + '+c'; end;
function Recur(n: longint): TPerson;
begin
  if n = 0 then begin Result := Mk('', 0); Result.name := 'base'; end
  else begin Result := Recur(n - 1); Result.name := Result.name + IntToStr(n); Result.age := Result.age + 1; end;
end;
function Boom(const n: string): TPerson;
begin
  Result := Mk(n, 0);
  raise Exception.Create('boom ' + n);
end;
function Catch(const n: string): string;
var r: TPerson;
begin
  r.name := 'old';
  try
    r := Boom(n);
    Result := 'no';
  except
    on e: Exception do Result := '/' + e.Message;
  end;
end;
function Fin(const n: string): TPerson;
begin
  try
    Result := Mk(n, 1);
    exit;
  finally
    writeln('fin');
  end;
end;
function P2: TPlain; begin Result.x := 1; Result.y := 2; end;
function Long: TPerson;
var i: longint;
begin Result := Mk('', 0); Result.name := ''; for i := 1 to 400 do Result.name := Result.name + 'z'; end;
function TBase.Get(const n: string): TPerson; begin Result := Mk('base' + n, 1); end;
function TKid.Get(const n: string): TPerson; begin Result := inherited Get('kid' + n); Result.age := 9; end;
function TBase.GetP: TPerson; begin Result := FP; Result.name := Result.name + '@'; end;
procedure TBase.SetP(const v: TPerson); begin FP := v; end;
class function TBase.Make(const n: string): TPerson; begin Result := Mk('cls' + n, 2); end;
procedure Show(const p: TPerson);
begin writeln(p.name, ' ', p.age, ' ', p.tags[1], ' ', p.tags[2], ' ', p.inner.a, ' ', p.inner.n); end;
procedure ByVal(p: TPerson);
begin p.name[1] := 'X'; p.tags[1] := 'chg'; Show(p); end;
var
  a, b: TPerson;
  arr: array[1..2] of TPerson;
  q: ^TPerson;
  o: TBase;
  i: longint;
  s: string;
  ad, ad2: TAdv;
  pl: TPlain;
  cnt: longint = 0;
function Counted: TPerson;
begin cnt := cnt + 1; Result := Mk(IntToStr(cnt), cnt); end;
begin
  a := Mk('ann', 30); Show(a);
  b := a; b.name[1] := 'B'; b.tags[1] := 'other'; Show(a); Show(b);
  Show(Mk('tmp', 1));
  ByVal(Mk('val', 2));
  ByVal(a); Show(a);
  writeln(Mk('f', 3).name, Mk('g', 4).tags[2], Mk('h', 5).inner.a, length(Mk('i', 6).name));
  s := Mk('j', 7).name; writeln(s);
  Mk('discard', 1);
  Show(ByName('byname'));
  Show(ViaExit(0)); Show(ViaExit(1)); Show(ViaExit(2));
  a := Chain(a); Show(a);
  a := Chain(Chain(Mk('cc', 1))); Show(a);
  Show(Recur(4));
  writeln(Catch('x'));
  Show(Fin('fin'));
  arr[1] := Mk('e1', 1); arr[2] := arr[1]; arr[2].name := 'e2'; Show(arr[1]); Show(arr[2]);
  arr[1] := Mk('e3', 3); arr[1] := arr[1]; Show(arr[1]);
  new(q); q^ := Mk('heap', 1); Show(q^); q^ := Mk('heap2', 2); Show(q^); dispose(q);
  o := TKid.Create;
  Show(o.Get('z'));
  Show(TBase.Make('m')); Show(o.Make('n'));
  o.P := Mk('prop', 8); Show(o.P); Show(o.Raw); o.Raw := Mk('raw', 9); Show(o.FP);
  writeln(o.P.name, o.Get('y').name);
  o.Free;
  for i := 1 to 3 do begin a := Mk('loop' + IntToStr(i), i); Show(a); end;
  i := 0; while Mk(IntToStr(i), i).age < 3 do i := i + 1; writeln(i);
  repeat i := i - 1; until Mk('r', i).age <= 0; writeln(i);
  if Mk('c', 1).name = 'c!' then writeln('eq');
  if (Counted.age = 1) and (Counted.age = 2) then writeln('twice');
  writeln(cnt);
  writeln(length(Long.name)); a := Long; writeln(length(a.name));
  pl := P2; writeln(pl.x, pl.y, P2.y);
  ad := 'x'; writeln(ad.s); ad2 := ad + ad; writeln(ad2.s); writeln((ad + ad2).s); writeln(ad.Twice.s); ad := ad.Twice.Twice; writeln(ad.s);
  writeln(Format('%s %d', [Mk('fmt', 1).name, Mk('n', 2).age]));
  writeln('done');
end.
