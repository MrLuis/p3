{ COM interfaces: more lifetimes. A nested routine storing into its
  parent's variable, a chain of objects held by fields, results of calls
  through procedural variables, a finally part an exception passes,
  class variables. Output is identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/comlifetimes.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/comlifetimes.expected %t.O2.out
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/comlifetimes.expected %t.g.out
}
program comlifetimes;
uses sysutils;
type
  IFoo = interface ['{11111111-1111-1111-1111-111111111111}']
    function Name: string;
    function Child: IFoo;
  end;
  TFoo = class(TInterfacedObject, IFoo)
    N: string;
    Kid: IFoo;
    class var Shared: IFoo;
    constructor Create(const s: string);
    destructor Destroy; override;
    function Name: string;
    function Child: IFoo;
  end;
  TMaker = function(const s: string): IFoo;
constructor TFoo.Create(const s: string); begin N := s; writeln('  + ', s); end;
destructor TFoo.Destroy; begin writeln('  - ', N); inherited; end;
function TFoo.Name: string; begin Name := N; end;
function TFoo.Child: IFoo; begin Child := Kid; end;
function Make(const s: string): IFoo; begin Result := TFoo.Create(s); end;

procedure Outer;
var mine: IFoo;
  procedure Inner;
  var loc: IFoo;
  begin
    loc := TFoo.Create('inner');
    mine := loc;
  end;
begin
  Inner;
  writeln('  outer has ', mine.Name);
end;

procedure Chain;
var a, b: IFoo; f: TFoo;
begin
  f := TFoo.Create('parent');
  a := f;
  f.Kid := TFoo.Create('kid');
  b := a.Child;
  a := nil;
  writeln('  kid alive: ', b.Name);
end;

procedure Proc;
var m: TMaker; x: IFoo;
begin
  m := @Make;
  x := m('via proc var');
  writeln('  ', x.Name);
end;

procedure Raising;
var x: IFoo;
begin
  x := TFoo.Create('finally');
  try
    raise Exception.Create('oops');
  finally
    writeln('  in finally');
  end;
end;

procedure ClassVars;
var x: IFoo;
begin
  x := TFoo.Create('shared');
  TFoo.Shared := x;
  x := nil;
  writeln('  held by class var');
  TFoo.Shared := nil;
end;

begin
  writeln('outer'); Outer;
  writeln('chain'); Chain;
  writeln('proc'); Proc;
  writeln('raising');
  try Raising; except on E: Exception do writeln('  caught ', E.Message); end;
  writeln('class vars'); ClassVars;
  writeln('end');
end.
