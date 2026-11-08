program errors;
type
  TA = class
  strict private
    FS: integer;
    function G(i: integer): integer;
    function GS(const s: string): integer;
    procedure S(i: integer; v: integer);
    function GI(i: integer): integer;
  strict protected
    FP: integer;
  public
    property P[i: integer]: integer read G write S; default;
    property Q[i: integer]: integer read G; default;
    property R[i: integer]: integer read FS;
    property T[i: real]: integer read G;
    property U[s: string]: integer read GS;
    property V[var i: integer]: integer read G;
    property W: integer index 1 + 1 read GI;
    property X: integer index 'a' read GI;
    property Y: integer read FS default 'z';
    property Z: integer read FS; default;
    property RO[i: integer]: integer read G;
    procedure M;
  end;
  TB = class(TA)
    procedure N;
  end;
  TC = class
  end;
var
  a: TA;
  c: TC;
  n: integer;

procedure Take(var x: integer); begin end;

function TA.G(i: integer): integer; begin G := i end;
function TA.GS(const s: string): integer; begin GS := 0 end;
procedure TA.S(i: integer; v: integer); begin end;
function TA.GI(i: integer): integer; begin GI := i end;
procedure TA.M; begin FS := 1; FP := 2; n := P[1] + Self[2] end;
procedure TB.N; begin FP := 3; FS := 4 end;

begin
  a := TA.Create;
  n := a.FS;
  n := a.FP;
  n := a.P;
  a.P := 1;
  n := a.P['x'];
  a.RO[1] := 2;
  Take(a.P[1]);
  Take(a[1]);
  n := c[1];
  n := a.W;
end.
