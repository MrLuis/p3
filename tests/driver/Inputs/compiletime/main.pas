program main;
{ Compiletime constants: an array, a record, a string, an enumeration and
  numbers, computed while compiling; one from another; one as an array
  bound; and from a unit's compiletime functions. }
uses tables;
type
  TCrcTable = array[0..255] of longword;
  TColor = (Red, Green, Blue);
  TInfo = record
    Name: string;
    Color: TColor;
    Ratio: double;
    Ch: char;
    Ok: boolean;
  end;

function BuildCrcTable: TCrcTable; compiletime;
var
  i, j: integer;
  c: longword;
begin
  for i := 0 to 255 do
  begin
    c := i;
    for j := 1 to 8 do
      if odd(c) then
        c := (c shr 1) xor $EDB88320
      else
        c := c shr 1;
    Result[i] := c;
  end;
end;

function Fib(n: integer): int64; compiletime;
begin
  if n < 2 then
    Fib := n
  else
    Fib := Fib(n - 1) + Fib(n - 2);
end;

function Info(k: integer): TInfo; compiletime;
begin
  Result.Name := 'it''s ' + chr(65 + k) + #10 + 'x';
  Result.Color := Blue;
  Result.Ratio := 1 / 3;
  Result.Ch := 'q';
  Result.Ok := k > 1;
end;

const
  CrcTable = BuildCrcTable;
  F30 = Fib(30);
  Small = Fib(F30 mod 7);
  I1 = Info(2);
  Again = Greeting('program');

var
  a: array[1..Small] of integer;

begin
  writeln(CrcTable[1], ' ', CrcTable[255]);
  writeln(F30, ' ', Small, ' ', high(a));
  write(I1.Name);
  writeln(' ', I1.Color, ' ', I1.Ratio = 1 / 3, ' ', I1.Ch, ' ', I1.Ok);
  writeln(Sq[3], ' ', Sq[5], ' ', Hello, ' ', Again, ' ', Picked);
end.
