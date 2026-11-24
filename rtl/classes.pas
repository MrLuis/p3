{ p3's Classes: TList and TStringList with Free Pascal's names and results.
  Needs --mode=objfpc. }
unit classes;

{$H+}

interface

{$IFNDEF P3_OBJFPC}
uses classes_needs_mode_objfpc;
{$ELSE}

const
  MaxListSize = high(longint) div 16;

type
  TPointerList = array[0..MaxListSize - 1] of pointer;
  PPointerList = ^TPointerList;
  TListSortCompare = function(Item1, Item2: pointer): longint;
  TNotifyEvent = procedure(Sender: TObject) of object;
  TListNotification = (lnAdded, lnExtracted, lnDeleted);
  TDuplicates = (dupIgnore, dupAccept, dupError);

  EListError = class(Exception);
  EStringListError = class(Exception);
  EStreamError = class(Exception);
  EFCreateError = class(EStreamError);
  EFOpenError = class(EStreamError);

  TPersistent = class
  private
    procedure AssignError(Source: TPersistent);
  protected
    procedure AssignTo(Dest: TPersistent); virtual;
  public
    procedure Assign(Source: TPersistent); virtual;
  end;

  TList = class
  private
    FList: PPointerList;
    FCount, FCapacity: longint;
    function Get(Index: longint): pointer;
    procedure Put(Index: longint; Item: pointer);
    procedure SetCapacity(NewCapacity: longint);
    procedure SetCount(NewCount: longint);
    procedure CheckIndex(Index: longint);
    procedure Grow;
    procedure RemoveAt(Index: longint);
  protected
    procedure Notify(Ptr: pointer; Action: TListNotification); virtual;
  public
    destructor Destroy; override;
    function Add(Item: pointer): longint;
    procedure Clear; virtual;
    procedure Delete(Index: longint);
    procedure Exchange(Index1, Index2: longint);
    function Expand: TList;
    function Extract(Item: pointer): pointer;
    function First: pointer;
    function IndexOf(Item: pointer): longint;
    procedure Insert(Index: longint; Item: pointer);
    function Last: pointer;
    procedure Move(CurIndex, NewIndex: longint);
    function Remove(Item: pointer): longint;
    procedure Pack;
    procedure Sort(Compare: TListSortCompare);
    property Capacity: longint read FCapacity write SetCapacity;
    property Count: longint read FCount write SetCount;
    property Items[Index: longint]: pointer read Get write Put; default;
    property List: PPointerList read FList;
  end;

  TStrings = class(TPersistent)
  private
    FUpdateCount: longint;
    FDelimiter, FQuoteChar, FNameValueSeparator: char;
    FSpecialCharsInited, FStrictDelimiter: boolean;
    procedure CheckSpecialChars;
    function GetDelimiter: char;
    procedure SetDelimiter(c: char);
    function GetQuoteChar: char;
    procedure SetQuoteChar(c: char);
    function GetNameValueSeparator: char;
    procedure SetNameValueSeparator(c: char);
    function GetDelimited(d, q: char; strict: boolean): string;
    procedure SetDelimited(const AValue: string; d, q: char; strict: boolean);
    function GetDelimitedText: string;
    procedure SetDelimitedText(const AValue: string);
    function GetCommaText: string;
    procedure SetCommaText(const AValue: string);
    function GetName(Index: longint): string;
    function GetValue(const Name: string): string;
    procedure SetValue(const Name, Value: string);
    function GetValueFromIndex(Index: longint): string;
    procedure SetValueFromIndex(Index: longint; const Value: string);
    procedure SetTextStr(const Value: string);
  protected
    function GetTextStr: string; virtual;
    procedure Error(const Msg: string; Data: longint);
    function Get(Index: longint): string; virtual; abstract;
    function GetCapacity: longint; virtual;
    function GetCount: longint; virtual; abstract;
    function GetObject(Index: longint): TObject; virtual;
    procedure Put(Index: longint; const S: string); virtual;
    procedure PutObject(Index: longint; AObject: TObject); virtual;
    procedure SetCapacity(NewCapacity: longint); virtual;
    function DoCompareText(const s1, s2: string): longint; virtual;
    procedure SetUpdateState(Updating: boolean); virtual;
    property UpdateCount: longint read FUpdateCount;
  public
    function Add(const S: string): longint; virtual;
    function AddObject(const S: string; AObject: TObject): longint; virtual;
    function AddPair(const Name, Value: string): longint;
    procedure Append(const S: string);
    procedure AddStrings(TheStrings: TStrings); overload; virtual;
    procedure AddStrings(const TheStrings: array of string); overload;
    procedure Assign(Source: TPersistent); override;
    procedure BeginUpdate;
    procedure EndUpdate;
    procedure Clear; virtual; abstract;
    procedure Delete(Index: longint); virtual; abstract;
    function Equals(TheStrings: TStrings): boolean;
    procedure Exchange(Index1, Index2: longint); virtual;
    function IndexOf(const S: string): longint; virtual;
    function IndexOfName(const Name: string): longint; virtual;
    function IndexOfObject(AObject: TObject): longint; virtual;
    procedure Insert(Index: longint; const S: string); virtual; abstract;
    procedure InsertObject(Index: longint; const S: string; AObject: TObject);
    procedure LoadFromFile(const FileName: string); virtual;
    procedure Move(CurIndex, NewIndex: longint); virtual;
    procedure SaveToFile(const FileName: string); virtual;
    property Capacity: longint read GetCapacity write SetCapacity;
    property CommaText: string read GetCommaText write SetCommaText;
    property Count: longint read GetCount;
    property DelimitedText: string read GetDelimitedText write SetDelimitedText;
    property Delimiter: char read GetDelimiter write SetDelimiter;
    property Names[Index: longint]: string read GetName;
    property NameValueSeparator: char read GetNameValueSeparator write SetNameValueSeparator;
    property Objects[Index: longint]: TObject read GetObject write PutObject;
    property QuoteChar: char read GetQuoteChar write SetQuoteChar;
    property StrictDelimiter: boolean read FStrictDelimiter write FStrictDelimiter;
    property Strings[Index: longint]: string read Get write Put; default;
    property Text: string read GetTextStr write SetTextStr;
    property ValueFromIndex[Index: longint]: string read GetValueFromIndex write SetValueFromIndex;
    property Values[const Name: string]: string read GetValue write SetValue;
  end;

  TStringItem = record
    FStr: string;
    FObject: TObject;
  end;
  TStringItemList = array[0..MaxListSize - 1] of TStringItem;
  PStringItemList = ^TStringItemList;

  TStringList = class;
  TStringListSortCompare = function(List: TStringList; Index1, Index2: longint): longint;

  TStringList = class(TStrings)
  private
    FList: PStringItemList;
    FCount, FCapacity: longint;
    FDuplicates: TDuplicates;
    FCaseSensitive, FSorted, FOwnsObjects: boolean;
    FOnChange, FOnChanging: TNotifyEvent;
    procedure CheckIndex(Index: longint);
    procedure Grow;
    procedure ClearFrom(Index: longint);
    procedure InsertItem(Index: longint; const S: string; AObject: TObject);
    procedure ExchangeItems(Index1, Index2: longint);
    procedure QuickSort(L, R: longint; Compare: TStringListSortCompare);
    procedure IntroSort(L, R, depth: longint; Compare: TStringListSortCompare);
    procedure InsertionSort(L, R: longint; Compare: TStringListSortCompare);
    procedure HeapSort(L, R: longint; Compare: TStringListSortCompare);
    procedure SetSorted(Value: boolean);
    procedure SetCaseSensitive(Value: boolean);
  protected
    function Get(Index: longint): string; override;
    function GetCapacity: longint; override;
    function GetCount: longint; override;
    function GetObject(Index: longint): TObject; override;
    procedure Put(Index: longint; const S: string); override;
    procedure PutObject(Index: longint; AObject: TObject); override;
    procedure SetCapacity(NewCapacity: longint); override;
    function DoCompareText(const s1, s2: string): longint; override;
    function GetTextStr: string; override;
    procedure SetUpdateState(Updating: boolean); override;
    procedure Changed; virtual;
    procedure Changing; virtual;
  public
    destructor Destroy; override;
    function Add(const S: string): longint; override;
    procedure Clear; override;
    procedure CustomSort(Compare: TStringListSortCompare);
    procedure Delete(Index: longint); override;
    procedure Exchange(Index1, Index2: longint); override;
    function Find(const S: string; var Index: longint): boolean; virtual;
    function IndexOf(const S: string): longint; override;
    function IndexOfName(const Name: string): longint; override;
    procedure Insert(Index: longint; const S: string); override;
    procedure Sort; virtual;
    property CaseSensitive: boolean read FCaseSensitive write SetCaseSensitive;
    property Duplicates: TDuplicates read FDuplicates write FDuplicates;
    property OnChange: TNotifyEvent read FOnChange write FOnChange;
    property OnChanging: TNotifyEvent read FOnChanging write FOnChanging;
    property OwnsObjects: boolean read FOwnsObjects write FOwnsObjects;
    property Sorted: boolean read FSorted write SetSorted;
  end;

{$ENDIF}

implementation

{$IFDEF P3_OBJFPC}

uses sysutils;

type
  { 16 bytes of a text at once (SIMD: SSE2, NEON) }
  TTextScan16 = vector[16] of byte;

{ The first LF or CR of p[i, n) (0-based), or n: 16 bytes at a time, then
  one by one }
function findLineBreak(p: PChar; i, n: longint): longint;
var
  v: TTextScan16;
  mask: QWord;
begin
  while i + 16 <= n do
  begin
    v := VectorLoad(TTextScan16, p + i);
    mask := VectorMask((v = 10) or (v = 13));
    if mask <> 0 then
    begin
      findLineBreak := i + CountTrailingZeros(mask);
      exit;
    end;
    i := i + 16;
  end;
  while (i < n) and (p[i] <> #10) and (p[i] <> #13) do
    i := i + 1;
  findLineBreak := i;
end;

const
  SListIndexError = 'List index (';
  SOutOfBounds = ') out of bounds';
  SCapacityError = 'List capacity (';
  SExceeded = ') exceeded.';
  SSortedListError = 'Operation not allowed on sorted list';
  SDuplicateString = 'String list does not allow duplicates';
  SFindNeedsSortedList = 'Cannot use find on unsorted list';

type
  TByteArray = array[0..high(longint) - 1] of char;
  PByteArray = ^TByteArray;

function c_fopen(name, mode: pchar): pointer; cdecl; external name 'fopen';
function c_fclose(f: pointer): longint; cdecl; external name 'fclose';
function c_fread(p: pointer; size, n: int64; f: pointer): int64; cdecl; external name 'fread';
function c_fwrite(p: pointer; size, n: int64; f: pointer): int64; cdecl; external name 'fwrite';
function sys_strerror: pchar; cdecl; external name '__pas_sys_strerror';

{ The methods named Move and Delete hide the system's routines. }
procedure moveBytes(src, dst: pointer; n: int64);
begin
  if n > 0 then
    move(PByteArray(src)^[0], PByteArray(dst)^[0], n);
end;

{ TPersistent }

procedure TPersistent.AssignError(Source: TPersistent);
var
  name: string;
begin
  if Source = nil then
    name := 'Nil'
  else
    name := Source.ClassName;
  raise EConvertError.Create('Cannot assign a ' + name + ' to a ' + ClassName + '.');
end;

procedure TPersistent.AssignTo(Dest: TPersistent);
begin
  Dest.AssignError(Self);
end;

procedure TPersistent.Assign(Source: TPersistent);
begin
  if Source <> nil then
    Source.AssignTo(Self)
  else
    AssignError(nil);
end;

{ TList }

destructor TList.Destroy;
begin
  Clear;
  freemem(FList);
  inherited Destroy;
end;

procedure TList.Notify(Ptr: pointer; Action: TListNotification);
begin
end;

procedure TList.CheckIndex(Index: longint);
begin
  if (Index < 0) or (Index >= FCount) then
    raise EListError.Create(SListIndexError + IntToStr(Index) + SOutOfBounds);
end;

procedure TList.SetCapacity(NewCapacity: longint);
begin
  if (NewCapacity < FCount) or (NewCapacity > MaxListSize) then
    raise EListError.Create(SCapacityError + IntToStr(NewCapacity) + SExceeded);
  if NewCapacity = FCapacity then
    exit;
  reallocmem(FList, int64(NewCapacity) * sizeof(pointer));
  FCapacity := NewCapacity;
end;

procedure TList.Grow;
var
  step: longint;
begin
  if FCapacity > 127 then
    step := FCapacity shr 2
  else if FCapacity > 8 then
    step := 16
  else
    step := 4;
  SetCapacity(FCapacity + step);
end;

function TList.Expand: TList;
begin
  if FCount = FCapacity then
    Grow;
  Result := Self;
end;

procedure TList.SetCount(NewCount: longint);
var
  i: longint;
begin
  if NewCount > MaxListSize then
    raise EListError.Create('List count (' + IntToStr(NewCount) + SOutOfBounds);
  if NewCount > FCount then
  begin
    if NewCount > FCapacity then
      SetCapacity(NewCount);
    for i := FCount to NewCount - 1 do
      FList^[i] := nil;
    FCount := NewCount;
  end
  else
    for i := FCount - 1 downto NewCount do
      Delete(i);
end;

function TList.Get(Index: longint): pointer;
begin
  CheckIndex(Index);
  Result := FList^[Index];
end;

procedure TList.Put(Index: longint; Item: pointer);
var
  old: pointer;
begin
  CheckIndex(Index);
  old := FList^[Index];
  FList^[Index] := Item;
  if old <> nil then
    Notify(old, lnDeleted);
  if Item <> nil then
    Notify(Item, lnAdded);
end;

procedure TList.Insert(Index: longint; Item: pointer);
begin
  if (Index < 0) or (Index > FCount) then
    raise EListError.Create(SListIndexError + IntToStr(Index) + SOutOfBounds);
  if FCount = FCapacity then
    Grow;
  if Index < FCount then
    moveBytes(@FList^[Index], @FList^[Index + 1], int64(FCount - Index) * sizeof(pointer));
  FList^[Index] := Item;
  FCount := FCount + 1;
  if Item <> nil then
    Notify(Item, lnAdded);
end;

function TList.Add(Item: pointer): longint;
begin
  Result := FCount;
  Insert(FCount, Item);
end;

{ Takes the item out, without a notification. }
procedure TList.RemoveAt(Index: longint);
begin
  FCount := FCount - 1;
  if Index < FCount then
    moveBytes(@FList^[Index + 1], @FList^[Index], int64(FCount - Index) * sizeof(pointer));
  if (FCapacity > 256) and (FCount < FCapacity shr 2) then
    SetCapacity(FCapacity shr 1);
end;

procedure TList.Delete(Index: longint);
var
  item: pointer;
begin
  CheckIndex(Index);
  item := FList^[Index];
  RemoveAt(Index);
  if item <> nil then
    Notify(item, lnDeleted);
end;

{ From the last item down: each is taken out (Count is its index) and then
  notified, as Delete does; the capacity shrinks once at the end to what the
  halvings in RemoveAt would have left. }
procedure TList.Clear;
var
  item: pointer;
  cap: longint;
begin
  while FCount > 0 do
  begin
    FCount := FCount - 1;
    item := FList^[FCount];
    if item <> nil then
      Notify(item, lnDeleted);
  end;
  cap := FCapacity;
  while (cap > 256) and (FCount < cap shr 2) do
    cap := cap shr 1;
  SetCapacity(cap);
end;

procedure TList.Exchange(Index1, Index2: longint);
var
  t: pointer;
begin
  CheckIndex(Index1);
  CheckIndex(Index2);
  t := FList^[Index1];
  FList^[Index1] := FList^[Index2];
  FList^[Index2] := t;
end;

function TList.IndexOf(Item: pointer): longint;
var
  i: longint;
begin
  for i := 0 to FCount - 1 do
    if FList^[i] = Item then
      exit(i);
  Result := -1;
end;

function TList.Extract(Item: pointer): pointer;
var
  i: longint;
begin
  Result := nil;
  i := IndexOf(Item);
  if i >= 0 then
  begin
    Result := Item;
    RemoveAt(i);
    Notify(Item, lnExtracted);
  end;
end;

function TList.Remove(Item: pointer): longint;
begin
  Result := IndexOf(Item);
  if Result >= 0 then
    Delete(Result);
end;

function TList.First: pointer;
begin
  if FCount = 0 then
    Result := nil
  else
    Result := FList^[0];
end;

function TList.Last: pointer;
begin
  if FCount = 0 then
    Result := nil
  else
    Result := FList^[FCount - 1];
end;

procedure TList.Move(CurIndex, NewIndex: longint);
var
  item: pointer;
begin
  CheckIndex(CurIndex);
  CheckIndex(NewIndex);
  item := FList^[CurIndex];
  if NewIndex > CurIndex then
    moveBytes(@FList^[CurIndex + 1], @FList^[CurIndex], int64(NewIndex - CurIndex) * sizeof(pointer))
  else if NewIndex < CurIndex then
    moveBytes(@FList^[NewIndex], @FList^[NewIndex + 1], int64(CurIndex - NewIndex) * sizeof(pointer));
  FList^[NewIndex] := item;
end;

procedure TList.Pack;
var
  i, n: longint;
begin
  n := 0;
  for i := 0 to FCount - 1 do
    if FList^[i] <> nil then
    begin
      FList^[n] := FList^[i];
      n := n + 1;
    end;
  FCount := n;
end;

procedure quickSortPointers(List: PPointerList; L, R: longint; Compare: TListSortCompare);
var
  i, j: longint;
  p, t: pointer;
begin
  repeat
    i := L;
    j := R;
    p := List^[(L + R) div 2];
    repeat
      while Compare(p, List^[i]) > 0 do
        i := i + 1;
      while Compare(p, List^[j]) < 0 do
        j := j - 1;
      if i <= j then
      begin
        t := List^[i];
        List^[i] := List^[j];
        List^[j] := t;
        i := i + 1;
        j := j - 1;
      end;
    until i > j;
    if j - L < R - i then
    begin
      if L < j then
        quickSortPointers(List, L, j, Compare);
      L := i;
    end
    else
    begin
      if i < R then
        quickSortPointers(List, i, R, Compare);
      R := j;
    end;
  until L >= R;
end;

procedure TList.Sort(Compare: TListSortCompare);
begin
  if FCount > 1 then
    quickSortPointers(FList, 0, FCount - 1, Compare);
end;

{ TStrings }

procedure TStrings.Error(const Msg: string; Data: longint);
begin
  raise EStringListError.Create(Msg + IntToStr(Data) + SOutOfBounds);
end;

procedure TStrings.CheckSpecialChars;
begin
  if not FSpecialCharsInited then
  begin
    FQuoteChar := '"';
    FDelimiter := ',';
    FNameValueSeparator := '=';
    FSpecialCharsInited := true;
  end;
end;

function TStrings.GetDelimiter: char;
begin
  CheckSpecialChars;
  Result := FDelimiter;
end;

procedure TStrings.SetDelimiter(c: char);
begin
  CheckSpecialChars;
  FDelimiter := c;
end;

function TStrings.GetQuoteChar: char;
begin
  CheckSpecialChars;
  Result := FQuoteChar;
end;

procedure TStrings.SetQuoteChar(c: char);
begin
  CheckSpecialChars;
  FQuoteChar := c;
end;

function TStrings.GetNameValueSeparator: char;
begin
  CheckSpecialChars;
  Result := FNameValueSeparator;
end;

procedure TStrings.SetNameValueSeparator(c: char);
begin
  CheckSpecialChars;
  FNameValueSeparator := c;
end;

function TStrings.GetCapacity: longint;
begin
  Result := Count;
end;

procedure TStrings.SetCapacity(NewCapacity: longint);
begin
end;

function TStrings.GetObject(Index: longint): TObject;
begin
  Result := nil;
end;

procedure TStrings.PutObject(Index: longint; AObject: TObject);
begin
end;

procedure TStrings.Put(Index: longint; const S: string);
var
  obj: TObject;
begin
  obj := Objects[Index];
  Delete(Index);
  InsertObject(Index, S, obj);
end;

function TStrings.DoCompareText(const s1, s2: string): longint;
begin
  Result := CompareText(s1, s2);
end;

function TStrings.Add(const S: string): longint;
begin
  Result := Count;
  Insert(Result, S);
end;

function TStrings.AddObject(const S: string; AObject: TObject): longint;
begin
  Result := Add(S);
  Objects[Result] := AObject;
end;

function TStrings.AddPair(const Name, Value: string): longint;
begin
  CheckSpecialChars;
  Result := AddObject(Name + FNameValueSeparator + Value, nil);
end;

procedure TStrings.Append(const S: string);
begin
  Add(S);
end;

procedure TStrings.AddStrings(TheStrings: TStrings);
var
  i: longint;
begin
  BeginUpdate;
  try
    for i := 0 to TheStrings.Count - 1 do
      AddObject(TheStrings[i], TheStrings.Objects[i]);
  finally
    EndUpdate;
  end;
end;

procedure TStrings.AddStrings(const TheStrings: array of string);
var
  i: longint;
begin
  BeginUpdate;
  try
    for i := 0 to high(TheStrings) do
      Add(TheStrings[i]);
  finally
    EndUpdate;
  end;
end;

procedure TStrings.Assign(Source: TPersistent);
var
  from: TStrings;
begin
  if Source is TStrings then
  begin
    from := TStrings(Source);
    BeginUpdate;
    try
      Clear;
      AddStrings(from);
    finally
      EndUpdate;
    end;
    from.CheckSpecialChars;
    FSpecialCharsInited := true;
    FQuoteChar := from.FQuoteChar;
    FDelimiter := from.FDelimiter;
    FNameValueSeparator := from.FNameValueSeparator;
    FStrictDelimiter := from.FStrictDelimiter;
  end
  else
    inherited Assign(Source);
end;

procedure TStrings.SetUpdateState(Updating: boolean);
begin
end;

procedure TStrings.BeginUpdate;
begin
  if FUpdateCount = 0 then
    SetUpdateState(true);
  FUpdateCount := FUpdateCount + 1;
end;

procedure TStrings.EndUpdate;
begin
  if FUpdateCount > 0 then
    FUpdateCount := FUpdateCount - 1;
  if FUpdateCount = 0 then
    SetUpdateState(false);
end;

function TStrings.Equals(TheStrings: TStrings): boolean;
var
  i: longint;
begin
  Result := Count = TheStrings.Count;
  if Result then
    for i := 0 to Count - 1 do
      if Strings[i] <> TheStrings[i] then
        exit(false);
end;

procedure TStrings.Exchange(Index1, Index2: longint);
var
  obj: TObject;
  s: string;
begin
  BeginUpdate;
  try
    obj := Objects[Index1];
    s := Strings[Index1];
    Objects[Index1] := Objects[Index2];
    Strings[Index1] := Strings[Index2];
    Objects[Index2] := obj;
    Strings[Index2] := s;
  finally
    EndUpdate;
  end;
end;

function TStrings.IndexOf(const S: string): longint;
var
  i: longint;
begin
  for i := 0 to Count - 1 do
    if DoCompareText(Strings[i], S) = 0 then
      exit(i);
  Result := -1;
end;

function TStrings.IndexOfName(const Name: string): longint;
var
  i, p: longint;
  s: string;
begin
  CheckSpecialChars;
  for i := 0 to Count - 1 do
  begin
    s := Strings[i];
    p := pos(FNameValueSeparator, s);
    if (p > 0) and (DoCompareText(copy(s, 1, p - 1), Name) = 0) then
      exit(i);
  end;
  Result := -1;
end;

function TStrings.IndexOfObject(AObject: TObject): longint;
var
  i: longint;
begin
  for i := 0 to Count - 1 do
    if Objects[i] = AObject then
      exit(i);
  Result := -1;
end;

procedure TStrings.InsertObject(Index: longint; const S: string; AObject: TObject);
begin
  Insert(Index, S);
  Objects[Index] := AObject;
end;

procedure TStrings.Move(CurIndex, NewIndex: longint);
var
  obj: TObject;
  s: string;
begin
  if CurIndex <> NewIndex then
  begin
    BeginUpdate;
    try
      s := Strings[CurIndex];
      obj := Objects[CurIndex];
      Delete(CurIndex);
      InsertObject(NewIndex, s, obj);
    finally
      EndUpdate;
    end;
  end;
end;

function TStrings.GetName(Index: longint): string;
var
  s: string;
  p: longint;
begin
  CheckSpecialChars;
  s := Strings[Index];
  p := pos(FNameValueSeparator, s);
  if p > 0 then
    Result := copy(s, 1, p - 1)
  else
    Result := '';
end;

function TStrings.GetValueFromIndex(Index: longint): string;
var
  s: string;
  p: longint;
begin
  CheckSpecialChars;
  s := Strings[Index];
  p := pos(FNameValueSeparator, s);
  if p > 0 then
    Result := copy(s, p + 1, high(longint))
  else
    Result := s;
end;

procedure TStrings.SetValueFromIndex(Index: longint; const Value: string);
begin
  if Value = '' then
    Delete(Index)
  else
  begin
    if Index < 0 then
      Index := Add('');
    CheckSpecialChars;
    Strings[Index] := GetName(Index) + FNameValueSeparator + Value;
  end;
end;

function TStrings.GetValue(const Name: string): string;
var
  i: longint;
begin
  i := IndexOfName(Name);
  if i < 0 then
    Result := ''
  else
    Result := GetValueFromIndex(i);
end;

procedure TStrings.SetValue(const Name, Value: string);
var
  i: longint;
begin
  CheckSpecialChars;
  i := IndexOfName(Name);
  if i < 0 then
    Add(Name + FNameValueSeparator + Value)
  else
    Strings[i] := Name + FNameValueSeparator + Value;
end;

function TStrings.GetTextStr: string;
var
  i, n, total, le, p: longint;
  s, eol: string;
  pr, pe: PChar;
begin
  n := Count;
  Result := '';
  eol := LineEnding;
  le := length(eol);
  total := 0;
  for i := 0 to n - 1 do
    total := total + length(Strings[i]) + le;
  SetLength(Result, total);
  pr := PChar(Result);
  pe := PChar(eol);
  p := 1;
  for i := 0 to n - 1 do
  begin
    s := Strings[i];
    if length(s) > 0 then
      moveBytes(PChar(s), PChar(pr + p - 1), length(s));
    p := p + length(s);
    moveBytes(PChar(pe), PChar(pr + p - 1), le);
    p := p + le;
  end;
end;

{ Lines end with LF, CR or CR LF; a last end of line makes no empty line. }
procedure TStrings.SetTextStr(const Value: string);
var
  i, start: longint;
begin
  BeginUpdate;
  try
    Clear;
    i := 1;
    while i <= length(Value) do
    begin
      start := i;
      i := findLineBreak(PChar(Value), i - 1, length(Value)) + 1;
      Add(copy(Value, start, i - start));
      if (i <= length(Value)) and (Value[i] = #13) then
        i := i + 1;
      if (i <= length(Value)) and (Value[i] = #10) and
         ((i = 1) or (Value[i - 1] <> #13) or (i - 1 >= start)) then
        i := i + 1;
    end;
  finally
    EndUpdate;
  end;
end;

function TStrings.GetDelimited(d, q: char; strict: boolean): string;
var
  breaks: set of char;
  i, j: longint;
  s: string;
  doQuote: boolean;
begin
  if strict then
    breaks := [#0, q, d]
  else
    breaks := [#0..' ', q, d];
  Result := '';
  for i := 0 to Count - 1 do
  begin
    s := Strings[i];
    doQuote := false;
    j := 1;
    while (j <= length(s)) and not doQuote do
    begin
      doQuote := s[j] in breaks;
      j := j + 1;
    end;
    if doQuote then
      Result := Result + q + StringReplace(s, q, q + q, [rfReplaceAll]) + q
    else
      Result := Result + s;
    if i < Count - 1 then
      Result := Result + d;
  end;
  if (Result = '') and (Count = 1) then
    Result := q + q;
end;

procedure TStrings.SetDelimited(const AValue: string; d, q: char; strict: boolean);
var
  i, j, n: longint;
  notFirst: boolean;
begin
  BeginUpdate;
  try
  Clear;
  n := length(AValue);
  i := 1;
  notFirst := false;
  while i <= n do
  begin
    if notFirst and (AValue[i] = d) then
      i := i + 1;
    if not strict then
      while (i <= n) and (AValue[i] <= ' ') do
        i := i + 1;
    if i > n then
    begin
      if notFirst then
        Add('');
    end
    else if AValue[i] = q then
    begin
      j := i + 1;
      while (j <= n) and ((AValue[j] <> q) or ((j < n) and (AValue[j + 1] = q))) do
        if AValue[j] = q then
          j := j + 2
        else
          j := j + 1;
      Add(StringReplace(copy(AValue, i + 1, j - i - 1), q + q, q, [rfReplaceAll]));
      i := j + 1;
    end
    else
    begin
      j := i;
      while (j <= n) and (AValue[j] <> d) and (strict or (AValue[j] > ' ')) do
        j := j + 1;
      Add(copy(AValue, i, j - i));
      i := j;
    end;
    if not strict then
      while (i <= n) and (AValue[i] <= ' ') do
        i := i + 1;
    notFirst := true;
  end;
  finally
    EndUpdate;
  end;
end;

function TStrings.GetDelimitedText: string;
begin
  CheckSpecialChars;
  Result := GetDelimited(FDelimiter, FQuoteChar, FStrictDelimiter);
end;

procedure TStrings.SetDelimitedText(const AValue: string);
begin
  CheckSpecialChars;
  SetDelimited(AValue, FDelimiter, FQuoteChar, FStrictDelimiter);
end;

function TStrings.GetCommaText: string;
begin
  Result := GetDelimited(',', '"', false);
end;

procedure TStrings.SetCommaText(const AValue: string);
begin
  SetDelimited(AValue, ',', '"', false);
end;

{ Read in blocks (fgetc would take the file's lock for every character),
  each line ending with LF, CR or CR LF, as SetTextStr splits them: a last
  end of line makes no empty line. A line begun in one block and ended in
  a later one is carried in part. }
procedure TStrings.LoadFromFile(const FileName: string);
const
  BlockSize = 65536;
var
  f: pointer;
  buf: PChar;
  got, i, start: int64;
  part: string;
  pendingLine, afterCR: boolean;
  c: char;

  { buf[start .. upto - 1], after what part holds }
  function piece(upto: int64): string;
  var
    n: int64;
  begin
    n := upto - start;
    SetLength(Result, n);
    if n > 0 then
      moveBytes(@buf[start], @Result[1], n);
    if part <> '' then
      Result := part + Result;
  end;

begin
  f := c_fopen(pchar(FileName), 'rb');
  if f = nil then
    raise EFOpenError.Create('Unable to open file "' + FileName + '": ' + StrPas(sys_strerror));
  buf := nil;
  BeginUpdate;
  try
    Clear;
    getmem(buf, BlockSize);
    part := '';
    pendingLine := false;
    afterCR := false;
    got := c_fread(buf, 1, BlockSize, f);
    while got > 0 do
    begin
      start := 0;
      for i := 0 to got - 1 do
      begin
        c := buf[i];
        if (c = #10) and afterCR then
        begin
          afterCR := false;
          start := i + 1;
        end
        else if (c = #10) or (c = #13) then
        begin
          Add(piece(i));
          part := '';
          pendingLine := false;
          afterCR := c = #13;
          start := i + 1;
        end
        else
        begin
          afterCR := false;
          pendingLine := true;
        end;
      end;
      if start < got then
        part := piece(got);
      got := c_fread(buf, 1, BlockSize, f);
    end;
    if pendingLine then
      Add(part);
  finally
    freemem(buf);
    c_fclose(f);
    EndUpdate;
  end;
end;

{ The lines are joined into one block of 64 KiB (from getmem, a task's
  coroutine stack may not hold it) and written whole: one c_fwrite for a
  full block, instead of a string and a locked write per line. The
  written bytes are the same: every line, then LineEnding
  (docs/classes-review.md CL-10). }
procedure TStrings.SaveToFile(const FileName: string);
const
  BlockSize = 65536;
var
  f: pointer;
  buf, src, pe: PChar;
  eol: string;
  i, at, len, le, n, j: longint;
  s: string;
begin
  f := c_fopen(pchar(FileName), 'wb');
  if f = nil then
    raise EFCreateError.Create('Unable to create file "' + FileName + '": ' + StrPas(sys_strerror));
  buf := nil;
  try
    getmem(buf, BlockSize);
    eol := LineEnding;
    le := length(eol);
    pe := PChar(eol);
    at := 0;
    for i := 0 to Count - 1 do
    begin
      s := Strings[i];
      len := length(s);
      src := PChar(s);
      j := 0;
      while j < len do
      begin
        n := BlockSize - at;
        if n > len - j then
          n := len - j;
        moveBytes(PChar(src + j), PChar(buf + at), n);
        at := at + n;
        j := j + n;
        if at = BlockSize then
        begin
          c_fwrite(buf, 1, BlockSize, f);
          at := 0;
        end;
      end;
      for j := 1 to le do
      begin
        buf[at] := pe[j - 1];
        at := at + 1;
        if at = BlockSize then
        begin
          c_fwrite(buf, 1, BlockSize, f);
          at := 0;
        end;
      end;
    end;
    if at > 0 then
      c_fwrite(buf, 1, at, f);
  finally
    freemem(buf);
    c_fclose(f);
  end;
end;

{ TStringList }

destructor TStringList.Destroy;
begin
  ClearFrom(0);
  freemem(FList);
  inherited Destroy;
end;

procedure TStringList.CheckIndex(Index: longint);
begin
  if (Index < 0) or (Index >= FCount) then
    Error(SListIndexError, Index);
end;

{ Frees the items from Index on. }
procedure TStringList.ClearFrom(Index: longint);
var
  i: longint;
begin
  for i := Index to FCount - 1 do
  begin
    FList^[i].FStr := '';
    if FOwnsObjects then
      FList^[i].FObject.Free;
  end;
  if Index < FCount then
    FCount := Index;
end;

procedure TStringList.SetCapacity(NewCapacity: longint);
begin
  if (NewCapacity < 0) or (NewCapacity > MaxListSize) then
    raise EStringListError.Create(SCapacityError + IntToStr(NewCapacity) + SExceeded);
  if NewCapacity < FCount then
    ClearFrom(NewCapacity);
  reallocmem(FList, int64(NewCapacity) * sizeof(TStringItem));
  FCapacity := NewCapacity;
end;

procedure TStringList.Grow;
begin
  if FCapacity >= 256 then
    SetCapacity(FCapacity + FCapacity div 4)
  else if FCapacity = 0 then
    SetCapacity(4)
  else
    SetCapacity(FCapacity * 4);
end;

function TStringList.GetCapacity: longint;
begin
  Result := FCapacity;
end;

function TStringList.GetCount: longint;
begin
  Result := FCount;
end;

function TStringList.Get(Index: longint): string;
begin
  CheckIndex(Index);
  Result := FList^[Index].FStr;
end;

procedure TStringList.Put(Index: longint; const S: string);
begin
  if FSorted then
    raise EStringListError.Create(SSortedListError);
  CheckIndex(Index);
  Changing;
  FList^[Index].FStr := S;
  Changed;
end;

function TStringList.GetObject(Index: longint): TObject;
begin
  CheckIndex(Index);
  Result := FList^[Index].FObject;
end;

procedure TStringList.PutObject(Index: longint; AObject: TObject);
begin
  CheckIndex(Index);
  Changing;
  FList^[Index].FObject := AObject;
  Changed;
end;

function TStringList.DoCompareText(const s1, s2: string): longint;
begin
  if FCaseSensitive then
    Result := CompareStr(s1, s2)
  else
    Result := CompareText(s1, s2);
end;

procedure TStringList.Changed;
begin
  if (FUpdateCount = 0) and Assigned(FOnChange) then
    FOnChange(Self);
end;

procedure TStringList.Changing;
begin
  if (FUpdateCount = 0) and Assigned(FOnChanging) then
    FOnChanging(Self);
end;

procedure TStringList.SetUpdateState(Updating: boolean);
begin
  if Updating then
    Changing
  else
    Changed;
end;

procedure TStringList.InsertItem(Index: longint; const S: string; AObject: TObject);
begin
  Changing;
  if FCount = FCapacity then
    Grow;
  if Index < FCount then
    moveBytes(@FList^[Index], @FList^[Index + 1], int64(FCount - Index) * sizeof(TStringItem));
  { the slot holds a moved or never-used item: it owns nothing }
  fillchar(FList^[Index], sizeof(TStringItem), 0);
  FList^[Index].FStr := S;
  FList^[Index].FObject := AObject;
  FCount := FCount + 1;
  Changed;
end;

function TStringList.Find(const S: string; var Index: longint): boolean;
var
  L, R, I, c: longint;
begin
  Result := false;
  Index := -1;
  if not FSorted then
    raise EListError.Create(SFindNeedsSortedList);
  L := 0;
  R := FCount - 1;
  while L <= R do
  begin
    I := L + (R - L) div 2;
    c := DoCompareText(S, FList^[I].FStr);
    if c > 0 then
      L := I + 1
    else
    begin
      R := I - 1;
      if c = 0 then
      begin
        Result := true;
        if FDuplicates <> dupAccept then
          L := I;
      end;
    end;
  end;
  Index := L;
end;

{ Unsorted: a scan of the items themselves, not of Strings[i] (a virtual
  call and a copy of each string, as TStrings.IndexOf does). }
function TStringList.IndexOf(const S: string): longint;
var
  i: longint;
begin
  if not FSorted then
  begin
    for i := 0 to FCount - 1 do
      if DoCompareText(FList^[i].FStr, S) = 0 then
        exit(i);
    Result := -1;
  end
  else if not Find(S, Result) then
    Result := -1;
end;

{ Scans the items themselves (not Strings[i]). A TStringList itself
  compares in place: a name is Name when the separator first comes right
  after length(Name) characters and those are Name (case-sensitively or
  not), which is when CompareStr / CompareText of copy(s, 1, p - 1) and
  Name is 0, without the copy. A descendant may override DoCompareText:
  it gets the copy, as in TStrings. }
{ The lines are added up and copied in place (docs/classes-review.md CL-3),
  now through PChars made once (CL-8); a descendant takes TStrings' way,
  through Strings[i]. }
function TStringList.GetTextStr: string;
var
  i, total, le, p, len: longint;
  eol: string;
  pr, pe: PChar;
begin
  if ClassType <> TStringList then
    exit(inherited GetTextStr);
  eol := LineEnding;
  le := length(eol);
  total := 0;
  for i := 0 to FCount - 1 do
    total := total + length(FList^[i].FStr) + le;
  SetLength(Result, total);
  pr := PChar(Result);
  pe := PChar(eol);
  p := 1;
  for i := 0 to FCount - 1 do
  begin
    len := length(FList^[i].FStr);
    if len > 0 then
      moveBytes(PChar(FList^[i].FStr), PChar(pr + p - 1), len);
    p := p + len;
    moveBytes(PChar(pe), PChar(pr + p - 1), le);
    p := p + le;
  end;
end;

function TStringList.IndexOfName(const Name: string): longint;
var
  i, k, n: longint;
  sep, a, b: char;
  same: boolean;
begin
  CheckSpecialChars;
  sep := FNameValueSeparator;
  if ClassType <> TStringList then
  begin
    for i := 0 to FCount - 1 do
    begin
      k := pos(sep, FList^[i].FStr);
      if (k > 0) and (DoCompareText(copy(FList^[i].FStr, 1, k - 1), Name) = 0) then
        exit(i);
    end;
    exit(-1);
  end;
  n := length(Name);
  for i := 0 to FCount - 1 do
    if (length(FList^[i].FStr) > n) and (FList^[i].FStr[n + 1] = sep) then
    begin
      same := true;
      for k := 1 to n do
      begin
        a := FList^[i].FStr[k];
        b := Name[k];
        if a = sep then
          same := false
        else if a <> b then
        begin
          if not FCaseSensitive then
          begin
            if (a >= 'A') and (a <= 'Z') then
              a := chr(ord(a) + 32);
            if (b >= 'A') and (b <= 'Z') then
              b := chr(ord(b) + 32);
          end;
          if a <> b then
            same := false;
        end;
        if not same then
          break;
      end;
      if same then
        exit(i);
    end;
  Result := -1;
end;

function TStringList.Add(const S: string): longint;
begin
  Result := FCount;
  if FSorted and Find(S, Result) then
    case FDuplicates of
      dupIgnore: exit;
      dupError: raise EStringListError.Create(SDuplicateString);
      dupAccept: ;
    end;
  InsertItem(Result, S, nil);
end;

procedure TStringList.Insert(Index: longint; const S: string);
begin
  if FSorted then
    raise EStringListError.Create(SSortedListError);
  if (Index < 0) or (Index > FCount) then
    Error(SListIndexError, Index);
  InsertItem(Index, S, nil);
end;

procedure TStringList.Delete(Index: longint);
begin
  CheckIndex(Index);
  Changing;
  FList^[Index].FStr := '';
  if FOwnsObjects then
    FList^[Index].FObject.Free;
  FCount := FCount - 1;
  if Index < FCount then
    moveBytes(@FList^[Index + 1], @FList^[Index], int64(FCount - Index) * sizeof(TStringItem));
  Changed;
end;

procedure TStringList.Clear;
begin
  if FCount = 0 then
  begin
    SetCapacity(0);
    exit;
  end;
  Changing;
  ClearFrom(0);
  SetCapacity(0);
  Changed;
end;

{ The two items' bytes trade places: each slot's string moves, whole, to
  the other, so no reference count changes (a TStringItem copy would add
  and release references three times). }
procedure TStringList.ExchangeItems(Index1, Index2: longint);
var
  t: record { TStringItem's layout, uncounted }
    FStr, FObject: pointer;
  end;
begin
  moveBytes(@FList^[Index1], @t, sizeof(TStringItem));
  moveBytes(@FList^[Index2], @FList^[Index1], sizeof(TStringItem));
  moveBytes(@t, @FList^[Index2], sizeof(TStringItem));
end;

procedure TStringList.Exchange(Index1, Index2: longint);
begin
  CheckIndex(Index1);
  CheckIndex(Index2);
  Changing;
  ExchangeItems(Index1, Index2);
  Changed;
end;

{ Items L..R in order. Hoare's partition around the first item (the median of
  three moved there), stopping on equal items, so many equal keys split the
  range in halves (the old partition made them O(n^2)); the larger side is
  looped on, the smaller recursed into; ranges of 12 or fewer are sorted by
  insertion; past 2 log2(n) levels the range is heap sorted. }
procedure TStringList.QuickSort(L, R: longint; Compare: TStringListSortCompare);
var
  depth, n: longint;
begin
  n := R - L + 1;
  depth := 0;
  while n > 1 do
  begin
    depth := depth + 2;
    n := n shr 1;
  end;
  IntroSort(L, R, depth, Compare);
end;

procedure TStringList.InsertionSort(L, R: longint; Compare: TStringListSortCompare);
var
  i, j: longint;
begin
  for i := L + 1 to R do
  begin
    j := i;
    while (j > L) and (Compare(Self, j - 1, j) > 0) do
    begin
      ExchangeItems(j - 1, j);
      j := j - 1;
    end;
  end;
end;

procedure TStringList.HeapSort(L, R: longint; Compare: TStringListSortCompare);
var
  n, i, root, child: longint;

  procedure sift(start, count: longint);
  begin
    root := start;
    while true do
    begin
      child := 2 * root + 1;
      if child >= count then
        break;
      if (child + 1 < count) and (Compare(Self, L + child, L + child + 1) < 0) then
        child := child + 1;
      if Compare(Self, L + root, L + child) >= 0 then
        break;
      ExchangeItems(L + root, L + child);
      root := child;
    end;
  end;

begin
  n := R - L + 1;
  for i := (n - 2) div 2 downto 0 do
    sift(i, n);
  for i := n - 1 downto 1 do
  begin
    ExchangeItems(L, L + i);
    sift(0, i);
  end;
end;

procedure TStringList.IntroSort(L, R, depth: longint; Compare: TStringListSortCompare);
var
  m, i, j: longint;
begin
  while R - L > 12 do
  begin
    if depth = 0 then
    begin
      HeapSort(L, R, Compare);
      exit;
    end;
    depth := depth - 1;
    { the median of the first, middle and last items goes first }
    m := L + (R - L) div 2;
    if Compare(Self, m, L) < 0 then ExchangeItems(m, L);
    if Compare(Self, R, m) < 0 then
    begin
      ExchangeItems(R, m);
      if Compare(Self, m, L) < 0 then ExchangeItems(m, L);
    end;
    ExchangeItems(L, m);
    i := L;
    j := R + 1;
    while true do
    begin
      i := i + 1;
      while (i <= R) and (Compare(Self, i, L) < 0) do
        i := i + 1;
      j := j - 1;
      while (j > L) and (Compare(Self, j, L) > 0) do
        j := j - 1;
      if i >= j then
        break;
      ExchangeItems(i, j);
    end;
    ExchangeItems(L, j);
    if j - L < R - j then
    begin
      IntroSort(L, j - 1, depth, Compare);
      L := j + 1;
    end
    else
    begin
      IntroSort(j + 1, R, depth, Compare);
      R := j - 1;
    end;
  end;
  InsertionSort(L, R, Compare);
end;

procedure TStringList.CustomSort(Compare: TStringListSortCompare);
begin
  if (not FSorted) and (FCount > 1) then
  begin
    Changing;
    QuickSort(0, FCount - 1, Compare);
    Changed;
  end;
end;

function compareItems(List: TStringList; Index1, Index2: longint): longint;
begin
  Result := List.DoCompareText(List.FList^[Index1].FStr, List.FList^[Index2].FStr);
end;

procedure TStringList.Sort;
begin
  CustomSort(@compareItems);
end;

procedure TStringList.SetSorted(Value: boolean);
begin
  if FSorted <> Value then
  begin
    if Value then
      Sort;
    FSorted := Value;
  end;
end;

procedure TStringList.SetCaseSensitive(Value: boolean);
begin
  if FCaseSensitive <> Value then
  begin
    FCaseSensitive := Value;
    if FSorted then
    begin
      FSorted := false;
      Sort;
      FSorted := true;
    end;
  end;
end;

{$ENDIF}

end.
