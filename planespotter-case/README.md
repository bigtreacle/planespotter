# PlaneSpotter case (CYD 2.8", USB-C + micro-USB board)

Two printed parts, one optional plunger, four M2.5 x 12 countersunk screws.
Dimensions measured on the actual board (7 Nov 2026); the STLs are ready
to print.

Two equivalent sources:

- `PlaneSpotterCase.FCMacro`: FreeCAD. Macro > Macros > run it. It creates
  a document with a `Params` spreadsheet (aliases, notes) and builds the
  parts. Edit a value in the sheet, run the macro again, it rebuilds.
  Set `EXPORT_STL = True` at the top to write STLs beside the saved file.
  Written against FreeCAD 1.0's Part API but not test-run here, so if it
  throws, the traceback line number will point straight at the culprit.
- `planespotter_case.scad`: OpenSCAD, same geometry, same parameter names.
  The STLs in this folder were generated from it.

    shell.stl    rear box with built-in 15 degree stand   (~55 g, ~2.5 h)
    bezel.stl    front frame                               (~15 g, ~45 min)
    plunger.stl  press-through BOOT button                 (~1 min)
    testfit.stl  low-wall shell to check fit first         (~15 min)

## One check before printing: `flip`

Everything is measured except which physical edge is "up" when the
firmware draws text. Power the board, hold it with the text upright and
the USB sockets on the left, and look at the back: the two tact switches
should be behind the **bottom** edge. If they are behind the top edge,
set `flip` to -1 (spreadsheet in FreeCAD, or the variable in the .scad)
and rebuild. The USB slot is symmetric, so nothing else changes.

Then print `testfit.stl` (15 min): the PCB should drop onto the four
standoffs with a little side play, and both USB plugs should pass the slot.

## Render

    openscad -o shell.stl   -D 'part="shell"'   planespotter_case.scad
    openscad -o bezel.stl   -D 'part="bezel"'   planespotter_case.scad
    openscad -o plunger.stl -D 'part="plunger"' planespotter_case.scad
    openscad -o testfit.stl -D 'part="testfit"' planespotter_case.scad

`part="all"` shows the assembly in the GUI.

## Print settings

- PETG or PLA, 0.2 mm layers, 3 walls, 20% infill. No supports.
- Shell: prints as exported, base on the bed. The inside of the top wall
  is a 13 mm bridge at 15 degrees; any printer handles it.
- Bezel: exported face-down. The window chamfer is a 45 degree overhang.
  A textured plate gives a nice front finish.
- Plunger: as exported, head down.
- Matte black for the front hides the yellow PCB best; the shell can be
  any colour.

## Assembly

1. Drop the plunger into the BOOT hole from inside the shell, head inwards.
2. Lower the PCB onto the standoffs, display out, USB sockets at the slot.
3. Place the bezel; its corner bosses sit on the PCB around the holes.
4. Four M2.5 x 12 screws from the front, self-tapping into the standoffs.
   Snug, not tight: the bosses clamp the PCB.

## Using it

- USB-C on the left. **Use a USB-A to USB-C cable**: this board lacks
  the resistors that let a C-to-C cable or PD charger turn on power.
- Hold the back button 3 s for setup, short press to check for updates.
- RST is the pinhole: paperclip.

## Changing the design

Everything is a variable: `tilt` for the stand angle, `wall`, `corner_r`,
`win_margin`, `vent`, `sd_slot` if you want microSD access on the right
wall. Set `foot_back` larger for a more stable base on a soft surface.
