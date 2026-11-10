{ pcc's Math: Free Pascal's names and results, over the C library.
  Written in the Turbo dialect so that every mode with units can use it;
  the floating-point overloads of Min, Max, Sign, InRange and EnsureRange
  need --mode=objfpc (overloading). Unlike Free Pascal, domain errors give
  NaN or infinity (the C library's results), not exceptions. }
unit math;

interface

function Floor(x: double): longint;
function Ceil(x: double): longint;
function Floor64(x: double): int64;
function Ceil64(x: double): int64;

function Power(base, exponent: double): double;
function IntPower(base: double; exponent: longint): double;
function Log10(x: double): double;
function Log2(x: double): double;
function LogN(n, x: double): double;
function Hypot(x, y: double): double;
function FMod(a, b: double): double;

function Tan(x: double): double;
function Cotan(x: double): double;
function ArcSin(x: double): double;
function ArcCos(x: double): double;
function ArcTan2(y, x: double): double;
function Sinh(x: double): double;
function Cosh(x: double): double;
function Tanh(x: double): double;
function DegToRad(deg: double): double;
function RadToDeg(rad: double): double;

function IsNan(x: double): boolean;
function IsInfinite(x: double): boolean;

function Sum(const data: array of double): double;
function SumInt(const data: array of longint): longint;
function Mean(const data: array of double): double;
function MinValue(const data: array of double): double;
function MaxValue(const data: array of double): double;
function MinIntValue(const data: array of longint): longint;
function MaxIntValue(const data: array of longint): longint;

function Min(a, b: int64): int64;
function Max(a, b: int64): int64;
function Sign(a: int64): longint;
function InRange(value, a, b: int64): boolean;
function EnsureRange(value, a, b: int64): int64;
{$IFDEF PCC_OBJFPC}
function MinValue(const data: array of longint): longint;
function MaxValue(const data: array of longint): longint;
function Min(a, b: double): double;
function Max(a, b: double): double;
function Sign(a: double): longint;
function InRange(value, a, b: double): boolean;
function EnsureRange(value, a, b: double): double;
{$ENDIF}

implementation

function c_floor(x: double): double; cdecl; external name 'floor';
function c_ceil(x: double): double; cdecl; external name 'ceil';
function c_pow(x, y: double): double; cdecl; external name 'pow';
function c_log10(x: double): double; cdecl; external name 'log10';
function c_log2(x: double): double; cdecl; external name 'log2';
function c_hypot(x, y: double): double; cdecl; external name 'hypot';
function c_fmod(x, y: double): double; cdecl; external name 'fmod';
function c_tan(x: double): double; cdecl; external name 'tan';
function c_asin(x: double): double; cdecl; external name 'asin';
function c_acos(x: double): double; cdecl; external name 'acos';
function c_atan2(y, x: double): double; cdecl; external name 'atan2';
function c_sinh(x: double): double; cdecl; external name 'sinh';
function c_cosh(x: double): double; cdecl; external name 'cosh';
function c_tanh(x: double): double; cdecl; external name 'tanh';

function Floor(x: double): longint;
begin
  Floor := trunc(c_floor(x));
end;

function Ceil(x: double): longint;
begin
  Ceil := trunc(c_ceil(x));
end;

function Floor64(x: double): int64;
begin
  Floor64 := trunc(c_floor(x));
end;

function Ceil64(x: double): int64;
begin
  Ceil64 := trunc(c_ceil(x));
end;

function Power(base, exponent: double): double;
begin
  Power := c_pow(base, exponent);
end;

function IntPower(base: double; exponent: longint): double;
begin
  IntPower := c_pow(base, exponent);
end;

function Log10(x: double): double;
begin
  Log10 := c_log10(x);
end;

function Log2(x: double): double;
begin
  Log2 := c_log2(x);
end;

function LogN(n, x: double): double;
begin
  LogN := ln(x) / ln(n);
end;

function Hypot(x, y: double): double;
begin
  Hypot := c_hypot(x, y);
end;

function FMod(a, b: double): double;
begin
  FMod := c_fmod(a, b);
end;

function Tan(x: double): double;
begin
  Tan := c_tan(x);
end;

function Cotan(x: double): double;
begin
  Cotan := 1.0 / c_tan(x);
end;

function ArcSin(x: double): double;
begin
  ArcSin := c_asin(x);
end;

function ArcCos(x: double): double;
begin
  ArcCos := c_acos(x);
end;

function ArcTan2(y, x: double): double;
begin
  ArcTan2 := c_atan2(y, x);
end;

function Sinh(x: double): double;
begin
  Sinh := c_sinh(x);
end;

function Cosh(x: double): double;
begin
  Cosh := c_cosh(x);
end;

function Tanh(x: double): double;
begin
  Tanh := c_tanh(x);
end;

function DegToRad(deg: double): double;
begin
  DegToRad := deg * (3.14159265358979323846 / 180.0);
end;

function RadToDeg(rad: double): double;
begin
  RadToDeg := rad * (180.0 / 3.14159265358979323846);
end;

function IsNan(x: double): boolean;
begin
  IsNan := x <> x;
end;

function IsInfinite(x: double): boolean;
begin
  IsInfinite := (x = x) and (x <> 0.0) and (x / 2.0 = x);
end;

function Sum(const data: array of double): double;
var
  i: longint;
  r: double;
begin
  r := 0.0;
  for i := 0 to high(data) do
    r := r + data[i];
  Sum := r;
end;

function SumInt(const data: array of longint): longint;
var
  i, r: longint;
begin
  r := 0;
  for i := 0 to high(data) do
    r := r + data[i];
  SumInt := r;
end;

function Mean(const data: array of double): double;
begin
  Mean := Sum(data) / length(data);
end;

function MinValue(const data: array of double): double;
var
  i: longint;
  r: double;
begin
  r := data[0];
  for i := 1 to high(data) do
    if data[i] < r then
      r := data[i];
  MinValue := r;
end;

function MaxValue(const data: array of double): double;
var
  i: longint;
  r: double;
begin
  r := data[0];
  for i := 1 to high(data) do
    if data[i] > r then
      r := data[i];
  MaxValue := r;
end;

function MinIntValue(const data: array of longint): longint;
var
  i, r: longint;
begin
  r := data[0];
  for i := 1 to high(data) do
    if data[i] < r then
      r := data[i];
  MinIntValue := r;
end;

function MaxIntValue(const data: array of longint): longint;
var
  i, r: longint;
begin
  r := data[0];
  for i := 1 to high(data) do
    if data[i] > r then
      r := data[i];
  MaxIntValue := r;
end;

function Min(a, b: int64): int64;
begin
  if a < b then Min := a else Min := b;
end;

function Max(a, b: int64): int64;
begin
  if a > b then Max := a else Max := b;
end;

function Sign(a: int64): longint;
begin
  if a < 0 then Sign := -1
  else if a > 0 then Sign := 1
  else Sign := 0;
end;

function InRange(value, a, b: int64): boolean;
begin
  InRange := (value >= a) and (value <= b);
end;

function EnsureRange(value, a, b: int64): int64;
begin
  if value < a then EnsureRange := a
  else if value > b then EnsureRange := b
  else EnsureRange := value;
end;

{$IFDEF PCC_OBJFPC}
function MinValue(const data: array of longint): longint;
begin
  MinValue := MinIntValue(data);
end;

function MaxValue(const data: array of longint): longint;
begin
  MaxValue := MaxIntValue(data);
end;

function Min(a, b: double): double;
begin
  if a < b then Min := a else Min := b;
end;

function Max(a, b: double): double;
begin
  if a > b then Max := a else Max := b;
end;

function Sign(a: double): longint;
begin
  if a < 0.0 then Sign := -1
  else if a > 0.0 then Sign := 1
  else Sign := 0;
end;

function InRange(value, a, b: double): boolean;
begin
  InRange := (value >= a) and (value <= b);
end;

function EnsureRange(value, a, b: double): double;
begin
  if value < a then EnsureRange := a
  else if value > b then EnsureRange := b
  else EnsureRange := value;
end;
{$ENDIF}

end.
