{ Sets: membership in constant and variable sets, union, intersection,
  difference, sets of an enumeration. }
program sets;
type
  TKind = (kIdent, kNumber, kString, kPlus, kMinus, kStar, kSlash, kLParen,
           kRParen, kComma, kSemi, kBegin, kEnd, kIf, kThen, kElse);
  TKinds = set of TKind;
  TChars = set of char;
var
  Operators, Starters: TKinds;

function Classify(c: char): longint;
begin
  if c in ['a'..'z', 'A'..'Z', '_'] then
    Classify := 1
  else if c in ['0'..'9'] then
    Classify := 2
  else if c in [' ', #9, #10, #13] then
    Classify := 3
  else
    Classify := 4;
end;

var
  i, j, sum: longint;
  k: TKind;
  seen, follow: TKinds;
  chars: TChars;
begin
  Operators := [kPlus, kMinus, kStar, kSlash];
  Starters := [kIdent, kNumber, kString, kLParen, kMinus];
  sum := 0;
  for i := 1 to 30000000 do
    sum := sum + Classify(chr(32 + i mod 95));
  writeln(sum);
  sum := 0;
  seen := [];
  for i := 1 to 30000000 do
  begin
    k := TKind(i mod 16);
    if k in Operators then
      seen := seen + [k]
    else if k in Starters then
      seen := seen - Operators
    else
      seen := seen * (Starters + [k]);
    follow := seen + [kSemi];
    if (kPlus in follow) and not (kStar in follow) then
      sum := sum + 1;
    if follow = [kSemi] then
      sum := sum + 2;
  end;
  writeln(sum);
  sum := 0;
  for i := 1 to 1500000 do
  begin
    chars := [];
    for j := 0 to 15 do
      chars := chars + [chr(65 + (i + j * 7) mod 58)];
    if 'Q' in chars then
      sum := sum + 1;
  end;
  writeln(sum);
end.
