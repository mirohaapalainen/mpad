# mpad - a screen-oriented terminal text editor #

# Overview #

The idea for this project is to basically create a vi/vim clone,
custom tailored for my purposes (i.e. with some extra features I want,
without the stuff in vi/vim I don't use). This is mostly for fun but I
do plan on getting some actual use out of it.

# Implementation details #

The fundamental buffer structure used is essentially a big array of lines. 
It's important to note, however, that there is an abstraction for the cursor
and what text is on the screen, as in the code the positions for these are referred
to in x, y coordinates rather than line numbers and line positions.

# To compile #

-Run 'make'. It will compile a ready-to-use executable.
