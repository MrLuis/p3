program errors;
{$modeswitch advancedrecords}
type
  TR = record
  strict private
    Secret: integer;
  private
    Hidden: integer;
  public
    V: integer;
    constructor Create;
    procedure P; virtual;
    procedure Q;
    procedure NotImplemented;
    function Get: integer;
    class function Make: TR; static;
    class operator +(const a, b: TR): TR;
    class operator *(const a: TR; k: integer): TR;
    class operator -(a, b, c: TR): TR;
    class operator not(var a: TR): TR;
    class operator /(a, b: integer): integer;
  end;

constructor TR.Create; begin end;
procedure TR.P; begin end;
procedure TR.Q; begin Secret := 1; Hidden := 2; end;
function TR.Get: integer; begin Get := V; end;
class function TR.Make: TR; begin Result.V := V; end;
procedure TR.Missing; begin end;
class operator TR.+(const a, b: TR): TR; begin Result.V := a.V + b.V; end;
class operator TR.*(const a: TR; k: integer): TR; begin Result.V := a.V * k; end;

procedure Local;
type
  TL = record
    x: integer;
    procedure M;
  end;
begin
end;

var
  r, s: TR;
  anon: record x: integer; procedure M; end;
  i: integer;
begin
  r.Secret := 1;
  r.Hidden := 2;
  TR.Q;
  s := r * r;
  i := -r;
  i := integer(r);
end.
