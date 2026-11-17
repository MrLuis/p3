{ for-in through a helper (objfpc): a type or record helper can give a
  type its GetEnumerator, and for ... in uses it (the type's own comes
  first; arrays, strings and sets keep their own iteration).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/forinhelpers.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/forinhelpers.expected
}
{$modeswitch typehelpers}
{$modeswitch advancedrecords}
program forinhelpers;

type
  { a plain record: its enumerator comes from a helper }
  TRange = record
    Lo, Hi: Integer;
  end;
  TRangeEnum = record
    Cur, Hi: Integer;
    function MoveNext: Boolean;
    property Current: Integer read Cur;
  end;
  TRangeHelper = record helper for TRange
    function GetEnumerator: TRangeEnum;
  end;

  { a generic record without methods, enumerated through a generic helper }
  generic TBag<T> = record
    Items: array of T;
  end;
  generic TBagEnum<T> = record
    Items: array of T;
    Pos: Integer;
    function MoveNext: Boolean;
    function GetCurrent: T;
    property Current: T read GetCurrent;
  end;
  generic TBagHelper<T> = record helper for specialize TBag<T>
    function GetEnumerator: specialize TBagEnum<T>;
  end;

function TRangeEnum.MoveNext: Boolean;
begin
  Inc(Cur);
  Result := Cur <= Hi;
end;

function TRangeHelper.GetEnumerator: TRangeEnum;
begin
  Result.Cur := Lo - 1;
  Result.Hi := Hi;
end;

function TBagEnum.MoveNext: Boolean;
begin
  Dec(Pos);
  Result := Pos >= 0;
end;

function TBagEnum.GetCurrent: T;
begin
  Result := Items[Pos];
end;

function TBagHelper.GetEnumerator: specialize TBagEnum<T>;
begin
  Result.Items := Items;
  Result.Pos := Length(Items); { backwards }
end;

var
  r: TRange;
  b: specialize TBag<string>;
  i: Integer;
  s: string;
begin
  r.Lo := 3;
  r.Hi := 6;
  for i in r do
    Write(i, ' ');
  Writeln;
  b.Items := ['one', 'two', 'three'];
  for s in b do
    Write(s, ' ');
  Writeln;
  for var x in b do
    Write(Length(x), ' ');
  Writeln;
end.
