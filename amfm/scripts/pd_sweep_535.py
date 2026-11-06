#!/usr/bin/env python3
"""AMFM #535 Phase A — clearance sweep of the CORRECTED PD hierarchy.

The #534 proof swept a rig in which the whole plate and the sensor pitched. The
Owner revoked that evidence, so this re-measures the hierarchy the Owner actually
asked for:

    pad (fixed, part of the ship)
      -> yaw group    rotates about transform.origin_source on the mount normal;
                      carries the connecting housing and the sensor, and turns
                      the whole upper assembly in azimuth
        -> pitch group rotates about its own origin_source on the derived axle;
                      carries ONLY the two arm + barrel subassemblies

Same grid as #534 so the numbers are comparable: yaw 0..345 step 15 (24) x pitch
0..-180 step -15 (13) = 312 poses per mount.  Obstacle = every cell of the ship
that is not part of the mount under test, within R of the yaw pivot.  A pose
fails when unit-cube interpenetration depth > 0; face contact is depth 0.

It also proves the two things the Owner asked to see numerically:
  * the arms sweep outward (the barrel tip leaves the deck on the outward side);
  * the two arms travel the same distance, so they elevate together.
"""
import json
import math
import sys

sys.path.insert(0, r'C:\projects_tools\goxel-lab\goxel\amfm\scripts')
import goxread

GOX = sys.argv[1] if len(sys.argv) > 1 else \
    r'Z:\data_art\Ref_SF\voxels\Star Marine Trooper\pd535\StarMarineTrooper_hollow_pd535_candidate.gox'
JOINTS = sys.argv[2] if len(sys.argv) > 2 else \
    r'Z:/data_art/Ref_SF/voxels/Star Marine Trooper/pd535/pd535_joints.json'
OUT = sys.argv[3] if len(sys.argv) > 3 else \
    r'Z:\data_art\Ref_SF\voxels\Star Marine Trooper\pd535\pd535_sweep.json'
YAWS = list(range(0, 360, 15))
PITCHES = list(range(0, -181, -15))
R = 8


def rot(p, o, ax, deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    x, y, z = p[0] - o[0], p[1] - o[1], p[2] - o[2]
    u, v, w = ax
    d = u * x + v * y + w * z
    return (o[0] + u * d + c * (x - u * d) + s * (v * z - w * y),
            o[1] + v * d + c * (y - v * d) + s * (w * x - u * z),
            o[2] + w * d + c * (z - w * d) + s * (u * y - v * x))


def main():
    allc = goxread.cells(goxread.load(GOX))
    mounts = json.load(open(JOINTS, encoding='utf-8'))['mounts']
    print(f'gox {GOX}')
    print(f'cells={len(allc)}  poses/mount={len(YAWS) * len(PITCHES)}')
    print()
    print('grp     stage cells        | fails/312  worst m  worst(y,p) | '
          'tip out at -90 | arm L/R travel at -90')
    rows = []
    for m in mounts:
        yaw_cells = {tuple(m['yaw_cell'])} | {tuple(c) for c in m['yaw_carries_cells']}
        pitch_cells = {tuple(c) for c in m['arm_L_cells']} | {tuple(c) for c in m['arm_R_cells']}
        moving = sorted(yaw_cells) + sorted(pitch_cells)
        obs = [p for p in allc if p not in (yaw_cells | pitch_cells)]
        yp = tuple(m['yaw_pivot_xyz'])
        obs = [p for p in obs if all(abs(p[i] - yp[i]) <= R for i in range(3))]
        ax_y = tuple(m['yaw_axis_source'])
        ax_p = tuple(m['pitch_axis_source'])
        h = tuple(m['pitch_hinge_xyz'])

        fails, worst, wpose = 0, -9.0, None
        for th in YAWS:
            for tp in PITCHES:
                pose = [rot(rot(p, h, ax_p, tp) if p in pitch_cells else p, yp, ax_y, th)
                        for p in moving]
                d = -9.0
                for a in pose:
                    for q in obs:
                        g = min(1.0 - abs(a[i] - q[i]) for i in range(3))
                        if g > d:
                            d = g
                if d > 0:
                    fails += 1
                if d > worst:
                    worst, wpose = d, (th, tp)

        # Outward proof: at a quarter turn the barrel tip must have left the deck
        # on the outward side of the plate, not buried under it.
        out = m['yaw_axis_source'][2]
        tips = {}
        for side in ('L', 'R'):
            b = tuple(m['barrels'][side]['source_cell'])
            t = rot(rot(b, h, ax_p, -90), yp, ax_y, 0)
            tips[side] = t
        outward_gain = min(out * (tips[s][2] - m['barrels'][s]['source_cell'][2])
                           for s in ('L', 'R'))
        travel = {s: round(sum(abs(a - b) for a, b in zip(
            tips[s], m['barrels'][s]['source_cell'])), 3) for s in ('L', 'R')}
        symmetric = abs(travel['L'] - travel['R']) < 1e-6

        print(f"{m['group']:6} yaw {len(yaw_cells):2d} pitch {len(pitch_cells):2d}      | "
              f"{fails:5}/{len(YAWS) * len(PITCHES)} {worst:8.2f}  {str(wpose[0]):>3},{str(wpose[1]):>4}    | "
              f"{outward_gain:+8.2f} m   | L {travel['L']:.2f} / R {travel['R']:.2f} "
              f"{'same' if symmetric else 'DIFFER'}")
        rows.append({
            'group': m['group'], 'mount': m['mount'],
            'yaw_stage_cells': len(yaw_cells), 'pitch_stage_cells': len(pitch_cells),
            'obstacle_cells': len(obs), 'poses': len(YAWS) * len(PITCHES),
            'fails': fails, 'worst_penetration_m': round(worst, 3),
            'worst_pose_deg': list(wpose),
            'outward_gain_at_minus90_m': round(outward_gain, 3),
            'arm_travel_at_minus90_m': travel, 'arms_symmetric': symmetric,
        })
    json.dump(rows, open(OUT, 'w'), indent=1)
    total = sum(r['fails'] for r in rows)
    print(f"\ntotal failing poses across 8 mounts: {total} of {8 * len(YAWS) * len(PITCHES)}")
    print('outward on every mount:', all(r['outward_gain_at_minus90_m'] > 0 for r in rows))
    print('arms symmetric on every mount:', all(r['arms_symmetric'] for r in rows))
    print('wrote', OUT)


main()
