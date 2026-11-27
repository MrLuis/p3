unit counter;
{ Mutable state (n), an unused routine and a cdecl routine: a unit
  linkUnitIR would not import. }
interface
procedure Bump;
function Count: integer;
procedure Unused(x: integer);
function CAdd(a, b: integer): integer; cdecl;
implementation
var n: integer;
procedure Bump; begin n := n + 1; end;
function Count: integer; begin Count := n; end;
procedure Unused(x: integer); begin writeln('unused ', x); end;
function CAdd(a, b: integer): integer; cdecl; begin CAdd := a + b; end;
end.
