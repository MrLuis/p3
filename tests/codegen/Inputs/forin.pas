program forin;
{$H+}
type
  TEnum = class
    function MoveNext: boolean;
    function GetCur: char;
    property Current: char read GetCur;
  end;
  TBag = class
    function GetEnumerator: TEnum;
  end;
function TEnum.MoveNext: boolean; begin result := false end;
function TEnum.GetCur: char; begin result := 'x' end;
function TBag.GetEnumerator: TEnum; begin result := TEnum.Create end;
var
  a: array of longint;
  s: string;
  c: char;
  i: longint;
  bag: TBag;
begin
  for i in a do
    write(i);
  for c in s do
    write(c);
  for c in ['a', 'z'] do
    write(c);
  for c in bag do
    write(c);
end.
