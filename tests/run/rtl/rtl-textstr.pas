{ TStrings.GetTextStr adds up the lines and copies them once; a
  TStringList reads its items directly (docs/classes-review.md CL-3), a
  descendant goes through Strings[i]. Both must give the same Text: an
  empty list, one line, empty lines, random lines. Output identical to Free
  Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-textstr.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-textstr.expected
}
program rtltextstr;
{$H+}
uses sysutils, classes;

type
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

function Visible(const s: string): string;
var
  i: longint;
begin
  Result := '';
  for i := 1 to length(s) do
    if s[i] in [#10, #13] then
      Result := Result + '$'
    else
      Result := Result + s[i];
end;

procedure Show(const what: string; l: TStringList);
begin
  writeln(what, ': ', length(l.Text), ' [', Visible(l.Text), ']');
end;

var
  fast: TStringList;
  plain: TPlainList;
  i, j, k, n, differ: longint;
  s: string;
begin
  fast := TStringList.Create;
  plain := TPlainList.Create;
  Show('empty', fast);
  Show('empty plain', plain);
  fast.Add('one');
  plain.Add('one');
  Show('one', fast);
  Show('one plain', plain);
  fast.Add('');
  plain.Add('');
  fast.Add('');
  plain.Add('');
  fast.Add('x');
  plain.Add('x');
  Show('empties', fast);
  Show('empties plain', plain);
  fast.Clear;
  plain.Clear;
  fast.Add('');
  plain.Add('');
  Show('one empty', fast);
  Show('one empty plain', plain);

  seed := 7;
  differ := 0;
  for i := 1 to 200 do
  begin
    fast.Clear;
    plain.Clear;
    n := Rnd(30);
    for j := 1 to n do
    begin
      s := '';
      for k := 1 to Rnd(40) do
        s := s + chr(97 + Rnd(26));
      fast.Add(s);
      plain.Add(s);
    end;
    if fast.Text <> plain.Text then
      differ := differ + 1;
    fast.Text := fast.Text;
    if fast.Count <> n then
      differ := differ + 1;
  end;
  writeln('differences: ', differ);
  fast.Free;
  plain.Free;
end.
