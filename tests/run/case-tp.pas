{ Turbo/FPC case extensions: label ranges, else/otherwise, no match = no-op.
  RUN: %p3 --mode=tp %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/case-tp.expected %t.out
}
program CaseTP(output);
var i: integer; c: char;
begin
  for i := 0 to 5 do
    case i of
      1..2: writeln(i, ' small');
      3: writeln(i, ' three');
    else
      writeln(i, ' other')
    end;
  { a range too large to expand into switch cases }
  case 1000 of
    0..9999: writeln('large range');
  end;
  case -5 of
    -100..-1: writeln('negative');
  otherwise writeln('nope')
  end;
  case 42 of
    1: writeln('not printed')
  end;
  c := 'q';
  case c of 'a'..'m': writeln('first half') else writeln('second half') end;
  writeln('done')
end.
