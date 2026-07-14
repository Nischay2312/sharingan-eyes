#!/usr/bin/env python3
"""Reference watch + lens models and slide-in housing for the Sharingan eye.

Watch : Waveshare ESP32-S3-Touch-AMOLED-2.06 (from official dimensional drawing).
Lens  : 48 mm x 14 mm plano-convex LED optic (2.5 mm flange, user measured).

Housing style (per user's sketch):
    - Rectangular tray, open at one END (rear) for slide-in.
    - Watch slides in first, seats against front wall.
    - Lens slides in on top; the flange is trapped between the watch top face
      and two inward "tabs" that run along the top left/right edges.
    - Dome protrudes through the gap between the tabs.
    - No mounting flange, no screw holes -- glue back face to the mask.

Coordinates (all parts share this frame):
    Origin at bottom-front-outer corner of housing (X = 0 is outer face of front wall,
    Y = 0 is centreline, Z = 0 is outer face of floor / mask side).
    +X = INTO housing (insertion direction).
    +Y = to the right.
    +Z = up, dome sticks out +Z.

Run:  python generate_eye_parts.py
Outputs (all in this folder):
    watch_amoled_2_06.stl / .step
    lens_48x14_plano_convex.stl / .step
    eye_case.stl / .step                 <- the part to 3D print
    eye_assembly.step                    <- multi-body preview
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

import cadquery as cq

HERE = Path(__file__).resolve().parent

# ==========================================================================
# WATCH  - ESP32-S3-Touch-AMOLED-2.06
# ==========================================================================
WATCH_L = 50.80         # long axis (X in housing / insertion axis)
WATCH_W = 42.00         # short axis (Y)
WATCH_H = 13.60         # total thickness
WATCH_BODY_H = 12.90    # main shell (front bezel adds the remaining 0.70)
WATCH_BEZEL_H = WATCH_H - WATCH_BODY_H  # 0.70
WATCH_CORNER_R = 6.0    # outer vertical corner radius (from drawing)

# Back recess (labelled 34.60 x 25.80, R4.5)
BACK_RECESS_L = 34.60
BACK_RECESS_W = 25.80
BACK_RECESS_R = 4.5
BACK_RECESS_DEPTH = 1.5

# Screen glass window on the front bezel (estimated, no explicit dim)
SCREEN_L = 44.5
SCREEN_W = 36.5
SCREEN_CORNER_R = 4.0
SCREEN_INSET = 0.35     # glass sits below bezel rim

# -Y face ports (USB-C, buttons, mics)
USBC_W = 9.0
USBC_H = 3.2
USBC_DEPTH = 3.5
BTN_SIZE = 3.6
BTN_DEPTH = 1.5
BTN_X = 8.8
MIC_D = 0.9
MIC_X = 22.0

# +Y face slots
TOP_SLOT_W = 12.0
TOP_SLOT_H = 1.8
TOP_SLOT_DEPTH = 1.5

# Side notches (+/-X faces, small triangular indicator)
SIDE_NOTCH_W = 2.6
SIDE_NOTCH_H = 1.4
SIDE_NOTCH_DEPTH = 0.6


# ==========================================================================
# LENS  - 48 x 14 mm plano-convex (user-verified 2.5 mm flange)
# ==========================================================================
LENS_OD = 48.0
LENS_TOTAL_H = 14.0
LENS_FLANGE_T = 2.5
LENS_DOME_BASE_D = 44.0     # dome base diameter at flange top (estimate)
LENS_DOME_H = LENS_TOTAL_H - LENS_FLANGE_T  # 11.5


# ==========================================================================
# HOUSING  - slide-in tray
# ==========================================================================
CLEAR_WATCH_XY = 0.5    # per side, XY (generous for print tolerances)
CLEAR_WATCH_Z  = 0.3    # top
CLEAR_LENS_XY  = 0.3    # per side (flange OD)
CLEAR_LENS_Z   = 0.15   # per side (flange thickness)

FRONT_WALL_T   = 2.5    # stops watch and lens at the far end
FLOOR_T        = 2.0    # mask-side wall
SIDE_WALL_T    = 2.0
TAB_T          = 1.5    # thickness of top tabs holding the flange down
TAB_INNER_MARGIN = 1.0  # tab covers this much of the flange from the outer edge inward
LEAD_IN_CHAMFER = 0.8   # chamfer at the rear opening for easier slide-in
CORNER_R_HOUSING = 4.0


def watch_cavity():
    """(X, Y, Z) inner dimensions of the watch cavity."""
    return (
        WATCH_L + 2 * CLEAR_WATCH_XY,
        WATCH_W + 2 * CLEAR_WATCH_XY,
        WATCH_H + CLEAR_WATCH_Z,
    )


def lens_slot():
    """(Y-diameter, Z-thickness) inner dimensions of the flange slot."""
    return (
        LENS_OD + 2 * CLEAR_LENS_XY,
        LENS_FLANGE_T + 2 * CLEAR_LENS_Z,
    )


def housing_outer():
    """(L, W, H) outer envelope of the housing."""
    cav_l, cav_w, cav_h = watch_cavity()
    slot_d, slot_t = lens_slot()
    L = FRONT_WALL_T + cav_l
    W = 2 * SIDE_WALL_T + slot_d
    H = FLOOR_T + cav_h + slot_t + TAB_T
    return L, W, H


# --------------------------------------------------------------------------
# Small helpers
# --------------------------------------------------------------------------
def rounded_prism_z(l, w, h, r):
    """Rectangular prism from Z=0 to Z=h, centred in XY, with vertical corner fillets."""
    prism = cq.Workplane("XY").rect(l, w).extrude(h)
    if r > 0:
        try:
            prism = prism.edges("|Z").fillet(r)
        except Exception:
            pass
    return prism


def safe_fillet(solid, selector, r):
    try:
        return solid.edges(selector).fillet(r)
    except Exception:
        return solid


def safe_chamfer(solid, selector, c):
    try:
        return solid.edges(selector).chamfer(c)
    except Exception:
        return solid


# --------------------------------------------------------------------------
# WATCH model
# --------------------------------------------------------------------------
def build_watch():
    """Watch centred in XY, back face at Z=0, screen face at Z=WATCH_H (+Z up)."""
    body = rounded_prism_z(WATCH_L, WATCH_W, WATCH_BODY_H, WATCH_CORNER_R)
    bezel = rounded_prism_z(WATCH_L, WATCH_W, WATCH_BEZEL_H, WATCH_CORNER_R).translate(
        (0, 0, WATCH_BODY_H)
    )
    watch = body.union(bezel)

    # Soften outer top/bottom edges
    watch = safe_fillet(watch, "<Z", 0.5)
    watch = safe_fillet(watch, ">Z", 0.3)

    # Screen glass inset (viewing side)
    screen_pocket = rounded_prism_z(
        SCREEN_L, SCREEN_W, WATCH_BEZEL_H + 0.5, SCREEN_CORNER_R
    ).translate((0, 0, WATCH_H - SCREEN_INSET))
    watch = watch.cut(screen_pocket)

    # Back central recess (34.60 x 25.80, R4.5)
    recess = rounded_prism_z(
        BACK_RECESS_L, BACK_RECESS_W, BACK_RECESS_DEPTH + 0.1, BACK_RECESS_R
    ).translate((0, 0, -0.05))
    watch = watch.cut(recess)

    # USB-C on -Y face
    usb = (
        cq.Workplane("XY")
        .box(USBC_W, USBC_DEPTH * 2, USBC_H)
        .translate((0, -WATCH_W / 2, WATCH_H * 0.45))
    )
    watch = watch.cut(usb)

    # Two buttons flanking the USB
    for bx in (-BTN_X, BTN_X):
        btn = (
            cq.Workplane("XY")
            .box(BTN_SIZE, BTN_DEPTH * 2, BTN_SIZE)
            .translate((bx, -WATCH_W / 2, WATCH_H * 0.45))
        )
        watch = watch.cut(btn)

    # Mic pinholes (outer ends of -Y face)
    for mx in (-MIC_X, MIC_X):
        mic = (
            cq.Workplane("XZ", origin=(mx, -WATCH_W / 2, WATCH_H * 0.55))
            .circle(MIC_D / 2)
            .extrude(-3)
        )
        watch = watch.cut(mic)

    # Top slot on +Y face (SD / test pads region)
    top_slot = (
        cq.Workplane("XY")
        .box(TOP_SLOT_W, TOP_SLOT_DEPTH * 2, TOP_SLOT_H)
        .translate((0, WATCH_W / 2, WATCH_H * 0.6))
    )
    watch = watch.cut(top_slot)

    # Small side notches on +/-X faces (strap indicator marks)
    for sx in (-1, 1):
        notch = (
            cq.Workplane("XY")
            .box(SIDE_NOTCH_DEPTH * 2, SIDE_NOTCH_W, SIDE_NOTCH_H)
            .translate((sx * WATCH_L / 2, 0, WATCH_H * 0.55))
        )
        watch = watch.cut(notch)

    return watch


# --------------------------------------------------------------------------
# LENS model
# --------------------------------------------------------------------------
def build_lens():
    """Origin at centre of flat face, +Z = dome. Solid of revolution around Z axis."""
    flange_r = LENS_OD / 2
    dome_r = LENS_DOME_BASE_D / 2
    dome_h = LENS_DOME_H

    # Sphere radius from sagitta:  R = (r^2 + h^2) / (2h)
    R = (dome_r ** 2 + dome_h ** 2) / (2 * dome_h)
    sphere_cz = LENS_TOTAL_H - R  # sphere centre Z relative to lens bottom

    # Intermediate point on the sphere (mid-height) for threePointArc
    mid_z = LENS_FLANGE_T + dome_h * 0.5
    dz = mid_z - sphere_cz
    mid_x = math.sqrt(R ** 2 - dz ** 2)

    profile = (
        cq.Workplane("XZ")
        .moveTo(0, 0)
        .lineTo(flange_r, 0)
        .lineTo(flange_r, LENS_FLANGE_T)
        .lineTo(dome_r, LENS_FLANGE_T)
        .threePointArc((mid_x, mid_z), (0, LENS_TOTAL_H))
        .lineTo(0, 0)
        .close()
    )
    lens = profile.revolve(axisStart=(0, 0, 0), axisEnd=(0, 0, 1))

    # Break the sharp flange-OD edge slightly (as real moulded lenses have)
    lens = safe_fillet(lens, ">Z and %CIRCLE", 0.2)
    return lens


# --------------------------------------------------------------------------
# HOUSING model
# --------------------------------------------------------------------------
def build_housing():
    """Slide-in housing.  Origin: bottom-front-outer corner (X = 0 outside face of front wall)."""
    L_out, W_out, H_out = housing_outer()
    cav_l, cav_w, cav_h = watch_cavity()
    slot_d, slot_t = lens_slot()

    # Outer shell -- rectangular box, centred on Y=0, from X=0 to X=L_out, Z=0 to Z=H_out
    outer = (
        cq.Workplane("XY")
        .center(L_out / 2, 0)
        .rect(L_out, W_out)
        .extrude(H_out)
    )
    outer = safe_fillet(outer, "|Z", CORNER_R_HOUSING)
    outer = safe_fillet(outer, "<Z", 0.6)   # small bottom edge fillet
    outer = safe_fillet(outer, ">Z", 0.5)   # small top edge fillet

    # ----- T-channel cutter (removes the watch cavity + flange slot + dome gap)
    # Cross-section in the Y-Z plane, extruded along +X for the cavity length.
    Y_watch = cav_w / 2                             # +/-Y at watch level
    Y_flange = slot_d / 2                           # +/-Y at flange slot level
    Y_dome = (LENS_DOME_BASE_D / 2) + 0.5           # +/-Y at dome gap level
    # Ensure the tab actually covers some of the flange edge:
    tab_reach = Y_flange - Y_dome                   # (should be > TAB_INNER_MARGIN)
    if tab_reach < TAB_INNER_MARGIN:
        Y_dome = Y_flange - TAB_INNER_MARGIN

    Z_watch_bot  = FLOOR_T
    Z_watch_top  = FLOOR_T + cav_h
    Z_flange_top = Z_watch_top + slot_t
    Z_top        = H_out + 0.5                      # overshoot for clean cut

    cross_section = [
        (-Y_watch,  Z_watch_bot),
        (-Y_watch,  Z_watch_top),
        (-Y_flange, Z_watch_top),
        (-Y_flange, Z_flange_top),
        (-Y_dome,   Z_flange_top),
        (-Y_dome,   Z_top),
        ( Y_dome,   Z_top),
        ( Y_dome,   Z_flange_top),
        ( Y_flange, Z_flange_top),
        ( Y_flange, Z_watch_top),
        ( Y_watch,  Z_watch_top),
        ( Y_watch,  Z_watch_bot),
    ]

    cutter = (
        cq.Workplane("YZ")
        .polyline(cross_section)
        .close()
        .extrude(cav_l + 1)                         # slight overshoot at the rear
        .translate((FRONT_WALL_T, 0, 0))            # start just past the front wall
    )

    housing = outer.cut(cutter)

    # Small lead-in chamfer at the rear opening (top and inner edges) for easier insertion
    try:
        housing = housing.faces(">X").edges().chamfer(LEAD_IN_CHAMFER)
    except Exception:
        pass

    return housing


# --------------------------------------------------------------------------
# Assembly (multi-body STEP for visualisation)
# --------------------------------------------------------------------------
def build_assembly():
    """Positioned watch, lens, and housing as a single Assembly (STEP-friendly)."""
    L_out, W_out, H_out = housing_outer()
    cav_l, _, cav_h = watch_cavity()

    housing = build_housing()

    # Watch: back at Z = FLOOR_T, centred in Y, seated against the front wall (X_min = FRONT_WALL_T)
    x_watch_centre = FRONT_WALL_T + WATCH_L / 2
    watch = build_watch().translate((x_watch_centre, 0, FLOOR_T))

    # Lens: flat face rests on watch top (Z = FLOOR_T + WATCH_H)
    lens = build_lens().translate((x_watch_centre, 0, FLOOR_T + WATCH_H))

    asm = cq.Assembly()
    asm.add(housing, name="case",  color=cq.Color(0.85, 0.85, 0.85))
    asm.add(watch,   name="watch", color=cq.Color(0.20, 0.20, 0.20))
    asm.add(lens,    name="lens",  color=cq.Color(0.75, 0.90, 1.00, 0.4))
    return asm


# --------------------------------------------------------------------------
# I/O
# --------------------------------------------------------------------------
def export_stl_step(name, solid):
    stl_path = HERE / f"{name}.stl"
    step_path = HERE / f"{name}.step"
    cq.exporters.export(solid, str(stl_path))
    cq.exporters.export(solid, str(step_path))
    print(f"  {name}.stl  /  {name}.step")


def main():
    print("Building watch reference model ...")
    export_stl_step("watch_amoled_2_06", build_watch())

    print("\nBuilding lens reference model ...")
    export_stl_step("lens_48x14_plano_convex", build_lens())

    print("\nBuilding slide-in housing ...")
    export_stl_step("eye_case", build_housing())

    print("\nBuilding multi-body assembly (STEP only) ...")
    try:
        asm = build_assembly()
        asm.save(str(HERE / "eye_assembly.step"))
        print("  eye_assembly.step")
    except Exception as e:
        print(f"  (skipped: {e})")

    L, W, H = housing_outer()
    cav_l, cav_w, cav_h = watch_cavity()
    slot_d, slot_t = lens_slot()
    print()
    print("---- SUMMARY ----")
    print(f"Housing outer      : {L:.2f} x {W:.2f} x {H:.2f} mm")
    print(f"Watch cavity (in)  : {cav_l:.2f} x {cav_w:.2f} x {cav_h:.2f} mm")
    print(f"Lens flange slot   : {slot_d:.2f} dia  x  {slot_t:.2f} thick")
    print(f"Dome exit gap (Y)  : {(2 * ((LENS_DOME_BASE_D / 2) + 0.5)):.2f} mm")
    print(f"Rear opening at X  = {L:.2f} (open); front wall at X = 0..{FRONT_WALL_T}")
    print()
    print("Assembly (slide in from open rear):")
    print("  1) Watch, screen up, slides in on floor until it hits the front wall.")
    print("  2) Lens, flat face down, slides in on top of the watch under the two")
    print("     side tabs; flange rests on the watch, dome protrudes upward.")
    print("  3) Glue the flat back of the case to the mask interior (no screws).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
