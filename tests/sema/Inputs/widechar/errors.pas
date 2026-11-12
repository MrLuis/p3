program errors;
type
  TSet = set of widechar;
  TLow = 'a'..'z';
const
  L: TLow = #$100;
  Ch: char = #$20AC;
var
  w: widechar;
  c: char;
  s: string;
procedure P(x: char); begin end;
procedure V(const a: array of const); begin end;
begin
  read(w);
  V([w]);
  c := chr(#$100);
  w := 'ab';
  c := #$D800;
  s := #$D800;
  P(#$20AC);
  c := #$41;
end.
