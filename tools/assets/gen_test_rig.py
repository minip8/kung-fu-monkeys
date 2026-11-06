#!/usr/bin/env python3
"""Generates the rigged monkey test model as a binary glTF 2.0 file (.glb).

This is the M2 stand-in for the Blender -> glTF asset pipeline (architecture
section 7): a low-poly placeholder built procedurally, skinned to a 7-joint
skeleton plus an unweighted hand_r joint (where the held banana sits,
architecture v0.53), with the seven animation clips the renderer's
animation layer plays (idle, run, dash, throw, hit, eliminated, charge). It uses the Python 3 standard
library only, and its output is a pure function of the arguments, so the
build regenerates it instead of checking exported glTF in (CLAUDE.md).

The look follows the Jungle Toon board (architecture v0.49, v0.54): a chunky
monkey with a big round head, a squat gi and headband in the player colour,
short arms close to the body and a curled tail; the face (eye patches,
muzzle, eyes, brows, nose, smile) is the board's front view painted on an
open shell over the head (face_paint, face_shell), not modelled.

Conventions (engine/presentation, architecture section 7):
  - model space: +x forward (rotateY(yaw) turns +x onto a heading), +y up,
    +z to the model's right; left is -z;
  - the feet stand on y = 0 and the top of the head is at
    radius * height_over_radius, the size of the capsule placeholder
    (PlayerDef::radius, kMonkeyHeightOverRadius); with the defaults the model
    is in units of the player radius and the renderer scales it per content;
  - joint rest rotations are identity, so each inverse bind matrix is a pure
    translation by minus the joint's bind position.

Usage: gen_test_rig.py --out test_rig.glb [--radius R] [--height-over-radius K]
"""

import argparse
import json
import math
import os
import struct
import sys

# --- small vector and quaternion helpers -------------------------------------


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    n = math.sqrt(dot(a, a))
    return mul(a, 1.0 / n) if n > 0.0 else (0.0, 0.0, 0.0)


def quat_axis_angle(axis, degrees):
    """Quaternion (x, y, z, w) rotating `degrees` about `axis` (right-handed)."""
    h = math.radians(degrees) * 0.5
    a = norm(axis)
    s = math.sin(h)
    return (a[0] * s, a[1] * s, a[2] * s, math.cos(h))


def quat_mul(a, b):
    """Hamilton product a * b: applies b first, then a."""
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def euler(x=0.0, y=0.0, z=0.0):
    """Rotation about z, then y, then x (degrees): q = qx * qy * qz."""
    q = quat_axis_angle((0, 0, 1), z)
    q = quat_mul(quat_axis_angle((0, 1, 0), y), q)
    return quat_mul(quat_axis_angle((1, 0, 0), x), q)


# --- skeleton ----------------------------------------------------------------
# Bind positions in units of the radius, laid out for a height of 3.2 radii;
# rest rotations are identity. Rotation signs, for reference: about +z a
# positive angle tips the top (+y) backward (toward -x) and swings a hanging
# arm (-y) forward (toward +x); about +x a positive angle raises a hanging left
# arm (-z side) outward and a hanging right arm inward.

BASE_HEIGHT_OVER_RADIUS = 3.2

# v0.54 (the design's proportions): a squat gi body and a big head that
# reaches down to the shoulders, short thick arms hanging close to the body.
# The spine pivots at the waist and the head at the neck, inside the body.
JOINTS = [
    # name, parent, bind position (model space, radius units)
    ("root", -1, (0.0, 0.0, 0.0)),
    ("spine", 0, (0.0, 0.7, 0.0)),
    ("head", 1, (0.0, 1.45, 0.0)),
    ("arm_upper_l", 1, (0.0, 1.3, -0.8)),
    ("arm_lower_l", 3, (0.08, 0.98, -0.97)),
    ("arm_upper_r", 1, (0.0, 1.3, 0.8)),
    ("arm_lower_r", 5, (0.08, 0.98, 0.97)),
    # v0.53: the right hand, a leaf no vertex is weighted to; it follows the
    # forearm, and the renderer puts the held banana there.
    ("hand_r", 6, (0.2, 0.68, 1.0)),
]
ROOT, SPINE, HEAD, UPPER_L, LOWER_L, UPPER_R, LOWER_R, HAND_R = range(len(JOINTS))
HANDS = {UPPER_L: (0.2, 0.68, -1.0), UPPER_R: (0.2, 0.68, 1.0)}
# The head: a sphere whose top is the model's top (3.2 radii), a little ahead
# of the body so the gi shows behind it from above.
HEAD_RADIUS = 0.88
HEAD_CENTER = (0.1, BASE_HEIGHT_OVER_RADIUS - HEAD_RADIUS, 0.0)

# --- mesh ----------------------------------------------------------------------


# Vertex colours (COLOR_0, architecture v0.49 "Look"): rgb is the part's own
# colour and alpha the player-colour mask; the renderer draws
# mix(rgb, player colour, alpha), so a part with alpha 1 (the gi, the
# headband) wears the slot's colour and the rest keeps its own.
# The board's colours: fur #8a5a2b, face #f2c98f, ink #2b1d0e (eyes, nose,
# brows and the belt), the eyes' glints white.
FUR = (0.541, 0.353, 0.169, 0.0)
FACE = (0.949, 0.788, 0.561, 0.0)
EYE = (0.169, 0.114, 0.055, 0.0)
GLINT = (1.0, 1.0, 1.0, 0.0)
TINT = (1.0, 1.0, 1.0, 1.0)


class Mesh:
    def __init__(self):
        self.positions = []
        self.normals = []
        self.colors = []
        self.joints = []
        self.weights = []
        self.indices = []

    def vertex(self, p, n, influences, color=TINT):
        """influences: list of (joint, weight); the 4 largest, normalised."""
        merged = {}
        for j, w in influences:
            merged[j] = merged.get(j, 0.0) + w
        infl = sorted(((j, w) for j, w in merged.items() if w > 1e-6),
                      key=lambda jw: (-jw[1], jw[0]))[:4]
        total = sum(w for _, w in infl)
        infl = [(j, w / total) for j, w in infl]
        while len(infl) < 4:
            infl.append((0, 0.0))
        self.positions.append(p)
        self.normals.append(n)
        self.colors.append(color)
        self.joints.append(tuple(j for j, _ in infl))
        self.weights.append(tuple(w for _, w in infl))
        return len(self.positions) - 1


def capsule(mesh, a, b, radius, skin, segments=12, rings=4, color=TINT):
    """A capsule from a to b (a sphere when a == b): hemispheres of `rings`
    latitude bands each, poles as single vertices with triangle fans, counter-
    clockwise seen from outside. skin(p) returns the vertex influences; every
    vertex gets `color`."""
    axis = sub(b, a)
    length = math.sqrt(dot(axis, axis))
    d = norm(axis) if length > 1e-9 else (0.0, 1.0, 0.0)
    helper = (1.0, 0.0, 0.0) if abs(d[0]) < 0.9 else (0.0, 0.0, 1.0)
    u = norm(cross(helper, d))
    v = cross(d, u)

    def add_vertex(p, n):
        return mesh.vertex(p, n, skin(p), color)

    # Latitude rings from the bottom (around a) to the top (around b).
    lats = []
    for i in range(1, rings + 1):  # bottom hemisphere up to the equator at a
        lats.append((a, -math.pi / 2 + (math.pi / 2) * i / rings))
    if length > 1e-9:
        lats.append((b, 0.0))  # the equator again at b: the cylinder
    for i in range(1, rings):  # top hemisphere, excluding the pole
        lats.append((b, (math.pi / 2) * i / rings))

    bottom = add_vertex(sub(a, mul(d, radius)), mul(d, -1.0))
    ring_ids = []
    for center, phi in lats:
        ids = []
        for s in range(segments):
            theta = 2.0 * math.pi * s / segments
            radial = add(mul(u, math.cos(theta)), mul(v, math.sin(theta)))
            n = add(mul(radial, math.cos(phi)), mul(d, math.sin(phi)))
            ids.append(add_vertex(add(center, mul(n, radius)), n))
        ring_ids.append(ids)
    top = add_vertex(add(b, mul(d, radius)), d)

    # theta turns from u toward v and u x v = d, so seen from outside the
    # quad (lower s, lower s+1, upper s+1, upper s) is counter-clockwise.
    first = ring_ids[0]
    for s in range(segments):
        s1 = (s + 1) % segments
        mesh.indices += [bottom, first[s1], first[s]]
    for lower, upper in zip(ring_ids, ring_ids[1:]):
        for s in range(segments):
            s1 = (s + 1) % segments
            mesh.indices += [lower[s], lower[s1], upper[s1], lower[s], upper[s1], upper[s]]
    last = ring_ids[-1]
    for s in range(segments):
        s1 = (s + 1) % segments
        mesh.indices += [last[s], last[s1], top]


def ellipsoid(mesh, center, radii, skin, color, segments=12, rings=6, basis=None):
    """An ellipsoid: a unit sphere of `rings` latitude bands scaled by radii
    along `basis` (three orthonormal axes; default x, y, z), normals by the
    inverse scale, counter-clockwise seen from outside (a positive scale and a
    right-handed basis keep the winding).

    `color` is a colour, or a function of the unit-sphere direction (in the
    basis) giving each triangle the colour at its centre: a vertex is shared
    only by triangles of its own colour, so regions such as the face meet
    with a crisp edge and no geometry of their own (no ink hull around
    them)."""
    bx, by, bz = basis or ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
    color_at = color if callable(color) else (lambda _d: color)

    def unit(i, j):
        if i == 0:
            return (0.0, -1.0, 0.0)
        if i == rings:
            return (0.0, 1.0, 0.0)
        phi = -math.pi / 2 + math.pi * i / rings
        theta = 2.0 * math.pi * (j % segments) / segments
        # theta turns from +x toward -z so that, with +y up, a quad
        # (lower j, lower j+1, upper j+1, upper j) is counter-clockwise.
        return (math.cos(phi) * math.cos(theta), math.sin(phi), -math.cos(phi) * math.sin(theta))

    cache = {}

    def point(i, j, c):
        key = (i, 0 if i in (0, rings) else j % segments, c)
        if key not in cache:
            n = unit(i, j)
            local = (radii[0] * n[0], radii[1] * n[1], radii[2] * n[2])
            p = add(center, add(add(mul(bx, local[0]), mul(by, local[1])), mul(bz, local[2])))
            g = (n[0] / radii[0], n[1] / radii[1], n[2] / radii[2])
            normal = norm(add(add(mul(bx, g[0]), mul(by, g[1])), mul(bz, g[2])))
            cache[key] = mesh.vertex(p, normal, skin(p), c)
        return cache[key]

    def tri(a, b, c):
        d = norm(add(add(unit(*a), unit(*b)), unit(*c)))
        col = color_at(d)
        mesh.indices += [point(*a, col), point(*b, col), point(*c, col)]

    for j in range(segments):
        tri((0, 0), (1, j + 1), (1, j))
        tri((rings - 1, j), (rings - 1, j + 1), (rings, 0))
    for i in range(1, rings - 1):
        for j in range(segments):
            tri((i, j), (i, j + 1), (i + 1, j + 1))
            tri((i, j), (i + 1, j + 1), (i + 1, j))


def ring(mesh, center, rx, rz, tube_out, tube_up, skin, color, segments=24, sides=8, tilt=0.0):
    """A band around the +y axis through `center` (the headband, the belt): an
    elliptic ring of half-axes rx, rz whose tube is an ellipse tube_out wide
    and tube_up tall, counter-clockwise from outside. `tilt` (radians) turns
    the tube's section so that its wide axis tips up by that much: a band on
    a sphere at latitude L, tilted by L, lies along the surface."""
    rows = []
    ct, st = math.cos(tilt), math.sin(tilt)
    for i in range(segments):
        theta = 2.0 * math.pi * i / segments
        c = add(center, (rx * math.cos(theta), 0.0, rz * math.sin(theta)))
        out = norm((rz * math.cos(theta), 0.0, rx * math.sin(theta)))  # the ellipse's normal
        e_out = add(mul(out, ct), (0.0, st, 0.0))
        e_up = add(mul(out, -st), (0.0, ct, 0.0))
        row = []
        for j in range(sides):
            phi = 2.0 * math.pi * j / sides
            cp, sp = math.cos(phi), math.sin(phi)
            p = add(c, add(mul(e_out, tube_out * cp), mul(e_up, tube_up * sp)))
            n = norm(add(mul(e_out, cp / tube_out), mul(e_up, sp / tube_up)))
            row.append(mesh.vertex(p, n, skin(p), color))
        rows.append(row)
    for i in range(segments):
        i1 = (i + 1) % segments
        for j in range(sides):
            j1 = (j + 1) % sides
            a, b, c, d = rows[i][j], rows[i1][j], rows[i1][j1], rows[i][j1]
            mesh.indices += [a, c, b, a, d, c]


def bezier(p0, p1, p2, p3, u):
    v = 1.0 - u
    return add(add(mul(p0, v * v * v), mul(p1, 3.0 * v * v * u)),
               add(mul(p2, 3.0 * v * u * u), mul(p3, u * u * u)))


def tube(mesh, points, radius, skin, color, segments=8, rings=2):
    """Capsules chained through `points`; equal radii make the joints seamless."""
    for a, b in zip(points, points[1:]):
        capsule(mesh, a, b, radius, skin, segments, rings, color)


def smoothstep(e0, e1, x):
    t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


# The board's face (JungleToon, the monkey's front view: the head a circle of
# radius 64 at (100, 90)), in head radii seen from the front: u to the
# viewer's right (the model's left, -z), v up. Each entry paints where it covers, later entries
# over earlier ones.
def _bezier2(p0, p1, p2, steps=12):
    out = []
    for i in range(steps + 1):
        t = i / steps
        out.append(((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
                    (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]))
    return out


def _board(x, y):
    """A board point (svg pixels) in head radii (u right, v up)."""
    return ((x - 100.0) / 64.0, (90.0 - y) / 64.0)


def _near_polyline(u, v, points, half_width):
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        dx, dy = x1 - x0, y1 - y0
        t = max(0.0, min(1.0, ((u - x0) * dx + (v - y0) * dy) / (dx * dx + dy * dy)))
        if (u - x0 - t * dx) ** 2 + (v - y0 - t * dy) ** 2 <= half_width * half_width:
            return True
    return False


def _in_ellipse(u, v, cx, cy, rx, ry):
    return ((u - cx) / rx) ** 2 + ((v - cy) / ry) ** 2 <= 1.0


_SMILE = [_board(x, y) for x, y in _bezier2((84.0, 124.0), (100.0, 138.0), (116.0, 124.0))]
_BROWS = [[_board(70.0, 76.0), _board(93.0, 82.0)], [_board(130.0, 76.0), _board(107.0, 82.0)]]


def face_paint(u, v):
    """The face's colour at (u, v), or None where the fur shows."""
    color = None
    for x in (80.0, 120.0):  # the two eye patches
        cu, cv = _board(x, 88.0)
        if _in_ellipse(u, v, cu, cv, 25.0 / 64.0, 25.0 / 64.0):
            color = FACE
    cu, cv = _board(100.0, 118.0)  # the muzzle
    if _in_ellipse(u, v, cu, cv, 42.0 / 64.0, 27.0 / 64.0):
        color = FACE
    for x in (83.0, 117.0):  # eyes
        cu, cv = _board(x, 92.0)
        if _in_ellipse(u, v, cu, cv, 8.0 / 64.0, 11.0 / 64.0):
            color = EYE
    for x in (86.0, 120.0):  # their glints
        cu, cv = _board(x, 88.0)
        if _in_ellipse(u, v, cu, cv, 3.0 / 64.0, 3.0 / 64.0):
            color = GLINT
    cu, cv = _board(100.0, 110.0)  # the nose
    if _in_ellipse(u, v, cu, cv, 7.0 / 64.0, 4.0 / 64.0):
        color = EYE
    if _near_polyline(u, v, _SMILE, 2.0 / 64.0):
        color = EYE
    for brow in _BROWS:
        if _near_polyline(u, v, brow, 2.5 / 64.0):
            color = EYE
    return color


# The face looks this far above the horizon (degrees): the board draws it
# straight on, but the game camera looks down at 55 degrees.
FACE_TILT = 16.0


def face_shell(mesh, center, radius, lift, k, skin, yaw_cells=72, pitch_cells=56):
    """The face as open patches of a sphere `lift` above the head: a grid in
    the face's own frame (tilted up by FACE_TILT), each cell painted with
    face_paint at its centre seen from the front, and only painted cells
    emitted. Open and facing out, the patches get no ink hull, so the face
    meets the fur without a rim; the eyes, nose, smile and brows are paint,
    not geometry."""
    tilt = math.radians(FACE_TILT)
    ct, st = math.cos(tilt), math.sin(tilt)
    yaw0, yaw1, pitch0, pitch1 = -1.15, 1.15, -1.0, 0.75

    def direction(i, j):
        """The model-space direction of grid corner (i, j): the face frame's
        (yaw, pitch) turned up by the tilt."""
        yaw = yaw0 + (yaw1 - yaw0) * i / yaw_cells
        pitch = pitch0 + (pitch1 - pitch0) * j / pitch_cells
        cp = math.cos(pitch)
        x, y, z = cp * math.cos(yaw), math.sin(pitch), cp * math.sin(yaw)
        return (x * ct - y * st, x * st + y * ct, z)

    cache = {}

    def corner(i, j, color):
        key = (i, j, color)
        if key not in cache:
            d = direction(i, j)
            p = add(center, (d[0] * (radius + lift), d[1] * (radius + lift) * k,
                             d[2] * (radius + lift)))
            cache[key] = mesh.vertex(p, d, skin(p), color)
        return cache[key]

    for i in range(yaw_cells):
        for j in range(pitch_cells):
            yaw = yaw0 + (yaw1 - yaw0) * (i + 0.5) / yaw_cells
            pitch = pitch0 + (pitch1 - pitch0) * (j + 0.5) / pitch_cells
            # Seen from the front the model's right (+z) is on the viewer's left.
            color = face_paint(-math.cos(pitch) * math.sin(yaw), math.sin(pitch))
            if color is None:
                continue
            a, b, c, d = (corner(i, j, color), corner(i + 1, j, color),
                          corner(i + 1, j + 1, color), corner(i, j + 1, color))
            # Seen from outside, yaw turns toward +z and pitch up: (a, d, c)
            # and (a, c, b) are counter-clockwise.
            mesh.indices += [a, d, c, a, c, b]


def build_mesh(r, k):
    """r: radius; k: vertical stretch (height_over_radius / 3.2)."""
    mesh = Mesh()

    def s(p):
        return (p[0] * r, p[1] * r * k, p[2] * r)

    def unscale(p):
        return (p[0] / r, p[1] / (r * k), p[2] / r)

    def sr(radii):
        return (radii[0] * r, radii[1] * r * k, radii[2] * r)

    def body_skin(p):
        # The gi follows the root at the hem and the spine above the waist.
        w = smoothstep(0.3, 0.95, unscale(p)[1])
        return [(ROOT, 1.0 - w), (SPINE, w)]

    def head_skin(_p):
        return [(HEAD, 1.0)]

    def root_skin(_p):
        return [(ROOT, 1.0)]

    def limb_skin(start, end, joint, parent_share, child_share):
        """Mostly `joint`; blends from the parent near `start` and toward the
        child near `end` (so both sides of the elbow agree)."""
        axis = sub(end, start)
        length2 = dot(axis, axis)

        def skin(p):
            t = min(max(dot(sub(unscale(p), start), axis) / length2, 0.0), 1.0)
            out = [(joint, 1.0)]
            if child_share is not None:
                c, share = child_share
                w = share * smoothstep(0.7, 1.0, t)
                out = [(joint, 1.0 - w), (c, w)]
            if parent_share is not None:
                pj, share = parent_share
                w = share * (1.0 - smoothstep(0.0, 0.3, t))
                out = [(j, ww * (1.0 - w)) for j, ww in out] + [(pj, w)]
            return out

        return skin

    def on_head(direction, lift=0.0):
        """The point of the head's surface (plus `lift`) along `direction`."""
        d = norm(direction)
        return add(HEAD_CENTER, mul(d, HEAD_RADIUS + lift))

    # v0.49 (the Jungle Toon character), v0.54 the board's proportions: a
    # chunky monkey whose round head is about as wide as its body and over
    # half its height, a squat gi in the player colour with an ink belt,
    # short thick furry arms hanging close to the body, round ears, a pale
    # face, a headband in the player colour with two tails, and a tail that
    # curls out behind. The body's widest point stays inside the contact
    # radius (the footprint and the blob shadow mark the hitbox); the head
    # grew instead. The top of the head stays at 3.2 radii (the capsule's
    # height).
    # Body: a squat ellipsoid, wider than deep, from the hem to the shoulders.
    ellipsoid(mesh, s((-0.05, 0.78, 0.0)), sr((0.8, 0.72, 0.92)), body_skin, TINT, 20, 8)
    # The belt where the gi is widest below the waist (the ellipse there).
    belt_y = 0.52
    f = math.sqrt(1.0 - ((belt_y - 0.78) / 0.72) ** 2)
    ring(mesh, s((-0.05, belt_y, 0.0)), 0.8 * f * r, 0.92 * f * r, 0.035 * r, 0.075 * r * k,
         body_skin, EYE, 28, 6)
    # Feet peeking out under the hem (the lowest point is y = 0).
    for z in (-0.4, 0.4):
        ellipsoid(mesh, s((0.3, 0.14, z)), sr((0.34, 0.14, 0.24)), root_skin, FUR, 12, 4)
    # The tail: out of the back of the gi, round to the right and curling up.
    tail = [bezier((-0.6, 0.45, 0.1), (-1.35, 0.3, 0.1), (-1.6, 0.45, 0.75), (-1.15, 0.9, 0.9),
                   i / 8.0) for i in range(9)]
    tube(mesh, [s(p) for p in tail], 0.15 * r, root_skin, FUR, 8, 2)
    # Head: one furry sphere; the face is painted on a shell just above it.
    ellipsoid(mesh, s(HEAD_CENTER), sr((HEAD_RADIUS,) * 3), head_skin, FUR, 40, 20)
    face_shell(mesh, s(HEAD_CENTER), HEAD_RADIUS * r, 0.006 * r, k, head_skin)

    def surface_basis(normal):
        """normal, the up direction along the surface, and their cross."""
        up = norm(sub((0.0, 1.0, 0.0), mul(normal, normal[1])))
        return (normal, up, cross(normal, up))

    # Round ears at the sides of the head, pale inside (toward the front).
    for z in (-1.0, 1.0):
        ear_axis = norm((0.5, 0.1, 0.86 * z))

        def ear_color(d):
            # d is in the ear's own basis (x: its axis).
            return FACE if d[0] > math.cos(0.62) else FUR

        ellipsoid(mesh, s(add(HEAD_CENTER, (-0.02, 0.06, 0.86 * z))), sr((0.24, 0.34, 0.31)),
                  head_skin, ear_color, 20, 10, surface_basis(ear_axis))
    # The headband around the crown, a knot at the back and two ribbon tails
    # hanging out behind it.
    # The band's section lies along the sphere at its latitude: its thickness
    # along the head's normal, its width along the surface, so its ink hull
    # leaves the head at a steep angle (a clean line, not a grazing speckle).
    band_lat = math.asin(0.64)
    band_c = HEAD_RADIUS + 0.03  # the section's centre, along the normal
    ring(mesh, s(add(HEAD_CENTER, (0.0, band_c * math.sin(band_lat), 0.0))),
         band_c * math.cos(band_lat) * r, band_c * math.cos(band_lat) * r, 0.065 * r, 0.12 * r,
         head_skin, TINT, 40, 10, band_lat)
    knot_dir = norm((-0.68, 0.6, 0.38))
    knot = on_head(knot_dir, 0.04)
    ellipsoid(mesh, s(knot), sr((0.13, 0.13, 0.13)), head_skin, TINT, 8, 4)
    for t in (norm((-0.7, -0.3, 0.55)), norm((-0.8, -0.55, 0.1))):
        thick = norm(sub(knot_dir, mul(t, dot(knot_dir, t))))
        ellipsoid(mesh, s(add(knot, mul(t, 0.27))), sr((0.3, 0.035, 0.12)), head_skin, TINT,
                  10, 4, (t, thick, cross(t, thick)))
    # Arms: short thick furry stubs, upper (shoulder -> elbow) and lower
    # (elbow -> hand), and a round mitt of a hand.
    for upper, lower in ((UPPER_L, LOWER_L), (UPPER_R, LOWER_R)):
        shoulder, elbow, hand = JOINTS[upper][2], JOINTS[lower][2], HANDS[upper]
        capsule(mesh, s(shoulder), s(elbow), 0.23 * r,
                limb_skin(shoulder, elbow, upper, (SPINE, 0.3), (lower, 0.5)), 12, 3, FUR)
        capsule(mesh, s(elbow), s(hand), 0.23 * r,
                limb_skin(elbow, hand, lower, (upper, 0.5), None), 12, 3, FUR)
        ellipsoid(mesh, s(hand), sr((0.26, 0.26, 0.26)), lambda _p, j=lower: [(j, 1.0)], FUR,
                  12, 5)
    return mesh


def check_mesh(mesh):
    """Fails loudly on a malformed mesh: 16-bit indices, normalised weights,
    no degenerate or clockwise triangles."""
    assert len(mesh.positions) < 65536, "too many vertices for 16-bit indices"
    for w in mesh.weights:
        assert abs(sum(w) - 1.0) < 1e-5, w
    for i in range(0, len(mesh.indices), 3):
        tri = mesh.indices[i:i + 3]
        a, b, c = (mesh.positions[t] for t in tri)
        n = cross(sub(b, a), sub(c, a))
        assert dot(n, n) > 1e-14, "degenerate triangle"
        avg = add(add(mesh.normals[tri[0]], mesh.normals[tri[1]]), mesh.normals[tri[2]])
        assert dot(n, avg) > 0.0, "clockwise triangle"


# --- animation clips -------------------------------------------------------------
# Each clip is (name, duration in seconds, key count, {joint: {path: f(u)}})
# with u in [0, 1] over the clip, keys evenly spaced. The looping clips (idle,
# run) end where they start; the others play once and hold their last key
# (the animation layer decides which loop, not the file).


def wave(u, cycles=1.0, phase=0.0):
    return math.sin(2.0 * math.pi * (cycles * u + phase))


def ease(u, start, end):
    return smoothstep(start, end, u)


def clips(r, k):
    spine_rest = JOINTS[SPINE][2]
    return [
        ("idle", 2.0, 17, {
            SPINE: {
                "translation": lambda u: (spine_rest[0] * r, (spine_rest[1] + 0.04 * wave(u)) * r * k,
                                          spine_rest[2] * r),
                "rotation": lambda u: euler(z=1.5 * wave(u)),
            },
            HEAD: {"rotation": lambda u: euler(z=-2.0 * wave(u, phase=0.1))},
            UPPER_L: {"rotation": lambda u: euler(x=6.0 + 3.0 * wave(u), z=2.0 * wave(u))},
            UPPER_R: {"rotation": lambda u: euler(x=-6.0 - 3.0 * wave(u), z=2.0 * wave(u))},
        }),
        ("run", 0.5, 17, {
            ROOT: {
                "translation": lambda u: (0.0, 0.08 * r * (1.0 - math.cos(4.0 * math.pi * u)), 0.0),
                "rotation": lambda u: euler(z=-12.0 + 2.0 * wave(u, 2.0)),
            },
            SPINE: {"rotation": lambda u: euler(y=10.0 * wave(u))},
            HEAD: {"rotation": lambda u: euler(y=-8.0 * wave(u), z=8.0)},
            UPPER_L: {"rotation": lambda u: euler(x=10.0, z=55.0 * wave(u))},
            LOWER_L: {"rotation": lambda u: euler(z=35.0 + 15.0 * wave(u))},
            UPPER_R: {"rotation": lambda u: euler(x=-10.0, z=-55.0 * wave(u))},
            LOWER_R: {"rotation": lambda u: euler(z=35.0 - 15.0 * wave(u))},
        }),
        ("dash", 0.25, 6, {
            ROOT: {"rotation": lambda u: euler(z=-35.0 * ease(u, 0.0, 0.5))},
            HEAD: {"rotation": lambda u: euler(z=18.0 * ease(u, 0.0, 0.5))},
            UPPER_L: {"rotation": lambda u: euler(x=25.0 * ease(u, 0.0, 0.5),
                                                  z=-75.0 * ease(u, 0.0, 0.5))},
            UPPER_R: {"rotation": lambda u: euler(x=-25.0 * ease(u, 0.0, 0.5),
                                                  z=-75.0 * ease(u, 0.0, 0.5))},
            LOWER_L: {"rotation": lambda u: euler(z=-20.0 * ease(u, 0.0, 0.5))},
            LOWER_R: {"rotation": lambda u: euler(z=-20.0 * ease(u, 0.0, 0.5))},
        }),
        ("throw", 0.35, 8, {
            SPINE: {"rotation": lambda u: euler(
                y=25.0 * ease(u, 0.0, 0.3) - 50.0 * ease(u, 0.3, 0.6) + 15.0 * ease(u, 0.6, 1.0))},
            UPPER_R: {"rotation": lambda u: euler(
                x=-60.0 * ease(u, 0.0, 0.3) + 30.0 * ease(u, 0.6, 1.0),
                z=-70.0 * ease(u, 0.0, 0.3) + 170.0 * ease(u, 0.3, 0.6) - 60.0 * ease(u, 0.6, 1.0))},
            LOWER_R: {"rotation": lambda u: euler(
                z=60.0 * ease(u, 0.0, 0.3) - 60.0 * ease(u, 0.3, 0.6))},
            UPPER_L: {"rotation": lambda u: euler(x=20.0 * ease(u, 0.0, 0.4),
                                                  z=30.0 * ease(u, 0.2, 0.6))},
        }),
        ("hit", 0.35, 8, {
            SPINE: {"rotation": lambda u: euler(
                z=28.0 * ease(u, 0.0, 0.2) - 18.0 * ease(u, 0.3, 1.0))},
            HEAD: {"rotation": lambda u: euler(
                z=22.0 * ease(u, 0.0, 0.25) - 16.0 * ease(u, 0.4, 1.0))},
            UPPER_L: {"rotation": lambda u: euler(
                x=60.0 * ease(u, 0.0, 0.2) - 40.0 * ease(u, 0.4, 1.0))},
            UPPER_R: {"rotation": lambda u: euler(
                x=-60.0 * ease(u, 0.0, 0.2) + 40.0 * ease(u, 0.4, 1.0))},
        }),
        ("eliminated", 0.8, 9, {
            ROOT: {
                "translation": lambda u: (0.0, -0.3 * r * ease(u, 0.2, 1.0), 0.0),
                "rotation": lambda u: euler(z=-12.0 * ease(u, 0.0, 0.6)),
            },
            SPINE: {"rotation": lambda u: euler(z=-50.0 * ease(u, 0.0, 0.8))},
            HEAD: {"rotation": lambda u: euler(x=15.0 * ease(u, 0.4, 1.0),
                                               z=-35.0 * ease(u, 0.2, 1.0))},
            UPPER_L: {"rotation": lambda u: euler(x=15.0 * ease(u, 0.0, 0.6),
                                                  z=25.0 * ease(u, 0.0, 0.8))},
            UPPER_R: {"rotation": lambda u: euler(x=-15.0 * ease(u, 0.0, 0.6),
                                                  z=25.0 * ease(u, 0.0, 0.8))},
        }),
        # v0.53, the charged throw's wind-up. Not played over time: the
        # renderer samples it at charge x duration, so u is the charge (0 a
        # tap, 1 full). The banana arm swings back and up until the hand is
        # behind the head (v0.54: the short arm cannot clear the big head, so
        # at full charge it also swings out to the side, keeping the banana in
        # view beside the head), the torso twists the right shoulder back and
        # leans, the head turns to keep facing the throw and the free arm
        # reaches forward. The release skips the throw clip's own wind-up in
        # proportion to the charge.
        ("charge", 1.0, 9, {
            SPINE: {"rotation": lambda u: euler(y=-30.0 * ease(u, 0.0, 1.0),
                                                z=-8.0 * ease(u, 0.2, 1.0))},
            HEAD: {"rotation": lambda u: euler(y=25.0 * ease(u, 0.0, 1.0),
                                               z=5.0 * ease(u, 0.3, 1.0))},
            UPPER_R: {"rotation": lambda u: euler(x=20.0 * ease(u, 0.3, 1.0),
                                                  z=-150.0 * ease(u, 0.0, 1.0))},
            LOWER_R: {"rotation": lambda u: euler(z=-15.0 * ease(u, 0.3, 1.0))},
            UPPER_L: {"rotation": lambda u: euler(x=20.0 * ease(u, 0.0, 1.0),
                                                  z=50.0 * ease(u, 0.1, 1.0))},
        }),
    ]


# --- glTF writer ---------------------------------------------------------------------

FLOAT, UBYTE, USHORT = 5126, 5121, 5123
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963


class Bin:
    def __init__(self):
        self.data = bytearray()
        self.views = []
        self.accessors = []

    def view(self, payload, target=None):
        while len(self.data) % 4:
            self.data.append(0)
        v = {"buffer": 0, "byteOffset": len(self.data), "byteLength": len(payload)}
        if target is not None:
            v["target"] = target
        self.data += payload
        self.views.append(v)
        return len(self.views) - 1

    def accessor(self, view, component, count, kind, minmax=None):
        a = {"bufferView": view, "componentType": component, "count": count, "type": kind}
        if minmax is not None:
            a["min"], a["max"] = minmax
        self.accessors.append(a)
        return len(self.accessors) - 1


def r6(x):
    # Rounded so the JSON text is the same on every platform.
    v = round(x, 6)
    return 0.0 if v == 0.0 else v


def floats(values):
    return struct.pack("<%df" % len(values), *values)


def vec_accessor(b, rows, kind, target=None, minmax=False):
    rows = [[r6(c) for c in row] for row in rows]
    mm = None
    if minmax:
        n = len(rows[0])
        mm = ([min(row[i] for row in rows) for i in range(n)],
              [max(row[i] for row in rows) for i in range(n)])
    flat = [c for row in rows for c in row]
    return b.accessor(b.view(floats(flat), target), FLOAT, len(rows), kind, mm)


def build_gltf(radius, stretch):
    mesh = build_mesh(radius, stretch)
    check_mesh(mesh)
    b = Bin()

    def bind(p):
        return (p[0] * radius, p[1] * radius * stretch, p[2] * radius)

    pos = vec_accessor(b, mesh.positions, "VEC3", ARRAY_BUFFER, minmax=True)
    nrm = vec_accessor(b, mesh.normals, "VEC3", ARRAY_BUFFER)
    col = vec_accessor(b, mesh.colors, "VEC4", ARRAY_BUFFER)
    jnt = b.accessor(b.view(bytes(j for row in mesh.joints for j in row), ARRAY_BUFFER),
                     UBYTE, len(mesh.joints), "VEC4")
    wgt = vec_accessor(b, mesh.weights, "VEC4", ARRAY_BUFFER)
    idx = b.accessor(b.view(struct.pack("<%dH" % len(mesh.indices), *mesh.indices),
                            ELEMENT_ARRAY_BUFFER), USHORT, len(mesh.indices), "SCALAR")

    # Inverse bind matrices (column-major): a translation by -bind position.
    ibms = []
    for _, _, p in JOINTS:
        x, y, z = bind(p)
        ibms.append([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1])
    ibm = vec_accessor(b, ibms, "MAT4")

    nodes = []
    for i, (name, parent, p) in enumerate(JOINTS):
        local = bind(p) if parent < 0 else sub(bind(p), bind(JOINTS[parent][2]))
        node = {"name": name, "translation": [r6(c) for c in local]}
        children = [c for c, j in enumerate(JOINTS) if j[1] == i]
        if children:
            node["children"] = children
        nodes.append(node)
    mesh_node = len(nodes)
    nodes.append({"name": "monkey", "mesh": 0, "skin": 0})

    animations = []
    for name, duration, keys, tracks in clips(radius, stretch):
        times = [duration * k / (keys - 1) for k in range(keys)]
        input_acc = b.accessor(b.view(floats([r6(t) for t in times])), FLOAT, keys, "SCALAR",
                               ([r6(times[0])], [r6(times[-1])]))
        samplers, channels = [], []
        for joint in sorted(tracks):
            for path in ("translation", "rotation"):
                f = tracks[joint].get(path)
                if f is None:
                    continue
                values = [f(k / (keys - 1)) for k in range(keys)]
                if path == "rotation":
                    # Keep consecutive keys in one hemisphere for interpolation.
                    for k in range(1, keys):
                        if sum(a * c for a, c in zip(values[k - 1], values[k])) < 0.0:
                            values[k] = tuple(-c for c in values[k])
                out = vec_accessor(b, values, "VEC4" if path == "rotation" else "VEC3")
                samplers.append({"input": input_acc, "output": out, "interpolation": "LINEAR"})
                channels.append({"sampler": len(samplers) - 1,
                                 "target": {"node": joint, "path": path}})
        animations.append({"name": name, "samplers": samplers, "channels": channels})

    gltf = {
        "asset": {"version": "2.0", "generator": "kfm tools/assets/gen_test_rig.py"},
        "scene": 0,
        "scenes": [{"name": "test_rig", "nodes": [ROOT, mesh_node]}],
        "nodes": nodes,
        "meshes": [{"name": "monkey", "primitives": [{
            "attributes": {"POSITION": pos, "NORMAL": nrm, "COLOR_0": col, "JOINTS_0": jnt,
                           "WEIGHTS_0": wgt},
            "indices": idx, "mode": 4}]}],
        "skins": [{"name": "monkey_rig", "joints": list(range(len(JOINTS))),
                   "inverseBindMatrices": ibm, "skeleton": ROOT}],
        "animations": animations,
        "buffers": [{"byteLength": len(b.data)}],
        "bufferViews": b.views,
        "accessors": b.accessors,
    }
    return gltf, bytes(b.data)


def write_glb(path, gltf, binary):
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    js += b" " * ((4 - len(js) % 4) % 4)
    binary += b"\0" * ((4 - len(binary) % 4) % 4)
    total = 12 + 8 + len(js) + 8 + len(binary)
    out = struct.pack("<4sII", b"glTF", 2, total)
    out += struct.pack("<I4s", len(js), b"JSON") + js
    out += struct.pack("<I4s", len(binary), b"BIN\0") + binary
    directory = os.path.dirname(path)
    if directory:
        os.makedirs(directory, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", required=True, help="output .glb path")
    parser.add_argument("--radius", type=float, default=1.0,
                        help="player radius the model is sized from (default 1: radius units)")
    parser.add_argument("--height-over-radius", type=float, default=BASE_HEIGHT_OVER_RADIUS,
                        help="model height / radius (kMonkeyHeightOverRadius, default 3.2)")
    args = parser.parse_args(argv)
    if args.radius <= 0.0 or args.height_over_radius < 2.0:
        parser.error("--radius must be positive and --height-over-radius at least 2")
    gltf, binary = build_gltf(args.radius, args.height_over_radius / BASE_HEIGHT_OVER_RADIUS)
    write_glb(args.out, gltf, binary)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
