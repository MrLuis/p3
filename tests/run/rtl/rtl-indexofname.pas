{ TStringList.IndexOfName compares names in place (docs/classes-review.md
  CL-2); a descendant takes TStrings' way (a copy of each name, then
  DoCompareText). Both must find the same line: random name=value lines,
  names looked up with and without case, with '=', ':' and a letter as the
  separator, empty names, lines without a separator, and names that
  contain the separator. Output identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-indexofname.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-indexofname.expected
}
program rtlindexofname;
{$H+}
uses sysutils, classes;

type
  { the general path: not a TStringList itself }
  TPlainList = class(TStringList)
  end;

var
  seed: longint;

{$Q-}{$R-}
function Rnd(n: longint): longint;
begin
  seed := (seed * 1103515245 + 12345) and $7FFFFFFF;
  Result := seed mod n;
end;
{$Q+}{$R+}

function RandName(const sep: char): string;
const
  Chars = 'abAB=:_ 1';
var
  i, n: longint;
begin
  Result := '';
  n := Rnd(5);
  for i := 1 to n do
    Result := Result + Chars[1 + Rnd(length(Chars))];
  if Rnd(10) = 0 then
    Result := Result + sep;
end;

procedure Run(const sep: char; caseSensitive: boolean);
var
  fast: TStringList;
  plain: TPlainList;
  i, a, b, found, differ: longint;
  name: string;
begin
  fast := TStringList.Create;
  plain := TPlainList.Create;
  fast.NameValueSeparator := sep;
  plain.NameValueSeparator := sep;
  fast.CaseSensitive := caseSensitive;
  plain.CaseSensitive := caseSensitive;
  for i := 1 to 300 do
  begin
    case Rnd(6) of
      0: name := RandName(sep);                       { no separator added }
      1: name := RandName(sep) + sep;                 { empty value }
    else
      name := RandName(sep) + sep + 'v' + IntToStr(i);
    end;
    fast.Add(name);
    plain.Add(name);
  end;
  found := 0;
  differ := 0;
  for i := 1 to 3000 do
  begin
    name := RandName(sep);
    if Rnd(3) = 0 then
      name := UpperCase(name);
    a := fast.IndexOfName(name);
    b := plain.IndexOfName(name);
    if a <> b then
      inc(differ);
    if a >= 0 then
      inc(found);
  end;
  writeln('''', sep, ''' caseSensitive=', caseSensitive, ': ', found, ' found, ',
          differ, ' differ');
  fast.Free;
  plain.Free;
end;

var
  sl: TStringList;
begin
  seed := 11;
  Run('=', false);
  Run('=', true);
  Run(':', false);
  Run('a', false);
  Run('a', true);
  { Values[] and IndexOfName on a few lines }
  sl := TStringList.Create;
  sl.Add('Alpha=1');
  sl.Add('beta=2');
  sl.Add('=empty name');
  sl.Add('no separator');
  sl.Add('gamma==3');
  writeln(sl.IndexOfName('ALPHA'), ' ', sl.Values['Beta'], ' ', sl.IndexOfName(''),
          ' ', sl.IndexOfName('no separator'), ' ', sl.Values['gamma'], ' ',
          sl.IndexOfName('delta'));
  sl.CaseSensitive := true;
  writeln(sl.IndexOfName('ALPHA'), ' ', sl.IndexOfName('Alpha'));
  sl.Free;
end.
