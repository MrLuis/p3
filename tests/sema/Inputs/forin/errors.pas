program errors;
type
  TColor = (red, green, blue);
  TColors = set of TColor;
  TPlain = record f: integer; end;
  TNoEnum = class end;
  TBadGet = class function GetEnumerator(n: integer): TNoEnum; end;
  TNoMove = class private FCur: integer; public property Current: integer read FCur; end;
  TNoMoveBag = class function GetEnumerator: TNoMove; end;
  TNoCur = class function MoveNext: boolean; end;
  TNoCurBag = class function GetEnumerator: TNoCur; end;
var
  i: integer;
  s: string;
  a: array[1..3] of integer;
  r: TPlain;
  o: TNoEnum;
  b1: TBadGet;
  b2: TNoMoveBag;
  b3: TNoCurBag;
function TBadGet.GetEnumerator(n: integer): TNoEnum; begin result := nil end;
function TNoMoveBag.GetEnumerator: TNoMove; begin result := nil end;
function TNoCur.MoveNext: boolean; begin result := false end;
function TNoCurBag.GetEnumerator: TNoCur; begin result := nil end;
begin
  for i in a do i := 1;
  for s in a do ;
  for i in 5 do ;
  for i in r do ;
  for i in o do ;
  for i in b1 do ;
  for i in b2 do ;
  for i in b3 do ;
  for red in [red] do ;
  for i in TColors do ;
  for nosuch in a do ;
  for i in a do for i in a do ;
end.
