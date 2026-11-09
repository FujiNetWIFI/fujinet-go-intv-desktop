REM srcmap test fixture: a loop that calls a procedure from an INCLUDEd file
x = 0
main:
  x = x + 1
  GOSUB bump
  WAIT
  GOTO main

INCLUDE "inc.bas"
