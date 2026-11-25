{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
(* netrouter — HTTP request routing in the style of Go's chi: routes by
  method and path pattern, URL parameters, middleware chains, groups,
  sub-routers. Routing only: it never parses a request nor writes a
  response. The program reads the request however it likes, hands the
  router the method, the path (without the query string) and a context
  object of its own type, and the router calls the handler of the route,
  through its middleware, with that context and a TRouteContext (the URL
  parameters, the matched pattern). Needs --mode=objfpc.

    type
      TApp = specialize TRouter<TMyConn>;     { TMyConn: the program's own }
    var
      r: TApp;
    ...
      r := TApp.Create;
      r.Use(Logger);                            { every route, in order }
      r.Get('/', @Home);
      r.Get('/posts/{id:[0-9]+}', @ShowPost);   { URLParam('id') }
      r.Route('/admin', procedure(a: TApp) => begin
        a.Use(RequireAdmin);                    { this sub-router's routes }
        a.Get('/stats', @Stats);
      end);
      r.NotFound(@NotFoundPage);
      ...
      case r.Serve(method, path, conn) of ... end;

  Patterns. A pattern starts with '/' and is split at '/' into segments,
  each matching one segment of the path:
    posts            a literal segment
    {id}             a parameter: a non-empty run up to the segment's end
                     (or up to the next literal in a mixed segment)
    {id:[0-9]+}      a parameter whose value must match the regular
                     expression (whole value; one segment, never a '/')
    {name}.{ext}     a mixed segment: parameters between literals; each
                     parameter ends at the first place its next literal
                     fits (where its expression, if any, also matches)
    *                the last segment only: the rest of the path, slashes
                     included (URLParam('*'))
  At each level the router tries literal segments first, then segments
  with expressions, then plain parameters, then '*', and backtracks into
  the next candidate when a deeper level does not match. Paths are matched
  as given: no percent-decoding, no slash cleaning; "/a" and "/a/" differ.

  Expressions (for {name:...}): literals, '.', classes [a-z0-9_-] and
  [^...], \d \w \s \D \W \S and escaped characters, groups ( ) and (?: ),
  alternation |, quantifiers * + ? {n} {n,} {n,m}, anchors ^ $. The value
  must match as a whole. Bytes, not code points.

  Handlers and middleware are closures (reference to). Middleware is a
  function from the next handler to a handler, as in chi:

    function Logger(Next: TApp.THandler): TApp.THandler;
    begin
      Result := procedure(C: TMyConn; R: TRouteContext) => begin
        Writeln(R.Method, ' ', R.Path);
        Next(C, R);                             { or not: stop the chain }
      end;
    end;

  A router's Use middleware wraps every route registered on it (and on its
  groups and sub-routers), its NotFound and its MethodNotAllowed handlers;
  Use must come before the router's first route. Using(M) returns a group
  sharing the router's routes with M added for the routes registered
  through it; Group(fn) is the same with no new middleware; Route(prefix,
  fn) builds a sub-router under prefix; Mount(prefix, sub) mounts one
  built elsewhere. A sub-router without its own NotFound/MethodNotAllowed
  handler uses its parent's.

  Serving: Serve(method, path, ctx) runs the route's chain and says what
  happened (rrMatched, rrNotFound, rrMethodNotAllowed — then
  AllowedMethods lists the methods the path has). Without a NotFound or
  MethodNotAllowed handler nothing runs for those outcomes: the caller
  answers. Configure the router first, then serve it from as many tasks
  and threads as needed: serving changes nothing in it.

  Async work: handlers are plain closures; inside a task (a connection's,
  say) they wait with Wait(SomeAsync(...)) or netasync's waits, which park
  the task without blocking its thread. *)
unit netrouter;

{$H+}

interface

{$IFNDEF P3_OBJFPC}
uses netrouter_needs_mode_objfpc;
{$ELSE}

uses sysutils;

type
  ERouterError = class(Exception);

  TRouteResult = (rrMatched, rrNotFound, rrMethodNotAllowed);

  { What the router found for a request: the URL parameters, the pattern
    of the matched route, the allowed methods of a 405. The handlers get
    it as their second argument. A caller may keep one per connection and
    pass it to Serve (it is reset there) to save an allocation per request;
    it is not shared between requests served at the same time. }
  TRouteContext = class
  private
    FPath: string;
    FMethod: string;
    FNames: array of string;
    FStarts, FLens: array of Integer;   { into FPath, 1-based starts }
    FCount: Integer;
    FPatterns: array of string;         { per router level, see RoutePattern }
    FNPat: Integer;
    FAllowed: string;
    FOutcome: TRouteResult;
  public
    procedure Reset;
    { the value of URL parameter Name ('' if the route has none such) }
    function URLParam(const Name: string): string;
    function HasParam(const Name: string; out Value: string): Boolean;
    function ParamCount: Integer;
    function ParamName(I: Integer): string;
    function ParamValue(I: Integer): string;
    (* the matched route's pattern, sub-routers' joined: '/api/posts/{id}' *)
    function RoutePattern: string;
    { (internal: for TRouter) }
    procedure Start(const AMethod, APath: string);
    procedure Finish(AOutcome: TRouteResult; const Allowed: string);
    procedure PushParam(const Name: string; Start, Len: Integer);
    procedure PopParams(ToCount: Integer);
    procedure PushPattern(const Pattern: string);
    function LastParamStart: Integer;
    property Method: string read FMethod;
    property Path: string read FPath;
    { for rrMethodNotAllowed: the path's methods, as 'GET, POST' }
    property AllowedMethods: string read FAllowed;
    property Outcome: TRouteResult read FOutcome;
  end;

  { ---- the machinery behind TRouter (public because the generic uses it:
    not meant to be used directly) ---- }

  { A compiled regular expression (whole-value matching). }
  TRxNodeKind = (rxChar, rxAny, rxSet, rxGroup, rxBol, rxEol);
  TRxNode = record
    Kind: TRxNodeKind;
    Chars: set of AnsiChar;      { rxChar (one member) and rxSet }
    Min, Max: Integer;           { quantifier; Max -1 = unbounded }
    FirstAlt, NAlt: Integer;     { rxGroup: its alternatives in Seqs }
  end;
  TRxSeq = array of Integer;     { node indexes }
  TRegex = class
  private
    FNodes: array of TRxNode;
    FSeqs: array of TRxSeq;
    FRoot: Integer;              { a group node: the whole expression }
    FSrc: string;
    FPos: Integer;
    function NewNode(Kind: TRxNodeKind): Integer;
    function ParseAlts: Integer;
    function ParseSeq: Integer;
    function ParseAtom(out Node: Integer): Boolean;
    procedure ParseQuant(Node: Integer);
    function ParseClass: TRxNode;
    procedure Fail(const Msg: string);
  public
    constructor Create(const Expr: string);
    function Matches(P: PAnsiChar; Len: Integer): Boolean;
    property Source: string read FSrc;
  end;

  { One piece of a segment pattern. }
  TSegPartKind = (spLit, spParam);
  TSegPart = record
    Kind: TSegPartKind;
    Text: string;                { spLit: the literal; spParam: the name }
    Rx: TRegex;                  { spParam: the expression, or nil }
  end;
  TSegParts = array of TSegPart;

  TRouteNode = class;
  TRouteEdge = record
    Key: string;                 { literal: the segment; pattern: canonical form }
    Parts: TSegParts;            { pattern edges }
    HasRx: Boolean;
    Node: TRouteNode;
  end;

  { A level of the route tree: one path segment deeper than its parent. }
  TRouteNode = class
  public
    Statics: array of TRouteEdge;
    { a node with many literal edges finds one by a table (open addressing,
      edge index + 1, 0 empty), kept up to date as edges are added }
    StaticSlots: array of Integer;
    Patterns: array of TRouteEdge;   { expressions first, then plain }
    CatchAll: TRouteNode;
    Methods: array of string;        { the endpoints here: method ('*' any) }
    Endpoints: array of Integer;     { ... and the router's endpoint index }
    destructor Destroy; override;
    function EndpointFor(const Method: string): Integer;
    function AllowedList: string;
    procedure AddStatic(const Edge: TRouteEdge);
    { the literal edge for the L bytes at P, or -1 }
    function FindStatic(P: PAnsiChar; L: Integer): Integer;
  end;

  { The tree: patterns in, endpoint indexes out. }
  TRouteTree = class
  private
    FRoot: TRouteNode;
    function MatchFrom(Node: TRouteNode; const Path: string; SegStart: Integer;
                       Ctx: TRouteContext; const Method: string;
                       var NotAllowedAt: TRouteNode): Integer;
    function LeafEndpoint(Node: TRouteNode; Ctx: TRouteContext; const Method: string;
                          var NotAllowedAt: TRouteNode): Integer;
  public
    constructor Create;
    destructor Destroy; override;
    { the node of Pattern (made as needed); raises ERouterError }
    function NodeOf(const Pattern: string): TRouteNode;
    { the endpoint for Method on the path from From (a '/', or past the end
      for "/"), -1 if none: then NotAllowedAt is a node whose path matched
      but not its methods, or nil }
    function Find(const Path: string; From: Integer; Ctx: TRouteContext;
                  const Method: string; out NotAllowedAt: TRouteNode): Integer;
  end;

  { ---- the router ---- }

  generic TRouter<TCtx> = class
  public type
    THandler = reference to procedure(Ctx: TCtx; Route: TRouteContext);
    TMiddleware = reference to function(Next: THandler): THandler;
    TRouterProc = reference to procedure(R: TRouter);
    TWalkProc = reference to procedure(const Method, Pattern: string);
  private type
    TEndpoint = record
      Handler: THandler;           { wrapped in its middleware }
      Method: string;
      Pattern: string;
      Sub: TRouter;                { a mount: the sub-router }
    end;
  private
    FOwner: TRouter;               { the router whose tree this one shares (groups) }
    FTree: TRouteTree;             { nil in a group: the owner's }
    FEnds: array of TEndpoint;     { the owner's }
    FMws: array of TMiddleware;
    FHasRoutes: Boolean;
    FNotFound, FNotAllowed: THandler;
    FParent: TRouter;              { the router this one is mounted on }
    FOwned: array of TRouter;      { groups and Route sub-routers, freed with it }
    function Root: TRouter;
    function Wrap(const H: THandler): THandler;
    procedure AddEndpoint(const AMethod, Pattern: string; const H: THandler; Sub: TRouter);
    function FallbackNotFound: THandler;
    function FallbackNotAllowed: THandler;
    procedure ServeAt(Ctx: TCtx; Route: TRouteContext; From: Integer);
    procedure WalkPrefix(const Prefix: string; const Fn: TWalkProc);
  public
    constructor Create;
    destructor Destroy; override;
    { middleware for every route of this router (and its groups and
      sub-routers), before its first route }
    procedure Use(const M: TMiddleware);
    { a group sharing this router's routes, with M as well }
    function Using(const M: TMiddleware): TRouter;
    { Fn gets a group of this router (its routes are this router's) }
    procedure Group(const Fn: TRouterProc);
    { a new sub-router under Prefix, built by Fn }
    procedure Route(const Prefix: string; const Fn: TRouterProc);
    { Sub answers every path under Prefix (Prefix itself as '/'); the caller
      keeps owning Sub }
    procedure Mount(const Prefix: string; Sub: TRouter);
    { routes: Method is the request method as sent ('GET'); Handle takes
      every method }
    procedure Handle(const Pattern: string; const H: THandler);
    procedure Method(const AMethod, Pattern: string; const H: THandler);
    procedure Get(const Pattern: string; const H: THandler);
    procedure Post(const Pattern: string; const H: THandler);
    procedure Put(const Pattern: string; const H: THandler);
    procedure Patch(const Pattern: string; const H: THandler);
    procedure Delete(const Pattern: string; const H: THandler);
    procedure Head(const Pattern: string; const H: THandler);
    procedure Options(const Pattern: string; const H: THandler);
    { for paths with no route / for a path without a route for the method
      (Route.AllowedMethods lists those it has) }
    procedure NotFound(const H: THandler);
    procedure MethodNotAllowed(const H: THandler);
    { routes Method + Path (without the query string) with Ctx: runs the
      route's handler, or the NotFound / MethodNotAllowed one if set; what
      happened. Route: a context to reuse (reset here), or nil for one of
      its own. }
    function Serve(const AMethod, Path: string; Ctx: TCtx;
                   Route: TRouteContext = nil): TRouteResult;
    { every route, sub-routers' included: Fn(method, full pattern) }
    procedure Walk(const Fn: TWalkProc);
  end;

{$ENDIF}

implementation

{$IFDEF P3_OBJFPC}

{ ---- TRouteContext ---- }

procedure TRouteContext.Reset;
begin
  FPath := '';
  FMethod := '';
  FCount := 0;
  FNPat := 0;
  FAllowed := '';
  FOutcome := rrNotFound;
end;

procedure TRouteContext.Start(const AMethod, APath: string);
begin
  Reset;
  FMethod := AMethod;
  FPath := APath;
end;

procedure TRouteContext.Finish(AOutcome: TRouteResult; const Allowed: string);
begin
  FOutcome := AOutcome;
  FAllowed := Allowed;
end;

procedure TRouteContext.PushParam(const Name: string; Start, Len: Integer);
begin
  if FCount >= Length(FNames) then
  begin
    SetLength(FNames, FCount + 8);
    SetLength(FStarts, FCount + 8);
    SetLength(FLens, FCount + 8);
  end;
  FNames[FCount] := Name;
  FStarts[FCount] := Start;
  FLens[FCount] := Len;
  inc(FCount);
end;

procedure TRouteContext.PopParams(ToCount: Integer);
begin
  if ToCount < FCount then
    FCount := ToCount;
end;

procedure TRouteContext.PushPattern(const Pattern: string);
begin
  if FNPat >= Length(FPatterns) then
    SetLength(FPatterns, FNPat + 4);
  FPatterns[FNPat] := Pattern;
  inc(FNPat);
end;

function TRouteContext.LastParamStart: Integer;
begin
  if FCount = 0 then
    exit(-1);
  Result := FStarts[FCount - 1];
end;

function TRouteContext.URLParam(const Name: string): string;
begin
  if not HasParam(Name, Result) then
    Result := '';
end;

function TRouteContext.HasParam(const Name: string; out Value: string): Boolean;
var
  i: Integer;
begin
  { the innermost (latest) one wins: a sub-router's over its parent's }
  for i := FCount - 1 downto 0 do
    if FNames[i] = Name then
    begin
      Value := Copy(FPath, FStarts[i], FLens[i]);
      exit(true);
    end;
  Value := '';
  Result := false;
end;

function TRouteContext.ParamCount: Integer;
begin
  Result := FCount;
end;

function TRouteContext.ParamName(I: Integer): string;
begin
  Result := FNames[I];
end;

function TRouteContext.ParamValue(I: Integer): string;
begin
  Result := Copy(FPath, FStarts[I], FLens[I]);
end;

function TRouteContext.RoutePattern: string;
var
  i: Integer;
  p: string;
begin
  { a mount's pattern ends in '/*': the sub-router's pattern replaces it }
  Result := '';
  for i := 0 to FNPat - 1 do
  begin
    p := FPatterns[i];
    if (i < FNPat - 1) and (Length(p) >= 2) and (Copy(p, Length(p) - 1, 2) = '/*') then
      SetLength(p, Length(p) - 2);
    if (i > 0) and (p = '/') and (Result <> '') then
      continue;
    Result := Result + p;
  end;
end;

{ ---- TRegex: a backtracking matcher for the parameters' expressions ---- }

const
  RX_INF = -1;

type
  { what remains to match after a node: the rest of a sequence, or one
    more round of a repeated group }
  PRxCont = ^TRxCont;
  TRxCont = record
    Rep: Boolean;
    Seq, Idx: Integer;         { not Rep: Seqs[Seq] from Idx }
    Node, Count, Last: Integer;{ Rep: Nodes[Node] matched Count times, the last ending at Last }
    Next: PRxCont;
  end;

procedure TRegex.Fail(const Msg: string);
begin
  raise ERouterError.Create('netrouter: bad expression "' + FSrc + '": ' + Msg);
end;

function TRegex.NewNode(Kind: TRxNodeKind): Integer;
begin
  SetLength(FNodes, Length(FNodes) + 1);
  Result := High(FNodes);
  FNodes[Result].Kind := Kind;
  FNodes[Result].Chars := [];
  FNodes[Result].Min := 1;
  FNodes[Result].Max := 1;
  FNodes[Result].FirstAlt := 0;
  FNodes[Result].NAlt := 0;
end;

function EscapeSet(c: AnsiChar; out S: TRxNode): Boolean;
begin
  Result := true;
  S.Kind := rxSet;
  case c of
    'd': S.Chars := ['0'..'9'];
    'D': S.Chars := [#0..#255] - ['0'..'9'];
    'w': S.Chars := ['a'..'z', 'A'..'Z', '0'..'9', '_'];
    'W': S.Chars := [#0..#255] - ['a'..'z', 'A'..'Z', '0'..'9', '_'];
    's': S.Chars := [' ', #9, #10, #11, #12, #13];
    'S': S.Chars := [#0..#255] - [' ', #9, #10, #11, #12, #13];
  else
    Result := false;
  end;
end;

function TRegex.ParseClass: TRxNode;
var
  neg: Boolean;
  c, hi: AnsiChar;
  esc: TRxNode;
  first: Boolean;
begin
  { at '[' }
  inc(FPos);
  Result.Kind := rxSet;
  Result.Chars := [];
  neg := (FPos <= Length(FSrc)) and (FSrc[FPos] = '^');
  if neg then inc(FPos);
  first := true;
  while true do
  begin
    if FPos > Length(FSrc) then
      Fail('a [ without its ]');
    c := FSrc[FPos];
    if (c = ']') and not first then
    begin
      inc(FPos);
      break;
    end;
    first := false;
    inc(FPos);
    if c = '\' then
    begin
      if FPos > Length(FSrc) then
        Fail('a \ at the end');
      c := FSrc[FPos];
      inc(FPos);
      if EscapeSet(c, esc) then
      begin
        Result.Chars := Result.Chars + esc.Chars;
        continue;
      end;
    end;
    { a range c-hi (a '-' last or first is itself) }
    if (FPos + 1 <= Length(FSrc)) and (FSrc[FPos] = '-') and (FSrc[FPos + 1] <> ']') then
    begin
      hi := FSrc[FPos + 1];
      inc(FPos, 2);
      if hi = '\' then
      begin
        if FPos > Length(FSrc) then
          Fail('a \ at the end');
        hi := FSrc[FPos];
        inc(FPos);
      end;
      if hi < c then
        Fail('a range out of order');
      Result.Chars := Result.Chars + [c..hi];
    end
    else
      Result.Chars := Result.Chars + [c];
  end;
  if neg then
    Result.Chars := [#0..#255] - Result.Chars;
end;

function ReadInt(const S: string; var P: Integer; out V: Integer): Boolean;
begin
  V := 0;
  Result := false;
  while (P <= Length(S)) and (S[P] >= '0') and (S[P] <= '9') do
  begin
    V := V * 10 + (Ord(S[P]) - Ord('0'));
    if V > 100000 then
      exit(false);
    inc(P);
    Result := true;
  end;
end;

procedure TRegex.ParseQuant(Node: Integer);
var
  lo, hi, save: Integer;
begin
  if FPos > Length(FSrc) then
    exit;
  case FSrc[FPos] of
    '*': begin FNodes[Node].Min := 0; FNodes[Node].Max := RX_INF; inc(FPos); end;
    '+': begin FNodes[Node].Min := 1; FNodes[Node].Max := RX_INF; inc(FPos); end;
    '?': begin FNodes[Node].Min := 0; FNodes[Node].Max := 1; inc(FPos); end;
    '{':
      begin
        save := FPos;
        inc(FPos);
        if not ReadInt(FSrc, FPos, lo) then
        begin
          FPos := save;   { not a quantifier: a literal '{' }
          exit;
        end;
        hi := lo;
        if (FPos <= Length(FSrc)) and (FSrc[FPos] = ',') then
        begin
          inc(FPos);
          if not ReadInt(FSrc, FPos, hi) then
            hi := RX_INF;
        end;
        if (FPos > Length(FSrc)) or (FSrc[FPos] <> '}') then
          Fail('a {n,m} without its }');
        inc(FPos);
        if (hi <> RX_INF) and (hi < lo) then
          Fail('{n,m} with m < n');
        FNodes[Node].Min := lo;
        FNodes[Node].Max := hi;
      end;
  else
    exit;
  end;
  { a lazy '?' after it changes nothing for a whole-value match }
  if (FPos <= Length(FSrc)) and (FSrc[FPos] = '?') then
    inc(FPos);
  if (FPos <= Length(FSrc)) and (FSrc[FPos] in ['*', '+', '?']) then
    Fail('a quantifier after a quantifier');
end;

function TRegex.ParseAtom(out Node: Integer): Boolean;
var
  c: AnsiChar;
  s: TRxNode;
  g: Integer;
begin
  Node := -1;
  if FPos > Length(FSrc) then
    exit(false);
  c := FSrc[FPos];
  case c of
    '|', ')':
      exit(false);
    '(':
      begin
        inc(FPos);
        if (FPos + 1 <= Length(FSrc)) and (FSrc[FPos] = '?') and (FSrc[FPos + 1] = ':') then
          inc(FPos, 2);
        g := ParseAlts;
        if (FPos > Length(FSrc)) or (FSrc[FPos] <> ')') then
          Fail('a ( without its )');
        inc(FPos);
        Node := g;
      end;
    '[':
      begin
        s := ParseClass;
        Node := NewNode(rxSet);
        FNodes[Node].Chars := s.Chars;
      end;
    '.':
      begin
        inc(FPos);
        Node := NewNode(rxAny);
      end;
    '^':
      begin
        inc(FPos);
        Node := NewNode(rxBol);
        exit(true);    { (no quantifier) }
      end;
    '$':
      begin
        inc(FPos);
        Node := NewNode(rxEol);
        exit(true);
      end;
    '*', '+', '?':
      Fail('a quantifier with nothing before it');
    '\':
      begin
        inc(FPos);
        if FPos > Length(FSrc) then
          Fail('a \ at the end');
        c := FSrc[FPos];
        inc(FPos);
        if EscapeSet(c, s) then
        begin
          Node := NewNode(rxSet);
          FNodes[Node].Chars := s.Chars;
        end
        else
        begin
          Node := NewNode(rxChar);
          FNodes[Node].Chars := [c];
        end;
      end;
  else
    begin
      inc(FPos);
      Node := NewNode(rxChar);
      FNodes[Node].Chars := [c];
    end;
  end;
  ParseQuant(Node);
  Result := true;
end;

{ a sequence of atoms: its index in FSeqs }
function TRegex.ParseSeq: Integer;
var
  seq: TRxSeq;
  n: Integer;
begin
  SetLength(seq, 0);
  while ParseAtom(n) do
  begin
    SetLength(seq, Length(seq) + 1);
    seq[High(seq)] := n;
  end;
  SetLength(FSeqs, Length(FSeqs) + 1);
  Result := High(FSeqs);
  FSeqs[Result] := seq;
end;

{ alternatives a|b|c: a group node }
function TRegex.ParseAlts: Integer;
var
  alts: array of Integer;
  i, first: Integer;
begin
  SetLength(alts, 0);
  while true do
  begin
    SetLength(alts, Length(alts) + 1);
    alts[High(alts)] := ParseSeq;
    if (FPos <= Length(FSrc)) and (FSrc[FPos] = '|') then
      inc(FPos)
    else
      break;
  end;
  { the alternatives' sequences, consecutive in FSeqs }
  first := Length(FSeqs);
  SetLength(FSeqs, first + Length(alts));
  for i := 0 to High(alts) do
    FSeqs[first + i] := FSeqs[alts[i]];
  Result := NewNode(rxGroup);
  FNodes[Result].FirstAlt := first;
  FNodes[Result].NAlt := Length(alts);
end;

constructor TRegex.Create(const Expr: string);
begin
  inherited Create;
  FSrc := Expr;
  FPos := 1;
  FRoot := ParseAlts;
  if FPos <= Length(FSrc) then
    Fail('an unmatched ' + FSrc[FPos]);
end;

type
  TRxMatcher = record
    Rx: TRegex;
    P: PAnsiChar;
    Len: Integer;
    Steps: Integer;
  end;

function RxRun(var M: TRxMatcher; Seq, Idx, Pos: Integer; K: PRxCont): Boolean; forward;

{ Node matched Count times so far (the last ending at Last); at Pos: one more,
  greedily, or enough already and on with K }
function RxRep(var M: TRxMatcher; Node, Count, Last, Pos: Integer; K: PRxCont): Boolean;
var
  nd: ^TRxNode;
  a: Integer;
  c: TRxCont;
begin
  inc(M.Steps);
  if M.Steps > 100000 then
    exit(false);   { a pathological expression: give up on this value }
  nd := @M.Rx.FNodes[Node];
  if (nd^.Max = RX_INF) or (Count < nd^.Max) then
  begin
    { (a round that matched nothing does not repeat once Min is met) }
    if (Count < nd^.Min) or (Count = 0) or (Pos <> Last) then
      case nd^.Kind of
        rxChar, rxSet, rxAny:
          if (Pos < M.Len) and ((nd^.Kind = rxAny) or ((M.P + Pos)^ in nd^.Chars)) then
            if RxRep(M, Node, Count + 1, Pos, Pos + 1, K) then
              exit(true);
        rxGroup:
          begin
            c.Rep := true;
            c.Node := Node;
            c.Count := Count + 1;
            c.Last := Pos;
            c.Next := K;
            for a := 0 to nd^.NAlt - 1 do
              if RxRun(M, nd^.FirstAlt + a, 0, Pos, @c) then
                exit(true);
          end;
      end;
  end;
  if Count < nd^.Min then
    exit(false);
  { enough: what follows }
  if K = nil then
    exit(Pos = M.Len);
  if K^.Rep then
    Result := RxRep(M, K^.Node, K^.Count, K^.Last, Pos, K^.Next)
  else
    Result := RxRun(M, K^.Seq, K^.Idx, Pos, K^.Next);
end;

function RxRun(var M: TRxMatcher; Seq, Idx, Pos: Integer; K: PRxCont): Boolean;
var
  node: Integer;
  c: TRxCont;
begin
  if Idx >= Length(M.Rx.FSeqs[Seq]) then
  begin
    if K = nil then
      exit(Pos = M.Len);
    if K^.Rep then
      exit(RxRep(M, K^.Node, K^.Count, K^.Last, Pos, K^.Next));
    exit(RxRun(M, K^.Seq, K^.Idx, Pos, K^.Next));
  end;
  node := M.Rx.FSeqs[Seq][Idx];
  case M.Rx.FNodes[node].Kind of
    rxBol:
      if Pos <> 0 then exit(false) else exit(RxRun(M, Seq, Idx + 1, Pos, K));
    rxEol:
      if Pos <> M.Len then exit(false) else exit(RxRun(M, Seq, Idx + 1, Pos, K));
  end;
  c.Rep := false;
  c.Seq := Seq;
  c.Idx := Idx + 1;
  c.Next := K;
  Result := RxRep(M, node, 0, -1, Pos, @c);
end;

function TRegex.Matches(P: PAnsiChar; Len: Integer): Boolean;
var
  m: TRxMatcher;
begin
  m.Rx := Self;
  m.P := P;
  m.Len := Len;
  m.Steps := 0;
  Result := RxRep(m, FRoot, 0, -1, 0, nil);
end;

{ ---- segment patterns ---- }

function c_memcmp(A, B: Pointer; N: Int64): LongInt; cdecl; external 'c' name 'memcmp';

function SameBytes(A, B: PAnsiChar; N: Integer): Boolean;
begin
  Result := (N <= 0) or (c_memcmp(A, B, N) = 0);
end;

function IsParamName(const S: string): Boolean;
var
  i: Integer;
begin
  if S = '' then
    exit(false);
  for i := 1 to Length(S) do
    if not (S[i] in ['a'..'z', 'A'..'Z', '0'..'9', '_', '-', '.']) then
      exit(false);
  Result := true;
end;

{ one segment of a pattern into its parts; Key is its canonical form (the
  parameters' names left out), HasRx whether one has an expression }
procedure ParseSegment(const Pattern, Seg: string; out Parts: TSegParts;
                       out Key: string; out HasRx: Boolean);
var
  i, depth, j, colon: Integer;
  lit, inner, name, expr: string;
  part: TSegPart;

  procedure AddLit;
  begin
    if lit = '' then
      exit;
    part.Kind := spLit;
    part.Text := lit;
    part.Rx := nil;
    SetLength(Parts, Length(Parts) + 1);
    Parts[High(Parts)] := part;
    Key := Key + lit;
    lit := '';
  end;

begin
  SetLength(Parts, 0);
  Key := '';
  HasRx := false;
  lit := '';
  i := 1;
  while i <= Length(Seg) do
  begin
    if Seg[i] = '{' then
    begin
      depth := 1;
      j := i + 1;
      while (j <= Length(Seg)) and (depth > 0) do
      begin
        if Seg[j] = '\' then
          inc(j)
        else if Seg[j] = '{' then
          inc(depth)
        else if Seg[j] = '}' then
          dec(depth);
        inc(j);
      end;
      if depth <> 0 then
        raise ERouterError.Create('netrouter: "' + Pattern + '": a { without its }');
      inner := Copy(Seg, i + 1, j - i - 2);
      colon := Pos(':', inner);
      if colon > 0 then
      begin
        name := Copy(inner, 1, colon - 1);
        expr := Copy(inner, colon + 1, Length(inner));
      end
      else
      begin
        name := inner;
        expr := '';
      end;
      if not IsParamName(name) then
        raise ERouterError.Create('netrouter: "' + Pattern + '": bad parameter name "' + name + '"');
      AddLit;
      if (Length(Parts) > 0) and (Parts[High(Parts)].Kind = spParam) then
        raise ERouterError.Create('netrouter: "' + Pattern +
          '": two parameters with nothing between them');
      part.Kind := spParam;
      part.Text := name;
      part.Rx := nil;
      if expr <> '' then
      begin
        part.Rx := TRegex.Create(expr);
        HasRx := true;
      end;
      SetLength(Parts, Length(Parts) + 1);
      Parts[High(Parts)] := part;
      Key := Key + '{' + expr + '}';
      i := j;
    end
    else
    begin
      if Seg[i] = '}' then
        raise ERouterError.Create('netrouter: "' + Pattern + '": a } without its {');
      lit := lit + Seg[i];
      inc(i);
    end;
  end;
  AddLit;
end;

{ do Parts (from part Pi) match Path[S..E) (1-based S, E exclusive)?
  pushes the parameters (popped again on failure) }
function MatchParts(const Parts: TSegParts; Pi: Integer; const Path: string;
                    S, E: Integer; Ctx: TRouteContext): Boolean;
var
  part: ^TSegPart;
  nxt: string;
  at, save, L: Integer;
  p: PAnsiChar;
begin
  if Pi > High(Parts) then
    exit(S = E);
  part := @Parts[Pi];
  p := PAnsiChar(Path);
  if part^.Kind = spLit then
  begin
    L := Length(part^.Text);
    if (E - S < L) or not SameBytes(p + S - 1, PAnsiChar(part^.Text), L) then
      exit(false);
    exit(MatchParts(Parts, Pi + 1, Path, S + L, E, Ctx));
  end;
  save := Ctx.ParamCount;
  if Pi = High(Parts) then
  begin
    { the last part: the rest of the segment }
    if E - S < 1 then
      exit(false);
    if (part^.Rx <> nil) and not part^.Rx.Matches(p + S - 1, E - S) then
      exit(false);
    Ctx.PushParam(part^.Text, S, E - S);
    exit(true);
  end;
  { up to an occurrence of the next literal: the first that works }
  nxt := Parts[Pi + 1].Text;
  L := Length(nxt);
  at := S + 1;
  while at + L <= E do
  begin
    if ((L = 0) or ((p + at - 1)^ = nxt[1])) and SameBytes(p + at - 1, PAnsiChar(nxt), L) then
      if (part^.Rx = nil) or part^.Rx.Matches(p + S - 1, at - S) then
      begin
        Ctx.PushParam(part^.Text, S, at - S);
        if MatchParts(Parts, Pi + 1, Path, at, E, Ctx) then
          exit(true);
        Ctx.PopParams(save);
        if part^.Rx = nil then
          exit(false);   { chi: a plain parameter ends at the first one }
      end;
    inc(at);
  end;
  Result := false;
end;

{ ---- TRouteNode ---- }

destructor TRouteNode.Destroy;
var
  i, k: Integer;
begin
  for i := 0 to High(Statics) do
    Statics[i].Node.Free;
  for i := 0 to High(Patterns) do
  begin
    for k := 0 to High(Patterns[i].Parts) do
      Patterns[i].Parts[k].Rx.Free;
    Patterns[i].Node.Free;
  end;
  CatchAll.Free;
  inherited Destroy;
end;

{$Q-}{$R-}
function SegHash(P: PAnsiChar; L: Integer): Cardinal;
var
  i: Integer;
  h: Cardinal;
begin
  h := 2166136261;
  for i := 0 to L - 1 do
    h := (h xor Cardinal(Ord((P + i)^))) * 16777619;
  Result := h xor (h shr 15);
end;
{$Q+}{$R+}

procedure TRouteNode.AddStatic(const Edge: TRouteEdge);
var
  i, slot, mask, size: Integer;
begin
  SetLength(Statics, Length(Statics) + 1);
  Statics[High(Statics)] := Edge;
  if Length(Statics) < 8 then
    exit;
  if 2 * Length(Statics) > Length(StaticSlots) then
  begin
    size := 32;
    while size < 2 * Length(Statics) do
      size := size * 2;
    SetLength(StaticSlots, 0);
    SetLength(StaticSlots, size);
    for i := 0 to High(Statics) do
    begin
      mask := size - 1;
      slot := Integer(SegHash(PAnsiChar(Statics[i].Key), Length(Statics[i].Key)) and Cardinal(mask));
      while StaticSlots[slot] <> 0 do
        slot := (slot + 1) and mask;
      StaticSlots[slot] := i + 1;
    end;
  end
  else
  begin
    i := High(Statics);
    mask := Length(StaticSlots) - 1;
    slot := Integer(SegHash(PAnsiChar(Edge.Key), Length(Edge.Key)) and Cardinal(mask));
    while StaticSlots[slot] <> 0 do
      slot := (slot + 1) and mask;
    StaticSlots[slot] := i + 1;
  end;
end;

function TRouteNode.FindStatic(P: PAnsiChar; L: Integer): Integer;
var
  i, slot, mask: Integer;
begin
  if Length(StaticSlots) = 0 then
  begin
    for i := 0 to High(Statics) do
      if (Length(Statics[i].Key) = L) and
         ((L = 0) or SameBytes(P, PAnsiChar(Statics[i].Key), L)) then
        exit(i);
    exit(-1);
  end;
  mask := Length(StaticSlots) - 1;
  slot := Integer(SegHash(P, L) and Cardinal(mask));
  while StaticSlots[slot] <> 0 do
  begin
    i := StaticSlots[slot] - 1;
    if (Length(Statics[i].Key) = L) and
       ((L = 0) or SameBytes(P, PAnsiChar(Statics[i].Key), L)) then
      exit(i);
    slot := (slot + 1) and mask;
  end;
  Result := -1;
end;

function TRouteNode.EndpointFor(const Method: string): Integer;
var
  i, any: Integer;
begin
  any := -1;
  for i := 0 to High(Methods) do
    if Methods[i] = Method then
      exit(Endpoints[i])
    else if Methods[i] = '*' then
      any := Endpoints[i];
  Result := any;
end;

function TRouteNode.AllowedList: string;
var
  i: Integer;
begin
  Result := '';
  for i := 0 to High(Methods) do
  begin
    if Result <> '' then
      Result := Result + ', ';
    Result := Result + Methods[i];
  end;
end;

{ ---- TRouteTree ---- }

constructor TRouteTree.Create;
begin
  inherited Create;
  FRoot := TRouteNode.Create;
end;

destructor TRouteTree.Destroy;
begin
  FRoot.Free;
  inherited Destroy;
end;

function TRouteTree.NodeOf(const Pattern: string): TRouteNode;
var
  node: TRouteNode;
  rest, seg, key: string;
  slash, i, k: Integer;
  parts: TSegParts;
  hasRx, found: Boolean;
  edge: TRouteEdge;
begin
  if (Pattern = '') or (Pattern[1] <> '/') then
    raise ERouterError.Create('netrouter: a pattern starts with "/": "' + Pattern + '"');
  node := FRoot;
  rest := Copy(Pattern, 2, Length(Pattern));
  while true do
  begin
    slash := Pos('/', rest);
    (* (a '/' inside {...} belongs to the parameter's expression) *)
    if slash > 0 then
    begin
      k := 0;
      for i := 1 to Length(rest) do
      begin
        if rest[i] = '{' then inc(k)
        else if rest[i] = '}' then dec(k)
        else if (rest[i] = '/') and (k = 0) then
        begin
          slash := i;
          break;
        end;
        if i = Length(rest) then
          slash := 0;
      end;
    end;
    if slash > 0 then
    begin
      seg := Copy(rest, 1, slash - 1);
      rest := Copy(rest, slash + 1, Length(rest));
    end
    else
    begin
      seg := rest;
      rest := '';
    end;
    if seg = '*' then
    begin
      if slash > 0 then
        raise ERouterError.Create('netrouter: "*" is only the last segment: "' + Pattern + '"');
      if node.CatchAll = nil then
        node.CatchAll := TRouteNode.Create;
      exit(node.CatchAll);
    end;
    if (Pos('{', seg) = 0) and (Pos('}', seg) = 0) then
    begin
      found := false;
      for i := 0 to High(node.Statics) do
        if node.Statics[i].Key = seg then
        begin
          node := node.Statics[i].Node;
          found := true;
          break;
        end;
      if not found then
      begin
        edge.Key := seg;
        SetLength(edge.Parts, 0);
        edge.HasRx := false;
        edge.Node := TRouteNode.Create;
        node.AddStatic(edge);
        node := edge.Node;
      end;
    end
    else
    begin
      ParseSegment(Pattern, seg, parts, key, hasRx);
      found := false;
      for i := 0 to High(node.Patterns) do
        if node.Patterns[i].Key = key then
        begin
          { the same shape: the same parameter names, or a conflict }
          for k := 0 to High(parts) do
          begin
            if (parts[k].Kind = spParam) and
               (parts[k].Text <> node.Patterns[i].Parts[k].Text) then
              raise ERouterError.Create('netrouter: "' + Pattern + '": parameter "' +
                parts[k].Text + '" where another route has "' +
                node.Patterns[i].Parts[k].Text + '"');
            parts[k].Rx.Free;
          end;
          node := node.Patterns[i].Node;
          found := true;
          break;
        end;
      if not found then
      begin
        edge.Key := key;
        edge.Parts := parts;
        edge.HasRx := hasRx;
        edge.Node := TRouteNode.Create;
        { expressions before plain parameters: insert after the last one }
        SetLength(node.Patterns, Length(node.Patterns) + 1);
        k := High(node.Patterns);
        if hasRx then
          while (k > 0) and not node.Patterns[k - 1].HasRx do
          begin
            node.Patterns[k] := node.Patterns[k - 1];
            dec(k);
          end;
        node.Patterns[k] := edge;
        node := edge.Node;
      end;
    end;
    if slash = 0 then
      exit(node);
  end;
end;

function TRouteTree.LeafEndpoint(Node: TRouteNode; Ctx: TRouteContext;
                                 const Method: string; var NotAllowedAt: TRouteNode): Integer;
begin
  if Length(Node.Methods) = 0 then
    exit(-1);
  Result := Node.EndpointFor(Method);
  if (Result < 0) and (NotAllowedAt = nil) then
    NotAllowedAt := Node;
end;

{ the segment of Path starting at SegStart (1-based, just past a '/'),
  under Node }
function TRouteTree.MatchFrom(Node: TRouteNode; const Path: string; SegStart: Integer;
                              Ctx: TRouteContext; const Method: string;
                              var NotAllowedAt: TRouteNode): Integer;
var
  segEnd, n, i, save, L: Integer;
  p: PAnsiChar;
  last: Boolean;
  child: TRouteNode;
begin
  n := Length(Path);
  p := PAnsiChar(Path);
  segEnd := SegStart;
  while (segEnd <= n) and ((p + segEnd - 1)^ <> '/') do
    inc(segEnd);
  last := segEnd > n;
  L := segEnd - SegStart;
  save := Ctx.ParamCount;
  { literal segments }
  i := Node.FindStatic(p + SegStart - 1, L);
  if i >= 0 then
  begin
    child := Node.Statics[i].Node;
    if last then
      Result := LeafEndpoint(child, Ctx, Method, NotAllowedAt)
    else
      Result := MatchFrom(child, Path, segEnd + 1, Ctx, Method, NotAllowedAt);
    if Result >= 0 then
      exit;
  end;
  { parameters (expressions first) }
  for i := 0 to High(Node.Patterns) do
  begin
    if MatchParts(Node.Patterns[i].Parts, 0, Path, SegStart, segEnd, Ctx) then
    begin
      child := Node.Patterns[i].Node;
      if last then
        Result := LeafEndpoint(child, Ctx, Method, NotAllowedAt)
      else
        Result := MatchFrom(child, Path, segEnd + 1, Ctx, Method, NotAllowedAt);
      if Result >= 0 then
        exit;
      Ctx.PopParams(save);
    end;
  end;
  { the rest of the path }
  if Node.CatchAll <> nil then
  begin
    Ctx.PushParam('*', SegStart, n - SegStart + 1);
    Result := LeafEndpoint(Node.CatchAll, Ctx, Method, NotAllowedAt);
    if Result >= 0 then
      exit;
    Ctx.PopParams(save);
  end;
  Result := -1;
end;

function TRouteTree.Find(const Path: string; From: Integer; Ctx: TRouteContext;
                         const Method: string; out NotAllowedAt: TRouteNode): Integer;
begin
  NotAllowedAt := nil;
  if From > Length(Path) then
  begin
    { a sub-router mounted at exactly this path: its "/" }
    Result := -1;
    { the empty segment after "/" }
    if FRoot <> nil then
      Result := MatchFrom(FRoot, Path, Length(Path) + 1, Ctx, Method, NotAllowedAt);
    exit;
  end;
  if Path[From] <> '/' then
    exit(-1);
  Result := MatchFrom(FRoot, Path, From + 1, Ctx, Method, NotAllowedAt);
end;

{ ---- TRouter ---- }

constructor TRouter.Create;
begin
  inherited Create;
  FTree := TRouteTree.Create;
end;

destructor TRouter.Destroy;
var
  i: Integer;
begin
  for i := 0 to High(FOwned) do
    FOwned[i].Free;
  FTree.Free;
  inherited Destroy;
end;

function TRouter.Root: TRouter;
begin
  Result := Self;
  while Result.FOwner <> nil do
    Result := Result.FOwner;
end;

function TRouter.Wrap(const H: THandler): THandler;
var
  i: Integer;
begin
  Result := H;
  for i := High(FMws) downto 0 do
    Result := FMws[i](Result);
end;

procedure TRouter.AddEndpoint(const AMethod, Pattern: string; const H: THandler; Sub: TRouter);
var
  r: TRouter;
  node: TRouteNode;
  i, idx: Integer;
begin
  if not Assigned(H) then
    raise ERouterError.Create('netrouter: a nil handler for "' + Pattern + '"');
  r := Root;
  node := r.FTree.NodeOf(Pattern);
  SetLength(r.FEnds, Length(r.FEnds) + 1);
  idx := High(r.FEnds);
  r.FEnds[idx].Handler := Wrap(H);
  r.FEnds[idx].Method := AMethod;
  r.FEnds[idx].Pattern := Pattern;
  r.FEnds[idx].Sub := Sub;
  { the same method and pattern again: the new handler replaces it }
  for i := 0 to High(node.Methods) do
    if node.Methods[i] = AMethod then
    begin
      r.FEnds[node.Endpoints[i]].Method := '';   { (gone: Walk skips it) }
      node.Endpoints[i] := idx;
      FHasRoutes := true;
      r.FHasRoutes := true;
      exit;
    end;
  SetLength(node.Methods, Length(node.Methods) + 1);
  SetLength(node.Endpoints, Length(node.Endpoints) + 1);
  node.Methods[High(node.Methods)] := AMethod;
  node.Endpoints[High(node.Endpoints)] := idx;
  FHasRoutes := true;
  r.FHasRoutes := true;
end;

procedure TRouter.Use(const M: TMiddleware);
begin
  if FHasRoutes then
    raise ERouterError.Create('netrouter: Use comes before the routes of a router ' +
      '(Using or Group give routes registered later their own middleware)');
  if not Assigned(M) then
    raise ERouterError.Create('netrouter: Use of a nil middleware');
  SetLength(FMws, Length(FMws) + 1);
  FMws[High(FMws)] := M;
end;

function TRouter.Using(const M: TMiddleware): TRouter;
var
  g: TRouter;
  owner: TRouter;
begin
  g := TRouter.Create;
  g.FTree.Free;
  g.FTree := nil;
  owner := Root;
  g.FOwner := Self;
  g.FMws := Copy(FMws);
  if Assigned(M) then
  begin
    SetLength(g.FMws, Length(g.FMws) + 1);
    g.FMws[High(g.FMws)] := M;
  end;
  SetLength(owner.FOwned, Length(owner.FOwned) + 1);
  owner.FOwned[High(owner.FOwned)] := g;
  Result := g;
end;

procedure TRouter.Group(const Fn: TRouterProc);
begin
  Fn(Using(nil));
end;

procedure TRouter.Mount(const Prefix: string; Sub: TRouter);
var
  base: string;
  s: TRouter;
begin
  if Sub = nil then
    raise ERouterError.Create('netrouter: Mount of a nil router');
  if Sub.FOwner <> nil then
    raise ERouterError.Create('netrouter: Mount of a group (mount its router)');
  base := Prefix;
  if (base <> '') and (base[Length(base)] = '/') then
    SetLength(base, Length(base) - 1);
  Sub.FParent := Self;
  s := Sub;
  { Prefix itself (the sub-router's "/") and everything under it }
  if base <> '' then
    AddEndpoint('*', base,
      procedure(C: TCtx; R: TRouteContext) => begin
        s.ServeAt(C, R, Length(R.Path) + 1);
      end, Sub);
  AddEndpoint('*', base + '/*',
    procedure(C: TCtx; R: TRouteContext) => begin
      { the mount's '*' starts just past the '/' that begins the rest }
      var from := R.LastParamStart - 1;
      R.PopParams(R.ParamCount - 1);
      s.ServeAt(C, R, from);
    end, Sub);
end;

procedure TRouter.Route(const Prefix: string; const Fn: TRouterProc);
var
  sub: TRouter;
  owner: TRouter;
begin
  sub := TRouter.Create;
  owner := Root;
  SetLength(owner.FOwned, Length(owner.FOwned) + 1);
  owner.FOwned[High(owner.FOwned)] := sub;
  Fn(sub);
  Mount(Prefix, sub);
end;

procedure TRouter.Handle(const Pattern: string; const H: THandler);
begin
  AddEndpoint('*', Pattern, H, nil);
end;

procedure TRouter.Method(const AMethod, Pattern: string; const H: THandler);
begin
  if (AMethod = '') or (AMethod = '*') then
    raise ERouterError.Create('netrouter: Method needs a method name (Handle takes all)');
  AddEndpoint(AMethod, Pattern, H, nil);
end;

procedure TRouter.Get(const Pattern: string; const H: THandler);
begin
  AddEndpoint('GET', Pattern, H, nil);
end;

procedure TRouter.Post(const Pattern: string; const H: THandler);
begin
  AddEndpoint('POST', Pattern, H, nil);
end;

procedure TRouter.Put(const Pattern: string; const H: THandler);
begin
  AddEndpoint('PUT', Pattern, H, nil);
end;

procedure TRouter.Patch(const Pattern: string; const H: THandler);
begin
  AddEndpoint('PATCH', Pattern, H, nil);
end;

procedure TRouter.Delete(const Pattern: string; const H: THandler);
begin
  AddEndpoint('DELETE', Pattern, H, nil);
end;

procedure TRouter.Head(const Pattern: string; const H: THandler);
begin
  AddEndpoint('HEAD', Pattern, H, nil);
end;

procedure TRouter.Options(const Pattern: string; const H: THandler);
begin
  AddEndpoint('OPTIONS', Pattern, H, nil);
end;

procedure TRouter.NotFound(const H: THandler);
begin
  Root.FNotFound := H;
end;

procedure TRouter.MethodNotAllowed(const H: THandler);
begin
  Root.FNotAllowed := H;
end;

{ this router's NotFound, or the nearest parent's }
function TRouter.FallbackNotFound: THandler;
var
  r: TRouter;
begin
  r := Root;
  while r <> nil do
  begin
    if Assigned(r.FNotFound) then
      exit(r.FNotFound);
    r := r.FParent;
    if r <> nil then
      r := r.Root;
  end;
  Result := nil;
end;

function TRouter.FallbackNotAllowed: THandler;
var
  r: TRouter;
begin
  r := Root;
  while r <> nil do
  begin
    if Assigned(r.FNotAllowed) then
      exit(r.FNotAllowed);
    r := r.FParent;
    if r <> nil then
      r := r.Root;
  end;
  Result := nil;
end;

procedure TRouter.ServeAt(Ctx: TCtx; Route: TRouteContext; From: Integer);
var
  r: TRouter;
  ep: Integer;
  na: TRouteNode;
  h: THandler;
begin
  r := Root;
  ep := r.FTree.Find(Route.Path, From, Route, Route.Method, na);
  if ep >= 0 then
  begin
    Route.Finish(rrMatched, '');
    Route.PushPattern(r.FEnds[ep].Pattern);
    r.FEnds[ep].Handler(Ctx, Route);
    exit;
  end;
  { not found here: the handler runs inside this router's middleware }
  if na <> nil then
  begin
    Route.Finish(rrMethodNotAllowed, na.AllowedList);
    h := FallbackNotAllowed;
  end
  else
  begin
    Route.Finish(rrNotFound, '');
    h := FallbackNotFound;
  end;
  if Assigned(h) then
    r.Wrap(h)(Ctx, Route);
end;

function TRouter.Serve(const AMethod, Path: string; Ctx: TCtx;
                       Route: TRouteContext): TRouteResult;
var
  own: Boolean;
begin
  own := Route = nil;
  if own then
    Route := TRouteContext.Create;
  try
    Route.Start(AMethod, Path);
    Root.ServeAt(Ctx, Route, 1);
    Result := Route.Outcome;
  finally
    if own then
      Route.Free;
  end;
end;

procedure TRouter.WalkPrefix(const Prefix: string; const Fn: TWalkProc);
var
  r: TRouter;
  i: Integer;
  p: string;
begin
  r := Root;
  for i := 0 to High(r.FEnds) do
  begin
    p := r.FEnds[i].Pattern;
    if r.FEnds[i].Method = '' then
      continue;   { replaced by a later registration }
    if r.FEnds[i].Sub <> nil then
    begin
      { a mount: its routes (once, from its '/*' endpoint) }
      if (Length(p) >= 2) and (Copy(p, Length(p) - 1, 2) = '/*') then
        r.FEnds[i].Sub.WalkPrefix(Prefix + Copy(p, 1, Length(p) - 2), Fn);
      continue;
    end;
    Fn(r.FEnds[i].Method, Prefix + p);
  end;
end;

procedure TRouter.Walk(const Fn: TWalkProc);
begin
  WalkPrefix('', Fn);
end;

{$ENDIF}

end.
