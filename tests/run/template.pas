(* Template strings (tp and objfpc): text between backticks, which may span
   lines, with interpolations ${value}, ${value:width} and
   ${value:width:digits} written as write writes them. The layout's line
   breaks and indentation are removed; r`...` is raw; Tag`...` calls Tag
   with the text parts and the values.
   RUN: %pcc --mode=tp %s -o %t
   RUN: %t | diff - %S/template.expected
   RUN: %pcc --mode=objfpc -O2 -fno-range-checks %s -o %t.fast
   RUN: %t.fast | diff - %S/template.expected
   RUN: %pcc --mode=objfpc -g %s -o %t.g
   RUN: %t.g | diff - %S/template.expected
*)
program template;
{$H+}

type
  TColor = (Red, Green, Blue);
  TPoint = record
    x, y: longint;
  end;

const
  Greeting = `Hello`;
  Query = `
    SELECT id, name
      FROM customers
    WHERE region = :region
    `;
  Pattern = r`^\d{3}-${4}$`;
  OneChar = `x`;
  Empty = ``;

var
  shown: longint;

procedure show(const s: string);
begin
  shown := shown + 1;
  writeln(shown:2, ' [', s, ']');
end;

function twice(n: longint): longint;
begin
  twice := 2 * n;
end;

function describe(const p: TPoint): string;
begin
  describe := `(${p.x}, ${p.y})`;
end;

function countdown(n: longint): string;
begin
  if n = 0 then
    countdown := `liftoff`
  else
    countdown := `${n} ${countdown(n - 1)}`;
end;

{ A tag: the text parts joined by the number of each value. }
function Numbered(const parts: array of string;
                  const values: array of const): string;
var
  i: longint;
  r: string;
begin
  r := '';
  for i := 0 to high(parts) do
  begin
    r := r + parts[i];
    if i <= high(values) then
      r := r + `<${i + 1}>`;
  end;
  Numbered := r + ` (${high(parts) + 1} parts, ${high(values) + 1} values)`;
end;

{ A tag that formats the values itself: strings quoted, integers plain. }
function Sql(const parts: array of string;
             const values: array of const): string;
var
  i: longint;
  r: string;
begin
  r := '';
  for i := 0 to high(parts) do
  begin
    r := r + parts[i];
    if i <= high(values) then
      case values[i].vtype of
        vtInteger: r := r + `${values[i].vinteger}`;
        vtAnsiString: r := r + '''' + ansistring(values[i].vansistring) + '''';
      else
        r := r + '?';
      end;
  end;
  Sql := r;
end;

var
  logged: longint;

procedure Log(const parts: array of string; const values: array of const);
begin
  logged := logged + 1;
  writeln('log ', logged, ': ', high(parts) + 1, ' parts, ', high(values) + 1,
          ' values, first [', parts[0], ']');
end;

var
  name: string;
  short: string[20];
  zs: string[10];
  count, i: longint;
  big: int64;
  huge: qword;
  total: real;
  c: char;
  ok: boolean;
  color: TColor;
  p: pchar;
  pt: TPoint;
  s, t: string;

begin
  name := 'World';
  short := 'short';
  count := 3;
  big := -9000000000;
  huge := 18446744073709551615;
  total := 12.5;
  c := 'q';
  ok := true;
  color := Green;
  zs := 'pchar'#0;
  p := @zs[1];
  pt.x := 4;
  pt.y := -7;

  { values of each type, as write writes them }
  show(`Hello ${name}, you have ${count} items totalling ${total:0:2}.`);
  show(`${count}|${big}|${huge}|${c}|${ok}|${color}|${short}|${p}`);
  show(`${total}|${total:10:3}|${total:0:0}|${-total:8:1}`);
  show(`${count:5}|${big:15}|${c:3}|${ok:6}|${color:7}|${name:8}|${short:7}`);
  show(`${'literal'}|${'c'}|${1.5:0:1}|${42}|${true}|${Blue}`);
  show(`${count:count + 1}|${total:count * 2:count - 1}`);

  { expressions }
  show(`${count + 1} ${twice(count) * 2} ${count > 2} ${name + '!'} ${pt.x * pt.y}`);
  show(`${describe(pt)} is ${length(describe(pt))} long`);
  show(`${ count { a comment } + (* another *) 1 }`);
  show(`${'}'} ${'{'} ${'it''s'} ${'`'}`);
  show(`nested ${`in ${count + 1} ner ${`deep ${name}`}`} out`);
  show(countdown(3));

  { text: escapes, and what is not one }
  show(`a `` backtick, a $${brace}, a $ dollar, $5, $$, {braces}, 'quotes'`);
  show(`${count}${count}`);
  show(`${count}`);
  show(``);
  show(`` + Greeting + ` ` + `${name}` + `!`);
  show(r`raw: ${name} \n $${x} `` done`);
  show(R`C:\dir\file`);
  show(Pattern);
  show(OneChar);
  show(Empty + Greeting);

  { lines: the first line break and the last line go, and the indentation
    the lines share }
  show(Query);
  show(`
    Report for ${name}
      Color: ${color}

    Total: ${total:0:2}
    `);
  show(`first line
        second line
          third line`);
  show(`
      deeper
    closing backtick to the left of the text: its indentation is kept
  `);
  show(`
    ${name} starts a line
    and ${count} is inside one
    ${count}`);
  show(`
    no last line to remove`);
  show(`

    blank lines around

    `);
  show(`one
`);
  show(`
`);
  show(`
	tab
	  tab and blanks
	`);
  show(`${count} values
       on ${count - 1} lines`);

  { where a string goes }
  s := `count = ${count}`;
  t := s + ` and ` + `name = ${name}`;
  show(t);
  short := `short ${count}`;
  show(short);
  short := `0123456789${'0123456789'}truncated`;
  show(short);
  if `${count}` = '3' then
    show('equal');
  show(`${length(`${name}${name}`)}`);
  writeln(`${name}`:10, '|', `${count}`, '|');
  for i := 1 to 3 do
    s := `${s}, ${i}`;
  show(s);
  s := '';
  for i := 1 to 200 do
    s := `${s}${i mod 10}`;
  show(`${length(s)} characters ending in ${s[200]}`);

  { tagged templates }
  show(Numbered`select ${count} from ${name} end`);
  show(Numbered`no values`);
  show(Numbered`${count}`);
  show(Numbered`${count}${name}`);
  show(Sql`
    SELECT id FROM customers
    WHERE region = ${name} AND rank > ${count}
    `);
  show(`tagged inside: ${Numbered`a ${count} b`}`);
  show(Numbered`template inside: ${`x${count}y`}`);
  Log`starting ${name}`;
  Log`plain`;
  Log`
    lines ${count}
    `;
end.
