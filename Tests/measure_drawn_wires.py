#!/usr/bin/env python3
"""Measure exported Slate Bezier centerlines; no Unreal assets are modified."""
import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path


def midpoint(a, b):
    return ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)


def flatten_parameters(controls, tolerance):
    """Chord/control-polygon bounds give a total length error budget per curve."""
    points, parameters, lower, upper = [controls[0]], [0.0], 0.0, 0.0
    stack = [(controls, tolerance, 0, 0.0, 1.0)]
    while stack:
        (a, b, c, d), budget, depth, start, end = stack.pop()
        chord = math.dist(a, d)
        polygon = math.dist(a, b) + math.dist(b, c) + math.dist(c, d)
        if polygon - chord <= budget:
            if d != points[-1]:
                points.append(d)
                parameters.append(end)
            lower += chord
            upper += polygon
            continue
        if depth == 24:
            raise ValueError("Curve subdivision exceeded its bound; measurement refused")
        ab, bc, cd = midpoint(a, b), midpoint(b, c), midpoint(c, d)
        abc, bcd = midpoint(ab, bc), midpoint(bc, cd)
        center = midpoint(abc, bcd)
        middle = (start + end) / 2
        stack.append(((center, bcd, cd, d), budget / 2, depth + 1, middle, end))
        stack.append(((a, ab, abc, center), budget / 2, depth + 1, start, middle))
    return points, lower, upper, parameters


def flatten(controls, tolerance):
    return flatten_parameters(controls, tolerance)[:3]


def subtract(a, b):
    return (a[0] - b[0], a[1] - b[1])


def cross(a, b):
    return a[0] * b[1] - a[1] * b[0]


def cubic(controls, t):
    weights = ((1-t)**3, 3*t*(1-t)**2, 3*t*t*(1-t), t**3)
    return tuple(sum(w*p[axis] for w, p in zip(weights, controls)) for axis in (0, 1))


def derivative(controls, t):
    return tuple(3*((1-t)**2*(controls[1][axis]-controls[0][axis]) +
                    2*t*(1-t)*(controls[2][axis]-controls[1][axis]) +
                    t*t*(controls[3][axis]-controls[2][axis])) for axis in (0, 1))


def refine_intersection(a, t, b, u):
    """Refine a chord candidate, or explicitly leave it unresolved.

    Translation avoids subtracting large graph coordinates at a shared
    departure. Small residual alone is insufficient at a multiple root:
    continue until the parameter step also converges.
    """
    origin = a[0]
    local_a = [subtract(p, origin) for p in a]
    local_b = [subtract(p, origin) for p in b]
    for _ in range(64):
        pa, pb = cubic(local_a, t), cubic(local_b, u)
        delta = subtract(pa, pb)
        da, db = derivative(local_a, t), derivative(local_b, u)
        determinant = cross(da, db)
        if determinant == 0:
            return None
        dt, du = -cross(delta, db)/determinant, -cross(delta, da)/determinant
        if not all(math.isfinite(v) for v in (dt, du)):
            return None
        t, u = t+dt, u+du
        if not (-1e-9 <= t <= 1+1e-9 and -1e-9 <= u <= 1+1e-9):
            return None
        t, u = min(1.0, max(0.0, t)), min(1.0, max(0.0, u))
        if max(abs(dt), abs(du)) <= 1e-11:
            pa, pb = cubic(local_a, t), cubic(local_b, u)
            if math.dist(pa, pb) <= 1e-7:
                return (origin[0]+(pa[0]+pb[0])/2, origin[1]+(pa[1]+pb[1])/2), t, u
            return None
    return None


def curve_rays(controls, piece, t):
    directions = []
    if t > 1e-9:
        directions.append(tuple(-v for v in derivative(controls[piece], t)))
    elif piece:
        directions.append(tuple(-v for v in derivative(controls[piece-1], 1)))
    else:
        return None
    if t < 1-1e-9:
        directions.append(derivative(controls[piece], t))
    elif piece+1 < len(controls):
        directions.append(derivative(controls[piece+1], 0))
    else:
        return None
    if any(math.hypot(*d) == 0 for d in directions):
        return []  # Stationary interior: these tangents cannot classify it.
    return [math.atan2(d[1], d[0]) for d in directions]


def alternating(ar, br):
    if not ar or not br:
        return False
    angles = sorted([(v, 0) for v in ar] + [(v, 1) for v in br])
    gaps = [(angles[(k+1) % 4][0]-angles[k][0]) % (2*math.pi) for k in range(4)]
    return min(gaps) > 1e-7 and all(angles[k][1] != angles[(k+1) % 4][1] for k in range(4))


def rays(points, index, point):
    # At a segment boundary, use neighbors on both sides of the whole path.
    # This counts a crossing at a curve join once and distinguishes a V-touch.
    left, right = index, index + 1
    while left >= 0 and math.dist(points[left], point) <= 1e-5:
        left -= 1
    while right < len(points) and math.dist(points[right], point) <= 1e-5:
        right += 1
    if left < 0 or right == len(points):
        return None  # A wire endpoint touching another path is not a crossing.
    return [math.atan2(p[1] - point[1], p[0] - point[0]) for p in (points[left], points[right])]


def pair_events(a, b, curve_a=None, curve_b=None):
    """Strict crossings, other point contacts, and positive collinear overlap."""
    events, overlaps = [], False
    b_segments = [(i, p, q, min(p[0], q[0]), max(p[0], q[0]), min(p[1], q[1]), max(p[1], q[1]))
                  for i, (p, q) in enumerate(zip(b, b[1:]))]
    for i, (p, q) in enumerate(zip(a, a[1:])):
        lo_x, hi_x, lo_y, hi_y = min(p[0], q[0]), max(p[0], q[0]), min(p[1], q[1]), max(p[1], q[1])
        direction = subtract(q, p)
        for j, r, s, bx0, bx1, by0, by1 in b_segments:
            if hi_x < bx0 or bx1 < lo_x or hi_y < by0 or by1 < lo_y:
                continue
            other, offset = subtract(s, r), subtract(r, p)
            determinant = cross(direction, other)
            if abs(determinant) <= 1e-12 * math.hypot(*direction) * math.hypot(*other):
                if abs(cross(offset, direction)) <= 1e-7 * math.hypot(*direction):
                    axis = 0 if abs(direction[0]) >= abs(direction[1]) else 1
                    overlap = min(max(p[axis], q[axis]), max(r[axis], s[axis])) - max(min(p[axis], q[axis]), min(r[axis], s[axis]))
                    overlaps |= overlap > 1e-7
                continue
            t, u = cross(offset, other) / determinant, cross(offset, direction) / determinant
            if not (-1e-10 <= t <= 1 + 1e-10 and -1e-10 <= u <= 1 + 1e-10):
                continue
            point = (p[0] + t * direction[0], p[1] + t * direction[1])
            if any(math.dist(point, e[1]) <= 1e-4 for e in events):
                continue
            proper = alternating(rays(a, i, point), rays(b, j, point))
            kind = "crossing" if proper else "contact"
            if proper and curve_a is not None and curve_b is not None:
                ai, at0, at1 = curve_a["segments"][i]
                bi, bt0, bt1 = curve_b["segments"][j]
                refined = refine_intersection(curve_a["controls"][ai], at0+t*(at1-at0),
                                              curve_b["controls"][bi], bt0+u*(bt1-bt0))
                if refined is None:
                    kind = "unresolved"
                else:
                    point, at, bt = refined
                    ar, br = curve_rays(curve_a["controls"], ai, at), curve_rays(curve_b["controls"], bi, bt)
                    kind = "unresolved" if ar == [] or br == [] else "crossing" if alternating(ar, br) else "contact"
                if any(math.dist(point, e[1]) <= 1e-4 for e in events):
                    continue
            events.append((kind, point))
    return events, overlaps


def read_wires(path, tolerance):
    grouped = defaultdict(list)
    with path.open(encoding="utf-8-sig", newline="") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            key = tuple(row[k] for k in ("policy", "from_node", "from_pin", "to_node", "to_pin"))
            grouped[key].append(row)
    policies = defaultdict(dict)
    for key, rows in sorted(grouped.items()):
        rows.sort(key=lambda row: int(row["piece"]))
        if [int(row["piece"]) for row in rows] != list(range(len(rows))):
            raise ValueError(f"Missing or duplicate curve pieces: {key}")
        points, segments, all_controls, lower, upper = [], [], [], 0.0, 0.0
        for row in rows:
            controls = tuple((float(row[f"p{i}x"]), float(row[f"p{i}y"])) for i in range(4))
            if not all(math.isfinite(v) for p in controls for v in p):
                raise ValueError(f"Non-finite control points: {key}")
            piece, low, high, parameters = flatten_parameters(controls, tolerance)
            if points and points[-1] != piece[0]:
                raise ValueError(f"Disconnected drawn pieces: {key}, gap={math.dist(points[-1], piece[0])}")
            points.extend(piece if not points else piece[1:])
            segments.extend((len(all_controls), a, b) for a, b in zip(parameters, parameters[1:]))
            all_controls.append(controls)
            lower += low
            upper += high
        policies[key[0]][key[1:]] = dict(points=points, lower=lower, upper=upper, pieces=len(rows),
                                        controls=all_controls, segments=segments,
                                        label=rows[0]["from_label"] + " -> " + rows[0]["to_label"],
                                        cached_fallback=int(rows[0]["cached_fallback"]))
    if set(policies) != {"native", "glooprint"} or set(policies["native"]) != set(policies["glooprint"]):
        raise ValueError("Native and styled exports must contain exactly the same original connections")
    return policies


def measure(path, tolerance):
    summary, details = [], []
    stage = path.name.removesuffix("-drawn-curves.tsv")
    for policy, wires in read_wires(path, tolerance).items():
        keys = list(wires)
        crossings = crossing_pairs = overlap_pairs = unrelated_overlaps = contacts = shared_pairs = unresolved = 0
        for i, key in enumerate(keys):
            a = wires[key]
            for other_key in keys[:i]:
                b = wires[other_key]
                shared = key[:2] == other_key[:2] or key[2:] == other_key[2:]
                shared_pairs += shared
                events, overlaps = pair_events(a["points"], b["points"], a, b)
                count = sum(kind == "crossing" for kind, point in events)
                crossings += count
                crossing_pairs += count > 0
                contacts += sum(kind == "contact" for kind, point in events)
                unresolved += sum(kind == "unresolved" for kind, point in events)
                overlap_pairs += overlaps
                unrelated_overlaps += overlaps and not shared
                if events or overlaps:
                    details.append(dict(stage=stage, policy=policy, a=a["label"], b=b["label"], shared_pin=shared,
                                        overlap=overlaps, events=[dict(kind=kind, x=p[0], y=p[1]) for kind, p in events]))
        lower, upper = sum(w["lower"] for w in wires.values()), sum(w["upper"] for w in wires.values())
        summary.append(dict(stage=stage, policy=policy, connections=len(wires), curves=sum(w["pieces"] for w in wires.values()),
                            cached_fallback_links=sum(w["cached_fallback"] != 0 for w in wires.values()) if policy == "glooprint" else None,
                            drawn_length=(lower + upper) / 2, length_error_bound=(upper - lower) / 2,
                            crossings=crossings, crossing_pairs=crossing_pairs, shared_pin_pairs=shared_pairs,
                            overlap_pairs=overlap_pairs, unrelated_overlap_pairs=unrelated_overlaps, point_contacts=contacts,
                            unresolved_candidates=unresolved))
    return summary, details


def self_test():
    for controls, exact in [(((0, 0), (1, 0), (2, 0), (3, 0)), 3),
                            (((0, 0), (1, 0), (2, 1), (3, 3)), 1.5 * math.sqrt(5) + .75 * math.asinh(2)),
                            (((0, 0), (4, 0), (-4, 0), (0, 0)), 8 * math.sqrt(3) / 3)]:
        points, lower, upper = flatten(controls, .001)
        assert lower - 1e-10 <= exact <= upper + 1e-10 and upper - lower <= .001 + 1e-10
        assert points[0] == controls[0] and points[-1] == controls[-1]
    for a, b, expected in [([(-1, 0), (1, 0)], [(0, -1), (0, 1)], 1),
                           ([(-1, 0), (0, 0), (1, 0)], [(0, -1), (0, 0), (0, 1)], 1),
                           ([(-1, 0), (1, 0)], [(-1, 1), (0, 0), (1, 1)], 0),
                           ([(0, 0), (2, 1)], [(0, 0), (2, -1)], 0)]:
        events, overlaps = pair_events(a, b)
        assert sum(kind == "crossing" for kind, p in events) == expected and not overlaps
    events, overlaps = pair_events([(0, 0), (1, 0), (2, 1)], [(0, 0), (1, 0), (2, -1)])
    assert overlaps and not any(kind == "crossing" for kind, p in events)
    # Same increasing y profile, different horizontal scale: the shorter
    # departure is strictly above the longer one after their common endpoint.
    # Independent chord approximations previously invented an intersection.
    def departure(length):
        return ((0, 0), (length/3, 0), (2*length/3, 1.5), (length, 1.5))
    def wire(controls, tolerance):
        points, _, _, parameters = flatten_parameters(controls, tolerance)
        return dict(points=points, controls=[controls],
                    segments=[(0, a, b) for a, b in zip(parameters, parameters[1:])])
    for tolerance in (.001, .0005):
        a, b = wire(departure(81.5), tolerance), wire(departure(69.75), tolerance)
        events, _ = pair_events(a["points"], b["points"], a, b)
        assert not any(kind in ("crossing", "unresolved") for kind, _ in events)
    for offset in ((0, 0), (1000000, -1000000)):
        a = tuple((x+offset[0], y+offset[1]) for x, y in ((0, 0), (1, 0), (2, 0), (3, 0)))
        b = tuple((x+offset[0], y+offset[1]) for x, y in ((0, -1), (1, -1), (2, 1), (3, 1)))
        result = refine_intersection(a, .4, b, .6)
        assert result is not None and math.dist(result[0], (offset[0]+1.5, offset[1])) <= 1e-7
        assert alternating(curve_rays([a], 0, result[1]), curve_rays([b], 0, result[2]))
    line = ((0, 0), (1, 0), (2, 0), (3, 0))
    assert refine_intersection(line, .5, line, .5) is None  # No unique crossing proof.
    stationary = ((-.125, 0), (.125, 0), (-.125, 0), (.125, 0))
    assert curve_rays([stationary], 0, .5) == []  # Interior is not an endpoint contact.
    assert curve_rays([stationary], 0, 0) is None
    print("Length bounds, crossings, tangencies, shared departures and translated cubic refinement passed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", type=Path, nargs="*")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--tolerance", type=float, default=.001, help="Length bound gap per cubic, in graph units")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if not math.isfinite(args.tolerance) or args.tolerance <= 0:
        parser.error("Tolerance must be finite and positive")
    if args.self_test:
        self_test()
    if not args.files:
        if not args.self_test:
            parser.error("Supply exported curve TSV files")
        return
    if args.output is None:
        parser.error("--output directory is required for measurements")
    summary, details = [], []
    for path in args.files:
        rows, events = measure(path, args.tolerance)
        summary.extend(rows)
        details.extend(events)
    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / "drawn-metrics.csv").open("w", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    (args.output / "intersections.json").write_text(json.dumps(details, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
