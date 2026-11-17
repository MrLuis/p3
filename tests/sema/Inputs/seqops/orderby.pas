program orderby;
uses sequences;
type
  TPoint = record
    X, Y: Integer;
  end;
var
  pts: array of TPoint;
begin
  pts.OrderBy(function(p) => p).Count;
end.
