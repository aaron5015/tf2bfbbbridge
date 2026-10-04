#!/usr/bin/env python3
"""Writes bfbbtest.vmf: a big sealed empty room for the BFBB bridge.

No Hammer needed: a VMF is plain text, and vbsp/vvis/vrad (shipped in
'Source SDK Base 2013 Multiplayer/bin/x64') compile it from the command line.
See compile_bfbbtest.bat.

Layout: interior is a cube of +-HALF units (default 15000) with a grid floor
at z=-HALF and a sky ceiling/walls. Walls are 1000 thick, so everything stays
inside the +-16384 limit of a Source map.
"""
import sys

HALF = 15000
THICK = 1000
OUT = HALF + THICK

_id = [1]
def nid():
    _id[0] += 1
    return _id[0]

# face -> (outward normal, plane points, uaxis, vaxis)
def box_sides(mn, mx):
    x0, y0, z0 = mn; x1, y1, z1 = mx
    # Points follow Hammer's convention: outward normal = (p3-p1) x (p2-p1).
    return {
        '+z': ((0, 0, 1),  [(x0, y1, z1), (x1, y1, z1), (x1, y0, z1)], "[1 0 0 0]", "[0 -1 0 0]"),
        '-z': ((0, 0, -1), [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0)], "[1 0 0 0]", "[0 -1 0 0]"),
        '-x': ((-1, 0, 0), [(x0, y1, z1), (x0, y0, z1), (x0, y0, z0)], "[0 1 0 0]", "[0 0 -1 0]"),
        '+x': ((1, 0, 0),  [(x1, y0, z0), (x1, y0, z1), (x1, y1, z1)], "[0 1 0 0]", "[0 0 -1 0]"),
        '+y': ((0, 1, 0),  [(x1, y1, z0), (x1, y1, z1), (x0, y1, z1)], "[1 0 0 0]", "[0 0 -1 0]"),
        '-y': ((0, -1, 0), [(x1, y0, z1), (x1, y0, z0), (x0, y0, z0)], "[1 0 0 0]", "[0 0 -1 0]"),
    }

def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def sub(a, b):
    return (a[0]-b[0], a[1]-b[1], a[2]-b[2])

def check_normals(sides):
    for name, (n, pts, _, _) in sides.items():
        p1, p2, p3 = pts
        c = cross(sub(p3, p1), sub(p2, p1))
        # same direction as the stated outward normal
        assert (c[0]*n[0] + c[1]*n[1] + c[2]*n[2]) > 0, "bad winding on " + name

def solid(mn, mx, mats):
    sides = box_sides(mn, mx)
    check_normals(sides)
    out = ['\tsolid', '\t{', '\t\t"id" "%d"' % nid()]
    for name, (n, pts, ua, va) in sides.items():
        plane = ' '.join('(%d %d %d)' % p for p in pts)
        out += ['\t\tside', '\t\t{',
                '\t\t\t"id" "%d"' % nid(),
                '\t\t\t"plane" "%s"' % plane,
                '\t\t\t"material" "%s"' % mats.get(name, 'TOOLS/TOOLSNODRAW'),
                '\t\t\t"uaxis" "%s 0.25"' % ua,
                '\t\t\t"vaxis" "%s 0.25"' % va,
                '\t\t\t"rotation" "0"',
                '\t\t\t"lightmapscale" "64"',
                '\t\t\t"smoothing_groups" "0"',
                '\t\t}']
    out += ['\t}']
    return out

GRID = 'DEV/GRAYGRID'
SKY = 'TOOLS/TOOLSSKYBOX'

brushes = []
# floor / ceiling: full footprint, only their inner face is visible
brushes += solid((-OUT, -OUT, -OUT), (OUT, OUT, -HALF), {'+z': GRID})
brushes += solid((-OUT, -OUT, HALF),  (OUT, OUT, OUT),  {'-z': SKY})
# x walls: full y, interior z
brushes += solid((-OUT, -OUT, -HALF), (-HALF, OUT, HALF), {'+x': SKY})
brushes += solid((HALF, -OUT, -HALF), (OUT, OUT, HALF),  {'-x': SKY})
# y walls: between the x walls
brushes += solid((-HALF, -OUT, -HALF), (HALF, -HALF, HALF), {'+y': SKY})
brushes += solid((-HALF, HALF, -HALF), (HALF, OUT, HALF),  {'-y': SKY})

def ent(classname, props):
    o = ['entity', '{', '\t"id" "%d"' % nid(), '\t"classname" "%s"' % classname]
    for k, v in props:
        o.append('\t"%s" "%s"' % (k, v))
    o.append('}')
    return o

vmf = ['versioninfo', '{', '\t"editorversion" "400"', '\t"editorbuild" "8000"',
       '\t"mapversion" "1"', '\t"formatversion" "100"', '\t"prefab" "0"', '}',
       'visgroups', '{', '}',
       'viewsettings', '{', '\t"bSnapToGrid" "1"', '\t"bShowGrid" "1"', '}',
       'world', '{', '\t"id" "1"', '\t"mapversion" "1"', '\t"classname" "worldspawn"',
       '\t"skyname" "sky_day01_01"', '\t"maxpropscreenwidth" "-1"']
vmf += brushes
vmf += ['}']

z = -HALF + 24
vmf += ent('info_player_teamspawn', [('TeamNum', '2'), ('origin', '0 0 %d' % z), ('angles', '0 0 0')])
vmf += ent('info_player_teamspawn', [('TeamNum', '3'), ('origin', '128 0 %d' % z), ('angles', '0 180 0')])
vmf += ent('light_environment', [('_ambient', '255 255 255 120'), ('_ambientHDR', '-1 -1 -1 1'),
                                 ('_AmbientScaleHDR', '1'), ('_light', '255 255 255 300'),
                                 ('_lightHDR', '-1 -1 -1 1'), ('_lightscaleHDR', '1'),
                                 ('angles', '0 0 0'), ('pitch', '-50'), ('SunSpreadAngle', '0'),
                                 ('origin', '0 0 %d' % (HALF - 200))])
vmf += ent('light', [('_light', '255 255 255 800'), ('_lightHDR', '-1 -1 -1 1'),
                     ('_lightscaleHDR', '1'), ('_constant_attn', '0'), ('_linear_attn', '0'),
                     ('_quadratic_attn', '1'), ('_fifty_percent_distance', '0'),
                     ('_zero_percent_distance', '0'), ('_hardfalloff', '0'),
                     ('origin', '0 0 %d' % (-HALF + 300))])

path = sys.argv[1] if len(sys.argv) > 1 else 'bfbbtest.vmf'
open(path, 'w', newline='\n').write('\n'.join(vmf) + '\n')
print('wrote', path, '-', len(brushes) // 1, 'lines of brush data')
