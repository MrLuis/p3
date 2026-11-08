{ Properties and visibility (--mode=objfpc): array properties (one and
  two indexes, const and value index parameters, string results), the
  default property (obj[i], Self[i], inherited and redeclared), indexed
  properties (index N), virtual accessors, defaults for streaming
  (default V, nodefault: no effect), strict private and strict protected.
  Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/properties.expected %t.out
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/properties.expected %t.O2.out
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/properties.expected %t.g.out
}
program properties;

type
  TRow = record
    v: array[0..2] of integer;
  end;

  TMatrix = class
  strict private
    FCells: array[0..2] of TRow;
    FName: string;
  private
    function GetCell(r, c: integer): integer;
    procedure SetCell(r, c: integer; v: integer);
    function GetRow(r: integer): TRow;
    function GetLabel(const prefix: string): string;
  protected
    function GetSize(which: integer): integer; virtual;
  public
    constructor Create(const name: string);
    procedure Fill;
    function Trace: integer;
    property Cells[r, c: integer]: integer read GetCell write SetCell; default;
    property Rows[r: integer]: TRow read GetRow;
    property Labels[const prefix: string]: string read GetLabel;
    property Height: integer index 0 read GetSize;
    property Width: integer index 1 read GetSize default 3;
    property Name: string read FName write FName nodefault;
  end;

  { Redeclares the default property; its sizes are virtual. }
  TBigMatrix = class(TMatrix)
  strict protected
    FScale: integer;
    function GetScaled(r: integer): integer;
  protected
    function GetSize(which: integer): integer; override;
  public
    property Scaled[r: integer]: integer read GetScaled; default;
    procedure SetScale(s: integer);
  end;

  TCounter = class
  strict private
    FCounts: array['a'..'e'] of integer;
    function GetCount(c: char): integer;
    procedure SetCount(c: char; v: integer);
  public
    property Counts[c: char]: integer read GetCount write SetCount; default;
    procedure Add(const s: string);
  end;

constructor TMatrix.Create(const name: string);
begin
  FName := name;
end;

function TMatrix.GetCell(r, c: integer): integer;
begin
  GetCell := FCells[r].v[c];
end;

procedure TMatrix.SetCell(r, c: integer; v: integer);
begin
  writeln('  ', FName, '[', r, ',', c, '] := ', v);
  FCells[r].v[c] := v;
end;

function TMatrix.GetRow(r: integer): TRow;
begin
  GetRow := FCells[r];
end;

function TMatrix.GetLabel(const prefix: string): string;
begin
  GetLabel := prefix + ' ' + FName;
end;

function TMatrix.GetSize(which: integer): integer;
begin
  GetSize := 3;
end;

procedure TMatrix.Fill;
var
  r, c: integer;
begin
  for r := 0 to Height - 1 do
    for c := 0 to Width - 1 do
      FCells[r].v[c] := r * 10 + c;
  Self[1, 1] := Cells[2, 2] + 1;
end;

function TMatrix.Trace: integer;
var
  i, t: integer;
begin
  t := 0;
  for i := 0 to Height - 1 do
    t := t + Self[i, i];
  Trace := t;
end;

function TBigMatrix.GetSize(which: integer): integer;
begin
  if which = 0 then
    GetSize := 2
  else
    GetSize := inherited GetSize(which);
end;

function TBigMatrix.GetScaled(r: integer): integer;
begin
  GetScaled := Cells[r, r] * FScale;
end;

procedure TBigMatrix.SetScale(s: integer);
begin
  FScale := s;
end;

function TCounter.GetCount(c: char): integer;
begin
  GetCount := FCounts[c];
end;

procedure TCounter.SetCount(c: char; v: integer);
begin
  FCounts[c] := v;
end;

procedure TCounter.Add(const s: string);
var
  i: integer;
begin
  for i := 1 to length(s) do
    if (s[i] >= 'a') and (s[i] <= 'e') then
      Counts[s[i]] := Counts[s[i]] + 1;
end;

var
  m: TMatrix;
  b: TBigMatrix;
  k: TCounter;
  row: TRow;
  c: char;
  i: integer;

begin
  m := TMatrix.Create('m');
  m.Fill;
  writeln('trace ', m.Trace, ', size ', m.Height, 'x', m.Width);
  m.Cells[0, 2] := 7;
  m[2, 0] := m.Cells[0, 2] * 2;
  writeln('m[2,0] = ', m[2, 0], ', m[0,2] = ', m.Cells[0, 2]);
  row := m.Rows[2];
  writeln('row 2: ', row.v[0], ' ', row.v[1], ' ', row.v[2],
          '; m.Rows[1].v[1] = ', m.Rows[1].v[1]);
  writeln(m.Labels['matrix'], ' / ', length(m.Labels['x']));
  m.Name := 'renamed';
  with m do
  begin
    Cells[1, 0] := 99;
    writeln(Labels['with'], ': ', Cells[1, 0], ' ', Height);
  end;

  b := TBigMatrix.Create('b');
  b.Fill;
  b.SetScale(10);
  writeln('big: size ', b.Height, 'x', b.Width, ', trace ', b.Trace);
  for i := 0 to 1 do
    writeln('  b[', i, '] = ', b[i], ', cells ', b.Cells[i, i]);

  k := TCounter.Create;
  k.Add('abracadabra');
  for c := 'a' to 'e' do
    write(c, '=', k[c], ' ');
  writeln;
  k['e'] := k['a'] + k.Counts['b'];
  writeln('e = ', k.Counts['e']);

  m.Free;
  b.Free;
  k.Free;
end.
