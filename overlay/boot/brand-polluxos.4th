2 brandX ! 1 brandY ! \ Initialize brand placement defaults

: brand+ ( x y c-addr/u -- x y' )
  2swap 2dup at-xy 2swap \ position the cursor
  [char] @ escc! \ replace @ with Esc
  type \ print to the screen
  1+ \ increase y for next time we're called
;

: brand ( x y -- ) \ "PolluxOS" brand (11 rows x 48 columns)

  s"  @[34;1m ____            _       _@[m" brand+
  s"  @[34;1m|  _ \          | |     | |@[m" brand+
  s"  @[34;1m| |_) |   ___   | |     | |      _   _  __  __@[m" brand+
  s"  @[34;1m|  __/   / _ \  | |     | |     | | | | \ \/ /@[m" brand+
  s"  @[34;1m|_|     | (_) | |_|     |_|     | |_| |  >  <@[m" brand+
  s"  @[34;1m         \___/                  \__,_| /_/\_\@[m" brand+
  s"  @[34;1m                 ___    ____@[m" brand+
  s"  @[34;1m                / _ \  / ___|@[m" brand+
  s"  @[34;1m               | | | | \___ \@[m" brand+
  s"  @[34;1m               | |_| |  ___) |@[m" brand+
  s"  @[34;1m                \___/  |____/@[m" brand+

  2drop
;
