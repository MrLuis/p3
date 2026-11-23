unit tables;
{ A unit's compiletime constants: in the interface (the functions'
  bodies in the implementation) and in the implementation. }
interface
type
  TSquares = array[1..5] of integer;
  TDay = (Mon, Tue, Wed);
function Squares: TSquares; compiletime;
function Greeting(const who: string): string; compiletime;
const
  Sq = Squares;
  Hello = Greeting('unit');
function Picked: TDay;
implementation
function Squares: TSquares;
var
  i: integer;
begin
  for i := 1 to 5 do
    Result[i] := i * i;
end;
function Greeting(const who: string): string;
begin
  Greeting := 'hello, ' + who;
end;
function Pick(d: integer): TDay; compiletime;
begin
  Pick := TDay(d);
end;
const
  PickedDay = Pick(2);
function Picked: TDay;
begin
  Picked := PickedDay;
end;
end.
