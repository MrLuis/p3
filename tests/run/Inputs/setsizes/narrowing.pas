program rc;
type
  TSmall = set of 0..31;
  TMid = set of 0..63;
  TLow = set of 3..10;
  TBytes = set of byte;
var
  small: TSmall; mid: TMid; low: TLow; wide: TBytes;
  n: integer; w: string;

procedure Take(s: TSmall);
begin
  writeln('taken');
end;

begin
  w := paramstr(1);
  wide := [1, 200]; mid := [2, 40]; small := [1, 20]; n := 300;
  if w = 'wide-small' then small := wide;
  if w = 'mid-small' then small := mid;
  if w = 'wide-mid' then mid := wide;
  if w = 'small-low' then low := small;
  if w = 'ctor' then small := [n];
  if w = 'ctor-small' then small := small + [40];
  if w = 'arg' then Take(wide);
  if w = 'fits' then
  begin
    small := wide - [200]; mid := wide * [0..63]; low := small * [3..10];
    Take(mid - [40]);
  end;
  writeln(1 in small, ' ', 20 in small, ' ', 2 in mid, ' ', 40 in mid, ' ', 3 in low);
end.
