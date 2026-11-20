{ TStrings.LoadFromFile reads 64 KiB blocks (docs/classes-review.md CL-1):
  line ends and lines that cross a block boundary give the same lines as
  setting Text to the file's contents. Output identical to Free Pascal
  3.2.2's (fpc -Mobjfpc). Run in its own directory: it creates and
  removes a file.
  RUN: rm -rf %t && mkdir -p %t && %p3 --mode=objfpc %s -o %t/a
  RUN: cd %t && ./a > out && diff %S/rtl-loadblocks.expected out
  RUN: %p3 --mode=objfpc -O2 %s -o %t/a2
  RUN: cd %t && ./a2 > out2 && diff %S/rtl-loadblocks.expected out2
}
program rtlloadblocks;
{$H+}
uses sysutils, classes;

const
  Block = 65536;

function Fill(n: longint; c: char): string;
var
  r: string;
begin
  SetLength(r, n);
  if n > 0 then
    FillChar(r[1], n, c);
  Fill := r;
end;

procedure WriteRaw(const name, data: string);
var
  f: file;
begin
  AssignFile(f, name);
  Rewrite(f, 1);
  if length(data) > 0 then
    BlockWrite(f, data[1], length(data));
  CloseFile(f);
end;

{ Loads data from a file and from Text; the same lines? }
procedure Check(const what, data: string);
var
  a, b: TStringList;
  i: longint;
  same: boolean;
begin
  WriteRaw('t_blocks', data);
  a := TStringList.Create;
  b := TStringList.Create;
  a.LoadFromFile('t_blocks');
  b.Text := data;
  same := a.Count = b.Count;
  if same then
    for i := 0 to a.Count - 1 do
      if a[i] <> b[i] then
        same := false;
  write(what, ': ', a.Count, ' lines, ', same);
  if a.Count > 0 then
    write(', last ', length(a[a.Count - 1]));
  writeln;
  a.Free;
  b.Free;
end;

var
  many: string;
  i: longint;
begin
  Check('CR at a block end, LF after it', Fill(Block - 1, 'x') + #13#10 + 'y');
  Check('CR LF just before a block end', Fill(Block - 2, 'x') + #13#10 + 'y');
  Check('LF first in a block', Fill(Block, 'x') + #10 + 'y');
  Check('CR last in the file at a block end', Fill(Block - 1, 'x') + #13);
  Check('a line over three blocks', Fill(3 * Block + 7, 'x'));
  Check('a line over two blocks, then more', Fill(2 * Block, 'x') + #10 + 'z'#13'w');
  Check('an empty line at a block end', Fill(Block - 1, 'x') + #10#10 + 'y');
  Check('exactly one block', Fill(Block - 1, 'x') + #10);
  many := '';
  for i := 1 to 30000 do
    case i mod 3 of
      0: many := many + 'line ' + IntToStr(i) + #10;
      1: many := many + 'line ' + IntToStr(i) + #13#10;
      2: many := many + 'line ' + IntToStr(i) + #13;
    end;
  Check('30000 lines, mixed ends', many);
  DeleteFile('t_blocks');
end.
