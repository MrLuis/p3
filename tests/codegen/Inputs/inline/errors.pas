program errors;
type
  TC = class
    procedure V; virtual; inline;
    procedure O; inline; override;
  end;
procedure F(x: integer); inline; forward;
function Ext(x: integer): integer; cdecl; inline; external 'c' name 'abs';
{$inline maybe}
procedure F(x: integer); begin end;
procedure TC.V; begin end;
procedure TC.O; begin end;
begin
end.
