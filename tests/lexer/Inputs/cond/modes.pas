{$ifdef P3} a {$endif}
{$ifdef P3_ISO} iso {$endif}
{$ifdef P3_TP} tp {$endif}
{$ifdef P3_OBJFPC} objfpc {$endif}
{$ifdef CMDLINE} cmdline {$endif}
{$if defined(p3) and not defined(Cmdline)} nocmdline {$ifend}
{$ifdef never} x { y } (* z *) 'w {$endif} '
{$endif}
{$ifdef never} x (* {$endif} *) y {$endif} z
