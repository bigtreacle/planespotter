// PlaneSpotter desk case for the ESP32-2432S028 "Cheap Yellow Display"
// (2.8" board, USB-C + micro-USB variant).
//
// Two printed parts plus an optional button plunger:
//   shell   - rear box with integrated 15-degree wedge stand, standoffs,
//             USB slot, BOOT/RST access, vent slots. Prints base-down.
//   bezel   - front frame with display window, screws through the PCB
//             corner holes into the shell. Prints face-down.
//   plunger - press-through button for BOOT (hold 3 s = setup portal).
//   testfit - a 10-minute print of the shell floor + low walls to confirm
//             the hole spacing and USB slot before printing the real thing.
//
// Hardware: 4 x M2.5 x 12 countersunk screws (self-tapping into the
// standoffs). M2 works too; nothing larger will pass the PCB holes.
//
// Coordinates while modelling: PCB in the XY plane, origin at PCB centre,
// X along the long edge, USB edge at -X, display facing +Z.
// The stand transform at the bottom puts it upright.

part = "all"; // ["all", "shell", "bezel", "plunger", "testfit"]

// ---------------- PCB (confirm with calipers, see README) ----------------
pcb_l      = 86.0;   // long edge
pcb_w      = 50.0;   // short edge
pcb_t      = 1.6;
hole_d     = 3.0;    // corner hole diameter
hole_dx    = 78.0;   // CONFIRM: hole centre spacing, long axis
hole_dy    = 42.0;   // CONFIRM: hole centre spacing, short axis

// ---------------- display ----------------
disp_aw    = 59.0;   // active area, long axis
disp_ah    = 44.2;   // active area, short axis
disp_cx    = -2.5;    // CONFIRM: active-area centre offset from PCB centre, X
disp_cy    = 0.0;    // CONFIRM: ... and Y (positive = towards +Y)
disp_h     = 4.4;    // glass top above PCB front face
win_margin = 1.0;    // window oversize per side around the active area

// ---------------- rear components ----------------
// flip = 1: with text the right way up and USB on the left, the two back
// switches are behind the BOTTOM edge (and microSD on the top edge).
// flip = -1 if they are behind the top edge.
flip       = 1;
rear_h     = 7.0;    // standoff height: clearance for ESP32 module + connectors
usb_slot_w = 34.0;   // one slot covers both USB sockets
usb_slot_h = 8.0;
usb_slot_cy= 0.0;    // CONFIRM: slot centre along the USB edge (Y)
boot_pos   = [28.5, -18.5*flip]; // CONFIRM: BOOT tact switch centre, PCB coords (back side)
rst_pos    = [33.5, -18.5*flip]; // CONFIRM: RST tact switch centre
btn_h      = 5.0;    // tact switch height above PCB back face
sd_slot    = false;  // set true + position if you want microSD access
sd_pos     = -4;     // slot centre along the top edge (X); slot is 14 wide

// ---------------- case ----------------
wall       = 2.0;
clr        = 0.4;    // PCB-to-wall clearance per side
corner_r   = 3.0;
tilt       = 15;     // stand angle, degrees back from vertical
foot_t     = 3.0;    // base plate thickness
foot_back  = 14.0;   // how far the base extends behind the case
foot_lip   = 3.0;    // ... and in front
screw_d    = 2.8;    // clearance for M2.5
pilot_d    = 2.1;    // self-tap pilot in standoffs
cs_d       = 5.4;    // countersink diameter
standoff_d = 6.0;
boss_d     = 5.5;
plunger_hole_d = 5.0;
plunger_stem_d = 4.4;
vent       = true;

$fn = 48;
eps = 0.01;

// ---------------- derived ----------------
in_l   = pcb_l + 2*clr;             // interior
in_w   = pcb_w + 2*clr;
out_l  = in_l + 2*wall;             // exterior
out_w  = in_w + 2*wall;
cav_h  = rear_h + pcb_t + disp_h + 0.5;   // interior depth, floor to bezel
depth  = wall + cav_h;              // shell exterior depth
pcb_z  = wall + rear_h;             // PCB back face, local Z
boss_h = cav_h - rear_h - pcb_t;    // bezel boss: bezel underside to PCB front
holes  = [[-hole_dx/2,-hole_dy/2],[hole_dx/2,-hole_dy/2],
          [hole_dx/2, hole_dy/2],[-hole_dx/2, hole_dy/2]];

module rrect(l, w, r, h) {
  linear_extrude(h)
    offset(r) offset(-r) square([l, w], center=true);
}

// ---------------- shell (local coords) ----------------
module shell_body(h = depth) {
  difference() {
    rrect(out_l, out_w, corner_r, h);
    // cavity
    translate([0,0,wall]) rrect(in_l, in_w, corner_r - wall, h);
    // USB slot on the -X wall: from just above the PCB front face downwards
    translate([-out_l/2 - eps, usb_slot_cy - usb_slot_w/2, pcb_z + pcb_t + 1 - usb_slot_h])
      cube([wall + 2*eps, usb_slot_w, usb_slot_h]);
    // BOOT plunger hole and RST pin hole through the floor
    translate([boot_pos[0], boot_pos[1], -eps]) cylinder(d=plunger_hole_d, h=wall+2*eps);
    translate([rst_pos[0],  rst_pos[1],  -eps]) cylinder(d=2.5, h=wall+2*eps);
    // optional microSD slot on the +X wall
    if (sd_slot)
      translate([sd_pos - 8, flip*(out_w/2) - wall/2 - wall, pcb_z - 3.5]) cube([16, 2*wall, 4]);
    // vent slots on the back, away from the buttons
    if (vent)
      for (i = [0:3])
        translate([-30 + i*7, -14, -eps]) cube([1.6, 28, wall + 2*eps]);
  }
  // standoffs with pilot holes
  for (p = holes)
    translate([p[0], p[1], wall - eps])
      difference() {
        cylinder(d=standoff_d, h=rear_h + eps);
        translate([0,0,1]) cylinder(d=pilot_d, h=rear_h);
      }
}

// ---------------- stand transform ----------------
// Local Z (depth) -> world -Y (display faces -Y, towards the viewer),
// local Y -> world Z (up), then lean back by `tilt`.
module place() {
  translate([0, 0, (out_w/2)*cos(tilt) + foot_t])
    rotate([-tilt, 0, 0]) rotate([90, 0, 0]) children();
}

module foot() {
  front = -depth*cos(tilt) - (out_w/2)*sin(tilt) - foot_lip;
  back  =  (out_w/2)*sin(tilt) + foot_back;
  hull() {
    // thin slice of the shell's bottom face, in world position
    place() translate([-out_l/2, -out_w/2, 0]) cube([out_l, 1, depth]);
    // base plate on the ground
    translate([-out_l/2, front, 0]) cube([out_l, back - front, foot_t]);
  }
}

module shell_with_stand() {
  union() {
    place() shell_body();
    foot();
  }
}

// ---------------- bezel (local coords, modelled in place on the shell) ----------------
module bezel_local() {
  wl = disp_aw + 2*win_margin;
  ww = disp_ah + 2*win_margin;
  difference() {
    union() {
      translate([0,0,depth]) rrect(out_l, out_w, corner_r, wall);
      // alignment lip inside the walls
      translate([0,0,depth - 1.5]) rrect(in_l - 2*clr, in_w - 2*clr, corner_r - wall, 1.5 + eps);
      // corner bosses down to the PCB front face
      for (p = holes)
        translate([p[0], p[1], pcb_z + pcb_t - eps]) cylinder(d=boss_d, h=boss_h + 2*eps);
    }
    // window with outward 45-degree chamfer
    translate([disp_cx, disp_cy, depth - 5])
      linear_extrude(10) square([wl, ww], center=true);
    translate([disp_cx, disp_cy, depth + wall - 1.5])
      linear_extrude(1.5 + eps, scale=[(wl+3)/wl, (ww+3)/ww]) square([wl, ww], center=true);
    // clear the display module and FPC: everything inside the lip above the PCB
    translate([0,0,pcb_z + pcb_t]) rrect(in_l - 2*clr - 2, in_w - 2*clr - 2, 1, disp_h + 0.3);
    // screw holes with countersink
    for (p = holes) {
      translate([p[0], p[1], pcb_z]) cylinder(d=screw_d, h=depth);
      translate([p[0], p[1], depth + wall - 1.6]) cylinder(d1=screw_d, d2=cs_d, h=1.6 + eps);
    }
  }
}

// ---------------- plunger ----------------
module plunger() {
  gap    = rear_h - btn_h;          // floor top to button top
  head_h = gap - 0.6;               // leaves 0.6 mm travel before it touches the button
  stem_l = wall + 2.5;              // through the floor, 2.5 mm proud
  cylinder(d=plunger_hole_d + 3, h=head_h);
  translate([0,0,-stem_l]) cylinder(d=plunger_stem_d, h=stem_l + eps);
}

// ---------------- PCB mock-up for the preview ----------------
module pcb_mock() {
  color("goldenrod", 0.5) translate([0,0,pcb_z]) difference() {
    translate([-pcb_l/2, -pcb_w/2, 0]) cube([pcb_l, pcb_w, pcb_t]);
    for (p = holes) translate([p[0], p[1], -eps]) cylinder(d=hole_d, h=pcb_t + 2*eps);
  }
  color("black", 0.6) translate([disp_cx - disp_aw/2, disp_cy - disp_ah/2, pcb_z + pcb_t])
    cube([disp_aw, disp_ah, disp_h]);
}

// ---------------- parts ----------------
if (part == "shell")   shell_with_stand();
if (part == "bezel")   rotate([180,0,0]) bezel_local();           // face down for printing
if (part == "plunger") plunger();
if (part == "testfit") intersection() {
  shell_body();
  translate([-100,-100,-1]) cube([200, 200, pcb_z + pcb_t + 2]);
}
if (part == "all") {
  color("dimgray") shell_with_stand();
  color("slategray", 0.9) place() bezel_local();
  place() pcb_mock();
  place() translate([boot_pos[0], boot_pos[1], wall]) color("orange") plunger();
}
