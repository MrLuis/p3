program sets;
type
  TColor = (Red, Green, Blue);
  TColors = set of TColor;
  TMid = set of 0..63;
  TChars = set of char;
var
  colors: TColors;
  mid: TMid;
  chars: TChars;
  c: char;
  k: TColor;
  n: integer;
  ok: boolean;

procedure Member;
begin
  ok := k in colors;
end;

procedure MidMember;
begin
  ok := n in mid;
end;

procedure WideMember;
begin
  ok := c in chars;
end;

procedure Ops;
begin
  colors := colors + [k];
  mid := mid * [1, 40];
  chars := chars - ['a'];
  ok := colors <= [Red, Green];
end;

procedure Mixed;
begin
  chars := chars + ['x'];
  mid := mid + [n];
end;

procedure InOne;
begin
  ok := c in ['x'];
end;

procedure InRange;
begin
  ok := c in ['0'..'9'];
end;

procedure InMask;
begin
  ok := c in ['a'..'z', 'A'..'Z', '_'];
end;

procedure InRanges;
begin
  ok := c in ['0'..'9', 'a'..'z'];
end;

procedure InVar;
begin
  ok := n in [n + 1, 5];
end;

begin
  Member; MidMember; WideMember; Ops; Mixed; InOne; InRange; InMask; InRanges;
  InVar;
  writeln(sizeof(TColors), sizeof(TMid), sizeof(TChars));
end.
