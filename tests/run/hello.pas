{ Compile, run, and compare stdout with the .expected file, at -O0 and -O2.
  RUN: %p3 %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/hello.expected %t.out
  RUN: %p3 -O2 %s -o %t.O2
  RUN: %t.O2 > %t.O2.out
  RUN: diff %S/hello.expected %t.O2.out
}
program hello(output);
begin
  writeln('Hello, World!')
end.
