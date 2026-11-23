{ A procedure lambda whose body is a call with arguments (a method's, or
  through a procedural value), written as an argument: the call statement
  ends at the ')' or ',' that follows it.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | FileCheck %s
  CHECK: one: 1 first
  CHECK-NEXT: two: x 2 second
  CHECK-NEXT: two: y 3 third
  CHECK-NEXT: nested:
  CHECK-NEXT: one:
  CHECK-NEXT: through a value: 6 sixth
}
{$mode objfpc}{$H+}
program lambdacallbody;
type
  TBox = class
    Log: string;
    procedure Put(N: Integer; const S: string);
  end;
  TAct = reference to procedure(B: TBox);
  TPutter = reference to procedure(N: Integer; const S: string);

procedure TBox.Put(N: Integer; const S: string);
begin
  if Log <> '' then Log := Log + ' ';
  Log := Log + Chr(Ord('0') + N) + ' ' + S;
end;

procedure One(const A: TAct);
var
  b: TBox;
begin
  b := TBox.Create;
  A(b);
  Writeln('one: ', b.Log);
  b.Free;
end;

procedure Two(const Tag: string; const A: TAct);
var
  b: TBox;
begin
  b := TBox.Create;
  A(b);
  Writeln('two: ', Tag, ' ', b.Log);
  b.Free;
end;

procedure Nested(const A: TAct);
var
  b: TBox;
begin
  b := TBox.Create;
  A(b);
  Writeln('nested: ', b.Log);
  b.Free;
end;

var
  b: TBox;
  put: TPutter;
begin
  One(procedure(B: TBox) => B.Put(1, 'first'));
  Two('x', procedure(B: TBox) => B.Put(2, 'second'));
  Nested(procedure(B: TBox) => Two('y', procedure(X: TBox) => X.Put(3, 'third')));
  b := TBox.Create;
  put := procedure(N: Integer; const S: string) => b.Put(N, S);
  One(procedure(B: TBox) => put(6, 'sixth'));
  Writeln('through a value: ', b.Log);
  b.Free;
end.
