unit words;
{$H+}
{$modeswitch advancedrecords}
interface
type
  TWordEnum = class
    p: longint;
    list: array of string;
    function MoveNext: boolean;
    function GetCur: string;
    property Current: string read GetCur;
  end;
  TWords = class
    items: array of string;
    procedure Add(const w: string);
    function GetEnumerator: TWordEnum;
  end;
  TDigits = record
    n, p: longint;
    function MoveNext: boolean;
    function GetCur: longint;
    property Current: longint read GetCur;
  end;
  TNumber = record
    value: longint;
    function GetEnumerator: TDigits;
  end;
function Total(const a: array of longint): longint;
implementation
function TWordEnum.MoveNext: boolean;
begin
  inc(p);
  result := p < length(list);
end;
function TWordEnum.GetCur: string;
begin
  result := list[p];
end;
procedure TWords.Add(const w: string);
begin
  setlength(items, length(items) + 1);
  items[high(items)] := w;
end;
function TWords.GetEnumerator: TWordEnum;
begin
  result := TWordEnum.Create;
  result.p := -1;
  result.list := items;
end;
function TDigits.MoveNext: boolean;
begin
  inc(p);
  result := p <= 4;
end;
function TDigits.GetCur: longint;
var
  k, v: longint;
begin
  v := n;
  for k := p to 3 do
    v := v div 10;
  result := v mod 10;
end;
function TNumber.GetEnumerator: TDigits;
begin
  result.n := value;
  result.p := 0;
end;
function Total(const a: array of longint): longint;
var
  x: longint;
begin
  result := 0;
  for x in a do
    result := result + x;
end;
end.
