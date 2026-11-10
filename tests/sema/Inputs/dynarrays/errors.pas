program errors;
type
  TI = array of longint;
  TF = array of text;
  TFile = file of TI;
  TV = record
    case boolean of
      true: (a: TI);
      false: (b: longint);
  end;
const
  C1: TI = (1, 2);
var
  a, b: TI;
  m: array of TI;
  s: array of string;
begin
  setlength(a);
  setlength(a, 'x');
  setlength(a, 1, 2);
  setlength(m, 1, 2, 3);
  setlength(copy(a), 1);
  a['x'] := 1;
  a := 5;
  a := s;
  if a < b then;
  writeln(high(TI));
  writeln(low(TI));
  a := copy(a, 'x');
  a := copy(a, 1, 2, 3);
  a := [1, 'x'];
  a := [1..2];
  m := [1];
  writeln(a);
end.
