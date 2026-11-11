program errors;
const
  Neg: QWord = -1;
  Max: QWord = 18446744073709551615;
  Over: int64 = 9223372036854775808;
type
  TSet = set of QWord;
  TArr = array[QWord] of byte;
var
  q: QWord;
procedure P(x: QWord = -2);
begin
end;
begin
  case q of
    -1: ;
    5..3: ;
    9223372036854775807..9223372036854775809: ;
    9223372036854775808: ;
  end;
  P;
  writeln(Max);
end.
