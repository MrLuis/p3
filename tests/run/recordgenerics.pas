{ Generic methods in advanced records (objfpc): generic function M<T> in a
  record (also a generic record: TSeq<T>.Map<R>), implemented as generic
  function TRec.M<T>; class ones are static, as the record's class methods.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/recordgenerics.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/recordgenerics.expected
}
{$H+}
{$modeswitch advancedrecords}
program recordgenerics;
uses SysUtils;

type
  generic TFunc<T, R> = reference to function(const X: T): R;
  generic TNext<T> = reference to procedure(var X: T; var Ok: Boolean);
  generic TArr<T> = array of T;

  { the sequence with its operations in it (no helper needed) }
  generic TSeq<T> = record
    Next: specialize TNext<T>;
    generic function Map<R>(F: specialize TFunc<T, R>): specialize TSeq<R>;
    function ToArray: specialize TArr<T>;
    generic class function From<E>(const Items: array of E): specialize TSeq<E>; static;
  end;

  { a plain record: generic methods using Self }
  TCounter = record
    Total: Integer;
    generic procedure AddAll<T>(const Xs: array of T);
    generic function Describe<T>(const X: T): string;
  end;

generic function TSeq.Map<R>(F: specialize TFunc<T, R>): specialize TSeq<R>;
var
  N: specialize TNext<T>;
begin
  N := Next;
  Result.Next := procedure(var Y: R; var Ok: Boolean) => begin
      var X: T;
      N(X, Ok);
      if Ok then
        Y := F(X);
    end;
end;

function TSeq.ToArray: specialize TArr<T>;
var
  X: T;
  Ok: Boolean;
begin
  Result := nil;
  while true do
  begin
    Next(X, Ok);
    if not Ok then
      break;
    SetLength(Result, Length(Result) + 1);
    Result[High(Result)] := X;
  end;
end;

generic class function TSeq.From<E>(const Items: array of E): specialize TSeq<E>;
var
  A: specialize TArr<E>;
  Pos, I: Integer;
begin
  SetLength(A, Length(Items));
  for I := 0 to High(Items) do
    A[I] := Items[I];
  Pos := 0;
  Result.Next := procedure(var Y: E; var Ok: Boolean) => begin
      Ok := Pos < Length(A);
      if Ok then
      begin
        Y := A[Pos];
        Inc(Pos);
      end;
    end;
end;

generic procedure TCounter.AddAll<T>(const Xs: array of T);
var
  X: T;
begin
  for X in Xs do
    Total := Total + Length(X);
end;

generic function TCounter.Describe<T>(const X: T): string;
begin
  Result := IntToStr(Total) + ':' + X;
end;

var
  s: specialize TSeq<Integer>;
  out: specialize TArr<string>;
  c: TCounter;
  t: string;
begin
  s := specialize TSeq<Integer>.From([1, 2, 3]);
  out := s.Map(function(x) => 'n' + IntToStr(x * 10)).ToArray;
  for t in out do
    Write(t, ' ');
  Writeln;
  c.Total := 0;
  c.AddAll(['ab', 'cde']);
  c.specialize AddAll<string>(['f']);
  Writeln(c.Total, ' ', c.Describe('x'));
end.
