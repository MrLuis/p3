program tpmode;
function Sq(x: integer): integer; inline;
begin Sq := x * x; end;
{$inline on}
function Cube(x: integer): integer; inline;
begin Cube := x * x * x; end;
begin writeln(Sq(2), Cube(2)); end.
