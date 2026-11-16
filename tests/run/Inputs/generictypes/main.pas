program main;
{$H+}
uses gtypes;
type
  TInts = specialize TBag<Integer>;
var
  names: TNames;
  again: specialize TBag<string>;
  ints: TInts;
  p: specialize TPair<Integer, string>;
begin
  names := MakeNames;
  again := names;
  again.Add('cy');
  ints := TInts.Create;
  ints.Add(40);
  ints.Add(2);
  p.First := ints.Item(0) + ints.Item(1);
  p.Second := names.Item(2);
  Writeln(p.First, ' ', p.Second, ' ', names.Count, ' ', ints.Count);
  names.Free;
  ints.Free;
end.
