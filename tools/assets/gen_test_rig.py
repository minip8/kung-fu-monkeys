#!/usr/bin/env python3
"""Generates the rigged monkey test model as a binary glTF 2.0 file (.glb).

This is the M2 stand-in for the Blender -> glTF asset pipeline (architecture
section 7): a low-poly placeholder built procedurally, skinned to a 7-joint
skeleton plus an unweighted hand_r joint (where the held banana sits,
architecture v0.53), with the seven animation clips the renderer's
animation layer plays (idle, run, dash, throw, hit, eliminated, charge). It uses the Python 3 standard
library only, and its output is a pure function of the arguments, so the
build regenerates it instead of checking exported glTF in (CLAUDE.md).

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

JOINTS = [
    # name, parent, bind position (model space, radius units)
    ("root", -1, (0.0, 0.0, 0.0)),
    ("spine", 0, (0.0, 1.0, 0.0)),
    ("head", 1, (0.0, 2.3, 0.0)),
    ("arm_upper_l", 1, (0.0, 1.85, -0.85)),
    ("arm_lower_l", 3, (0.1, 1.35, -1.2)),
    ("arm_upper_r", 1, (0.0, 1.85, 0.85)),
    ("arm_lower_r", 5, (0.1, 1.35, 1.2)),
    # v0.53: the right hand, a leaf no vertex is weighted to; it follows the
    # forearm, and the renderer puts the held banana there.
    ("hand_r", 6, (0.3, 0.95, 1.25)),
]
ROOT, SPINE, HEAD, UPPER_L, LOWER_L, UPPER_R, LOWER_R, HAND_R = range(len(JOINTS))
HANDS = {UPPER_L: (0.3, 0.95, -1.25), UPPER_R: (0.3, 0.95, 1.25)}

# --- mesh ----------------------------------------------------------------------


class Mesh:
    def __init__(self):
        self.positions = []
        self.normals = []
        self.joints = []
        self.weights = []
        self.indices = []

    def vertex(self, p, n, influences):
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
        self.joints.append(tuple(j for j, _ in infl))
        self.weights.append(tuple(w for _, w in infl))
        return len(self.positions) - 1


def capsule(mesh, a, b, radius, skin, segments=12, rings=4):
    """A capsule from a to b (a sphere when a == b): hemispheres of `rings`
    latitude bands each, poles as single vertices with triangle fans, counter-
    clockwise seen from outside. skin(p) returns the vertex influences."""
    axis = sub(b, a)
    length = math.sqrt(dot(axis, axis))
    d = norm(axis) if length > 1e-9 else (0.0, 1.0, 0.0)
    helper = (1.0, 0.0, 0.0) if abs(d[0]) < 0.9 else (0.0, 0.0, 1.0)
    u = norm(cross(helper, d))
    v = cross(d, u)

    def add_vertex(p, n):
        return mesh.vertex(p, n, skin(p))

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


def smoothstep(e0, e1, x):
    t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def build_mesh(r, k):
    """r: radius; k: vertical stretch (height_over_radius / 3.2)."""
    mesh = Mesh()

    def s(p):
        return (p[0] * r, p[1] * r * k, p[2] * r)

    def unscale(p):
        return (p[0] / r, p[1] / (r * k), p[2] / r)

    def body_skin(p):
        y = unscale(p)[1]
        if y < 2.0:
            w = smoothstep(0.6, 1.4, y)
            return [(ROOT, 1.0 - w), (SPINE, w)]
        w = smoothstep(2.0, 2.45, y)
        return [(SPINE, 1.0 - w), (HEAD, w)]

    def head_skin(_p):
        return [(HEAD, 1.0)]

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

    # Body: a capsule from the feet to the shoulders, radius 0.9. The capsule
    # caps stay spherical; only the spacing of the centres follows k.
    capsule(mesh, s((0.0, 0.9 / k, 0.0)), s((0.0, 2.4 - 0.9 / k, 0.0)), 0.9 * r, body_skin, 16, 4)
    # Head, muzzle (it shows which way the model faces) and ears.
    capsule(mesh, s((0.05, 2.55, 0.0)), s((0.05, 2.55, 0.0)), 0.65 * r, head_skin, 14, 4)
    capsule(mesh, s((0.55, 2.42, 0.0)), s((0.55, 2.42, 0.0)), 0.32 * r, head_skin, 10, 3)
    for z in (-0.62, 0.62):
        capsule(mesh, s((0.0, 2.78, z)), s((0.0, 2.78, z)), 0.2 * r, head_skin, 8, 2)
    # Arms: upper (shoulder -> elbow) and lower (elbow -> hand) stubs.
    for upper, lower in ((UPPER_L, LOWER_L), (UPPER_R, LOWER_R)):
        shoulder, elbow, hand = JOINTS[upper][2], JOINTS[lower][2], HANDS[upper]
        capsule(mesh, s(shoulder), s(elbow), 0.22 * r,
                limb_skin(shoulder, elbow, upper, (SPINE, 0.3), (lower, 0.5)), 10, 3)
        capsule(mesh, s(elbow), s(hand), 0.2 * r,
                limb_skin(elbow, hand, lower, (upper, 0.5), None), 10, 3)
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
        # above and behind the head, the torso twists the right shoulder back
        # and leans, the head turns to keep facing the throw and the free arm
        # reaches forward. The release skips the throw clip's own wind-up in
        # proportion to the charge.
        ("charge", 1.0, 9, {
            SPINE: {"rotation": lambda u: euler(y=-30.0 * ease(u, 0.0, 1.0),
                                                z=-8.0 * ease(u, 0.2, 1.0))},
            HEAD: {"rotation": lambda u: euler(y=25.0 * ease(u, 0.0, 1.0),
                                               z=5.0 * ease(u, 0.3, 1.0))},
            UPPER_R: {"rotation": lambda u: euler(x=-15.0 * ease(u, 0.2, 1.0),
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
            "attributes": {"POSITION": pos, "NORMAL": nrm, "JOINTS_0": jnt, "WEIGHTS_0": wgt},
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
