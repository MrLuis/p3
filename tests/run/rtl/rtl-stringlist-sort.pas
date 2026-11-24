{ TStringList.Sort and CustomSort: lists of 0 to 600 items with few or many
  distinct values (and all equal, sorted, reversed, organ pipe) come out in
  order, hold the same items, and every object stays with its string.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-stringlist-sort.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-stringlist-sort.expected
}
program stringlistsort;
{$H+}{$Q-}{$R-}
uses sysutils, classes;

type
  TTag = class
    Index: Integer;
  end;

var
  Seed: Cardinal = 777;
  Tags: array of TTag;
  Bad, Lists: Integer;

function Rnd(N: Integer): Integer;
begin
  Seed := (Seed * 1103515245 + 12345) and $7FFFFFFF;
  Result := (Seed shr 8) mod N;
end;

function Desc(List: TStringList; I, J: longint): longint;
begin
  Result := CompareStr(List[J], List[I]);
end;

function Tag(I: Integer): TTag;
begin
  Result := TTag.Create;
  Result.Index := I;
  SetLength(Tags, Length(Tags) + 1);
  Tags[High(Tags)] := Result;
end;

procedure Fill(L: TStringList; Kind, N, Distinct: Integer);
var
  I: Integer;
begin
  L.Clear;
  for I := 0 to N - 1 do
    case Kind of
      0: L.AddObject(Format('k%.5d', [Rnd(Distinct)]), Tag(I));
      1: L.AddObject(Format('k%.5d', [I]), Tag(I));
      2: L.AddObject(Format('k%.5d', [N - I]), Tag(I));
      3: L.AddObject(Format('k%.5d', [Distinct]), Tag(I));
      4: if I < N div 2 then
           L.AddObject(Format('k%.5d', [I]), Tag(I))
         else
           L.AddObject(Format('k%.5d', [N - I]), Tag(I));
    end;
end;

procedure Check(const Name: string; L: TStringList; const Orig: array of string; Down: Boolean);
var
  I: Integer;
  Seen: array of Boolean;
begin
  Inc(Lists);
  SetLength(Seen, Length(Orig));
  for I := 0 to L.Count - 1 do
  begin
    if I > 0 then
      if (not Down and (CompareStr(L[I - 1], L[I]) > 0)) or (Down and (CompareStr(L[I - 1], L[I]) < 0)) then
      begin
        Inc(Bad);
        Writeln('out of order: ', Name);
        Exit;
      end;
    if Orig[TTag(L.Objects[I]).Index] <> L[I] then
    begin
      Inc(Bad);
      Writeln('object left its string: ', Name);
      Exit;
    end;
    Seen[TTag(L.Objects[I]).Index] := True;
  end;
  for I := 0 to High(Orig) do
    if not Seen[I] then
    begin
      Inc(Bad);
      Writeln('item lost: ', Name);
      Exit;
    end;
end;

var
  L: TStringList;
  Orig: array of string;
  Kind, N, D, I, K, E: Integer;
begin
  Bad := 0;
  Lists := 0;
  L := TStringList.Create;
  L.CaseSensitive := True;
  for Kind := 0 to 4 do
    for N := 0 to 60 do
      for D := 1 to 3 do
      begin
        Fill(L, Kind, N, D * D * 3);
        SetLength(Orig, N);
        for I := 0 to N - 1 do
          Orig[I] := L[I];
        L.Sort;
        Check('sort ' + IntToStr(Kind) + '/' + IntToStr(N), L, Orig, False);
        Fill(L, Kind, N, D * D * 3);
        for I := 0 to N - 1 do
          Orig[I] := L[I];
        L.CustomSort(@Desc);
        Check('desc ' + IntToStr(Kind) + '/' + IntToStr(N), L, Orig, True);
      end;
  for Kind := 0 to 4 do
    for K := 0 to 2 do
      for E := 0 to 3 do
      begin
        N := 100 + K * 250 + K * K * 25;
        D := 1;
        for I := 1 to E do
          D := D * 12;

        Fill(L, Kind, N, D);
        SetLength(Orig, N);
        for I := 0 to N - 1 do
          Orig[I] := L[I];
        L.Sort;
        Check('big ' + IntToStr(Kind) + '/' + IntToStr(N) + '/' + IntToStr(D), L, Orig, False);
      end;
  Writeln('sorted ', Lists, ' lists, ', Bad, ' wrong');
  L.Free;
end.
