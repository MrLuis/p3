{ write and writeln of enumeration values (tp and objfpc), as in Free
  Pascal: the value's name as declared (its case kept), also of subranges,
  record fields, array elements, scoped enumerations, function results and
  succ/pred/High/Low, to output or to a text file variable; a field width
  pads on the right and never cuts. Output is identical to Free Pascal
  3.2.2's (fpc -Mtp and -Mobjfpc).
  RUN: %p3 --mode=tp %s -o %t
  RUN: %t | diff - %S/enum-write.expected
  RUN: %p3 --mode=objfpc %s -o %t.objfpc
  RUN: %t.objfpc | diff - %S/enum-write.expected
  RUN: %p3 --mode=objfpc -O2 -g %s -o %t.O2
  RUN: %t.O2 | diff - %S/enum-write.expected
}
program enumwrite;

type
  TColor = (Red, greenish, BLUE);
  TWarm = greenish..BLUE;
  TRec = record
    c: TColor;
    n: integer;
  end;
{$scopedenums on}
  TShade = (Light, Dark);
{$scopedenums off}
  TDay = (Mon, Tue, Wed, Thu, Fri, Sat, Sun);

var
  c: TColor;
  w: TWarm;
  r: TRec;
  a: array[1..2] of TColor;
  s: TShade;
  d: TDay;
  b: boolean;
  v: (Alpha, Beta);

function Fav: TColor;
begin
  Fav := BLUE;
end;

begin
  c := BLUE;
  w := greenish;
  r.c := Red;
  a[2] := c;
  s := TShade.Dark;
  writeln(c, ' ', w, ' ', r.c, ' ', a[2], ' ', s, ' ', TShade.Light);
  writeln('[', c:8, '][', c:-8, '][', Red:2, '][', Fav:0, ']');
  writeln(Succ(Red), ' ', TColor(1), ' ', High(TColor), ' ', Low(TWarm), ' ', Pred(c));
  write(c);
  write(Fav);
  writeln;
  writeln(output, c, Red, greenish);
  for d := Mon to Sun do
    write(d:4);
  writeln;
  for d := Sun downto Fri do
    write(Ord(d), '=', d, ' ');
  writeln;
  v := Beta;
  b := true;
  writeln(v, ' ', Alpha, ' ', b, ' ', c = BLUE);
end.
