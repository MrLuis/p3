unit fmt;
{ Overloaded routines and methods in a unit's interface. }
interface
type
  TFormatter = class
    function Show(x: integer): string; overload;
    function Show(const s: string): string; overload;
  end;
function Twice(x: integer): integer; overload;
function Twice(const s: string): string; overload;
implementation
function TFormatter.Show(x: integer): string;
begin
  Show := '#' + chr(ord('0') + x);
end;
function TFormatter.Show(const s: string): string;
begin
  Show := '"' + s + '"';
end;
function Twice(x: integer): integer;
begin
  Twice := 2 * x;
end;
function Twice(const s: string): string;
begin
  Twice := s + s;
end;
end.
