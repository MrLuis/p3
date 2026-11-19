{ Fixed-point real formatting: the fast path, its rounding fallbacks, and
  the large-value fallback.  RUN: %p3 %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/real-fixed.expected %t.out
}
program RealFixed(output);
begin
  writeln(-1428.571428571:0:6);
  writeln(181375198356877.97:0:2);
  writeln(-2869926440445.2388:0:5);
  writeln(183494.19875010251:0:9);
  writeln(0.5:0:0, ' ', -0.5:0:0, ' ', 2.5:0:0, ' ', 0.0:0:0);
end.
