{ write / writeln with strings, chars and integers.
  RUN: %p3 %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/write.expected %t.out
}
program WriteTest(output);
begin
  write('a');
  write('b', 'c');
  writeln;
  WRITELN('It''s ', 42, ' and ', 0);
  writeln(2147483647);
  Writeln('x', 'y', 'z', 1, 2, 3);
  writeln;
  (* the last line has no newline *)
  write('no newline at end')
end.
