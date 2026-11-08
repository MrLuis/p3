program rterrors;
{ A failed `as` on an interface, a call through a nil interface. }
type
  IA = interface procedure P; end;
  IB = interface procedure Q; end;
  TA = class(TObject, IA) procedure P; end;
procedure TA.P; begin writeln('TA.P') end;
var
  o: TObject;
  a: IA;
  b: IB;
  w: string;
begin
  o := TA.Create;
  a := o as IA;
  a.P;
  w := paramstr(1);
  if w = 'as' then
    b := o as IB;
  if w = 'nil' then
  begin
    a := nil;
    a.P;
  end;
end.
