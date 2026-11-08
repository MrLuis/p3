{ Overloading (--mode=objfpc): routines and methods of one name told
  apart by their parameters; the best fit (exact types, then wider integer
  types, integer before int64 before real, the nearest ancestor class);
  forward declarations, nested routines, procedural parameters and @p,
  virtual overloads and their overrides, constructors (and TObject.Create
  through `overload`), inherited calls, property accessors. Output is
  identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/overloads.expected %t.out
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/overloads.expected %t.O2.out
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/overloads.expected %t.g.out
}
program overloads;

type
  TAnimal = class
    Name: string;
    constructor Create; overload;
    constructor Create(const n: string); overload;
    procedure Speak; overload; virtual;
    procedure Speak(times: integer); overload; virtual;
    function Describe: string; overload;
    function Describe(const prefix: string): string; overload;
  end;

  TDog = class(TAnimal)
    constructor Create(const n: string; age: integer); overload;
    procedure Speak; override;
    procedure Speak(times: integer); override;
  end;

  TBox = class
  private
    FItems: array[0..3] of integer;
    FLabel: string;
    function Get(i: integer): integer; overload;
    function Get(const s: string): integer; overload;
    procedure Put(i: integer; v: integer);
  public
    property Items[i: integer]: integer read Get write Put; default;
    property Named[const s: string]: integer read Get;
  end;

  TIntFn = function(x: integer): integer;
  TRealFn = function(x: real): real;

constructor TAnimal.Create;
begin
  Name := 'nobody';
end;

constructor TAnimal.Create(const n: string);
begin
  Name := n;
end;

procedure TAnimal.Speak;
begin
  writeln('  ', Name, ': ...');
end;

procedure TAnimal.Speak(times: integer);
var
  i: integer;
begin
  for i := 1 to times do
    Speak;
end;

function TAnimal.Describe: string;
begin
  Describe := 'animal ' + Name;
end;

function TAnimal.Describe(const prefix: string): string;
begin
  Describe := prefix + ' ' + Describe;
end;

constructor TDog.Create(const n: string; age: integer);
begin
  inherited Create(n);
  writeln('  a dog of ', age);
end;

procedure TDog.Speak;
begin
  writeln('  ', Name, ': woof');
end;

procedure TDog.Speak(times: integer);
begin
  write('  (', times, ' times)');
  writeln;
  inherited;
end;

function TBox.Get(i: integer): integer;
begin
  Get := FItems[i];
end;

function TBox.Get(const s: string): integer;
begin
  Get := length(s) + length(FLabel);
end;

procedure TBox.Put(i: integer; v: integer);
begin
  FItems[i] := v;
end;

procedure Show(x: integer); overload;
begin
  writeln('integer ', x);
end;

procedure Show(x: int64); overload;
begin
  writeln('int64 ', x);
end;

procedure Show(x: real); overload;
begin
  writeln('real ', x:0:2);
end;

procedure Show(c: char); overload;
begin
  writeln('char ', c);
end;

procedure Show(const s: string); overload;
begin
  writeln('string ', s);
end;

procedure Show(b: boolean); overload;
begin
  writeln('boolean ', b);
end;

procedure Show(x, y: integer); overload;
begin
  writeln('pair ', x, ' ', y);
end;

procedure Show(a: TAnimal); overload;
begin
  writeln('animal ', a.Name);
end;

procedure Show(d: TDog); overload;
begin
  writeln('dog ', d.Name);
end;

procedure Swap(var a, b: integer); overload;
var
  t: integer;
begin
  t := a; a := b; b := t;
end;

procedure Swap(var a, b: string); overload;
var
  t: string;
begin
  t := a; a := b; b := t;
end;

function Twice(x: integer): integer; overload; forward;
function Twice(x: real): real; overload; forward;

function Twice(x: integer): integer;
begin
  Twice := 2 * x;
end;

function Twice(x: real): real;
begin
  Twice := 2 * x;
end;

function Halve(x: real): real;
begin
  Halve := x / 2;
end;

function ApplyInt(f: TIntFn; x: integer): integer;
begin
  ApplyInt := f(x);
end;

function ApplyReal(f: TRealFn; x: real): real;
begin
  ApplyReal := f(x);
end;

function Total: integer; overload;
begin
  Total := 0;
end;

function Total(a: integer): integer; overload;
begin
  Total := a;
end;

function Total(a, b: integer): integer; overload;
begin
  Total := Total(a) + Total(b) + Total;
end;

procedure Nested;
  function Sq(x: integer): integer; overload;
  begin
    Sq := x * x;
  end;
  function Sq(x: real): real; overload;
  begin
    Sq := x * x;
  end;
begin
  writeln('nested ', Sq(3), ' ', Sq(1.5):0:2);
end;

var
  i, j: integer;
  big: int64;
  r: real;
  b: byte;
  w: word;
  s, t: string;
  a: TAnimal;
  d: TDog;
  box: TBox;
  fi: TIntFn;
  fr: TRealFn;

begin
  i := 3; big := 4; r := 2.5; b := 1; w := 2;
  Show(i); Show(big); Show(r); Show('c'); Show('text'); Show(true);
  Show(1, 2); Show(5); Show(b); Show(w); Show(10000000000);
  Show(2 + 0.5);

  a := TAnimal.Create;
  d := TDog.Create('rex', 3);
  Show(a); Show(d); Show(TAnimal(d));
  a.Free;
  a := TAnimal.Create('tom');
  writeln(a.Describe, ' / ', a.Describe('the'));
  a.Speak; a.Speak(2);
  d.Speak; d.Speak(2);
  a.Free;
  a := d;
  a.Speak(1);
  d.Free;
  a := TDog.Create;
  writeln(a.Name, ' ', a.ClassName);
  a.Free;

  j := 7;
  Swap(i, j);
  s := 'left'; t := 'right';
  Swap(s, t);
  writeln('swapped ', i, ' ', j, ' ', s, ' ', t);

  writeln('twice ', Twice(21), ' ', Twice(1.25):0:2);
  writeln('apply ', ApplyInt(@Twice, 5), ' ', ApplyReal(@Halve, 0.5):0:2);
  fi := @Twice;
  fr := @Twice;
  writeln('values ', fi(10), ' ', fr(10):0:1);
  writeln('totals ', Total, ' ', Total(4), ' ', Total(4, 5));
  Nested;

  box := TBox.Create;
  box[1] := 11;
  box.Items[2] := box[1] * 2;
  writeln('box ', box[1], ' ', box.Items[2], ' ', box.Named['abc']);
  box.Free;
end.
