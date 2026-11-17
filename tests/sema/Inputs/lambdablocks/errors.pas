program b2;
type
  TIntFn = function(x: Integer): Integer;
var
  g: TIntFn;
begin
  var a := function(x: Integer) => begin
             Writeln(x);
           end;
  var b := function(x: Integer) => begin
             exit(x);
           end;
  var c := function(x: Integer) => begin
             Result := nil;
           end;
  g := function(x) => begin
         Result := 'text';
       end;
end.
