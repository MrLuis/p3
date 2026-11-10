unit withdef;
{$H+}
interface
const
  Base = 40;
type
  TC = class
    function Get(a: longint = Base + 2): longint;
  end;
function Add(a: longint; b: longint = Base): longint;
function Greet(const who: string = 'world'): string;
implementation
function Add(a: longint; b: longint): longint;
begin
  Add := a + b;
end;
function Greet(const who: string = 'world'): string;
begin
  Greet := 'hello ' + who;
end;
function TC.Get(a: longint): longint;
begin
  Get := a;
end;
end.
