program target;
begin
{$IFDEF LINUX} writeln('linux'); {$ENDIF}
{$IFDEF DARWIN} writeln('darwin'); {$ENDIF}
{$IFDEF BSD} writeln('bsd'); {$ENDIF}
{$IFDEF WINDOWS} writeln('windows'); {$ENDIF}
{$IFDEF MSWINDOWS} writeln('mswindows'); {$ENDIF}
{$IFDEF WIN64} writeln('win64'); {$ENDIF}
{$IFDEF UNIX} writeln('unix'); {$ENDIF}
{$IFDEF CPUX86_64} writeln('x8664'); {$ENDIF}
{$IFDEF CPUAARCH64} writeln('aarch64'); {$ENDIF}
{$IFDEF CPU64} writeln('cpu64'); {$ENDIF}
end.
