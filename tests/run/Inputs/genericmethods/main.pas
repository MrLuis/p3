program main;
{$H+}
uses boxes;
type
  TIntCell = specialize TCell<Integer>;
var
  b: TBox;
  c: TIntCell;
begin
  b := TBox.Create('b');
  Writeln(b.Wrap(7));
  Writeln(b.Wrap('s'));
  c := TIntCell.Create;
  Writeln(c.Convert(2.5):0:1);
  c.Free;
  b.Free;
end.
