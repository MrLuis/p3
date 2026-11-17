{ The SCOPEDENUMS directive, ON or OFF (also + and -; tp and objfpc), as
  in Free Pascal: the values of an enumeration declared while it is on are
  reached only through its type (TColor.Red), so another Red can be
  declared; the values of every enumeration can be qualified (TPlain.pb).
  Qualified values work wherever constants do: subrange bounds, case and
  variant labels, sets, array indexes, typed constants, for loops. Output is
  identical to Free Pascal 3.2.2's (fpc -Mtp and -Mobjfpc).
  RUN: %p3 --mode=tp %s -o %t
  RUN: %t | diff - %S/scopedenums.expected
  RUN: %p3 --mode=objfpc %s -o %t.objfpc
  RUN: %t.objfpc | diff - %S/scopedenums.expected
  RUN: %p3 --mode=objfpc -O2 -g %s -o %t.O2
  RUN: %t.O2 | diff - %S/scopedenums.expected
}
program scopedenums;

type
  TPlain = (pa, pb, pc);
{$SCOPEDENUMS ON}
  TColor = (Red, Green, Blue);
  TShade = (Light, Dark, Red); { its own Red }
  TColors = set of TColor;
  TRec = record
    c: TColor;
    case k: TShade of
      TShade.Light: (n: integer);
      TShade.Dark, TShade.Red: (m: byte);
  end;
  TCounts = array[TColor] of integer;
  TWarm = TColor.Green..TColor.Blue;
{$scopedenums off}
  TOther = (oa, ob);
{$scopedenums+}
  TDir = (Up, Down);
{$scopedenums-}

const
  Red = 42; { no clash with TColor.Red }
  Favorite: TColor = TColor.Blue;
  Second = TColor.Green;

var
  c: TColor;
  s: TColors;
  a: TCounts;
  w: TWarm;
  r: TRec;
  d: TDir;
  sh: TShade;

function Describe(x: TColor): integer;
begin
  case x of
    TColor.Red: Describe := 1;
    TColor.Green..TColor.Blue: Describe := 2;
  end;
end;

begin
  c := TColor.Green;
  writeln(Ord(c), ' ', Red, ' ', Ord(Favorite), ' ', Ord(Second));
  writeln(Ord(TPlain.pb), ' ', Ord(pb), ' ', Ord(TOther.ob), ' ', Ord(ob));
  writeln(Ord(TShade.Red), ' ', Ord(TColor.Red), ' ', Ord(TDir.Down));
  s := [TColor.Red, TColor.Blue];
  writeln(TColor.Red in s, ' ', TColor.Green in s, ' ', TColor.Blue in [TColor.Green, TColor.Blue]);
  for c := Low(TColor) to High(TColor) do
    write(Ord(c), ':', Describe(c), ' ');
  writeln;
  for c := TColor.Blue downto TColor.Green do
    write(Ord(c), ' ');
  writeln;
  a[TColor.Blue] := 7;
  a[TColor.Red] := 3;
  writeln(a[TColor.Blue] + a[TColor.Red]);
  w := TColor.Blue;
  writeln(Ord(w), ' ', Ord(Low(TWarm)), ' ', Ord(High(TWarm)));
  writeln(Ord(Succ(TColor.Red)), ' ', Ord(Pred(TColor.Blue)), ' ', Ord(TColor(2)));
  r.c := TColor.Red;
  r.k := TShade.Dark;
  r.m := 9;
  writeln(Ord(r.c), ' ', Ord(r.k), ' ', r.m);
  d := TDir.Up;
  sh := TShade.Red;
  if (d = TDir.Up) and (sh <> TShade.Light) then
    writeln('qualified comparisons');
  writeln(TColor.Blue > TColor.Red, ' ', Ord(TColor.Blue) - Ord(TColor.Red));
end.
