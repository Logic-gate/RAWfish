#!/usr/bin/env python3
# lens-calibrate.py -- measure a camera's lens distortion from chessboard
# photos and write the lens distortion file RAWfish-ir corrects with
# (android/lensdist.h; docs/LENS-DISTORTION.md). For maintainers, on a PC:
# the phone only reads the file (no OpenCV on the phone).
#
#   scripts/lens-calibrate.py --camera-id 2 --name wide samples/wide/43 samples/wide/169
#
# writes android/data/lens-distortion/distortion_<maker>-<model>_cam<ID>.json
# (maker and model from the photos' Exif, as the engine names the file).
#
# ONE file per camera, for every photo size and the viewfinder: each folder
# of samples may be of another size or shape (4:3, 16:9, 12 or 50 MP); its
# corners are mapped onto the largest 4:3 frame (the camera's full frame:
# a 16:9 photo is its middle band, a 12 MP one the same frame scaled), and
# one model is fitted on all of them. More shapes, more of the frame's
# corners covered.
#
# Samples: photos taken WITH RAWfish-ir (its JPG; the lens correction OFF),
# of a flat printed chessboard, the board filling much of the frame, in the
# corners and along the edges above all; portrait or landscape (the pixels
# are read as the sensor gave them, the Exif orientation ignored).
#
# Requires: python3, opencv-python, numpy.
#
# The detection (inner corners, then the outer corners of the border squares,
# by intersecting the squares' sides: on a wide lens most of the distortion
# is in the outer ring) and the model choice come from the first
# calibration of the JP2's lenses (camera_calibration.py, November 2026).

import argparse
import datetime
import glob
import json
import os
import re
import struct
import sys

import cv2
import numpy as np

IMAGE_EXTENSIONS = ('.jpg', '.jpeg', '.png')
FORMAT = 'rawfish-ir lens distortion 1'

# The board, set from the command line (set_board)
nX, nY = 6, 9
pattern_size = (nX, nY)
square_size = 0.023
object_grid = None

USE_OUTER_CORNERS = True

# Distortion models to try, best first; the first whose radial curve does
# not fold back inside the frame is used. The phone lenses have "mustache"
# distortion (slight pincushion in the middle, strong barrel towards the
# corners), which the 5 coefficient model cannot represent; the rational
# model can, and with k3 and k6 fixed it stays monotonic into the corners.
CALIBRATION_MODELS = [
    ('rational k1,k2,k4,k5', cv2.CALIB_RATIONAL_MODEL | cv2.CALIB_FIX_K3 | cv2.CALIB_FIX_K6),
    ('k1,k2,k3', 0),
    ('k1,k2', cv2.CALIB_FIX_K3),
]

# Samples whose reprojection error is more than this many times the median
# are dropped and the fit repeated without them.
OUTLIER_FACTOR = 2.0

# Corners are found on a downscaled copy (fast) and refined at full resolution.
DETECTION_WIDTH = 1024

subpix_criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 50, 0.001)
calib_criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 300, 1e-10)


def set_board(squares_x, squares_y, square_mm):
    """The chessboard: squares along x and y, the side of a square in mm."""
    global nX, nY, pattern_size, square_size, object_grid
    nX, nY = squares_x - 1, squares_y - 1
    pattern_size = (nX, nY)
    square_size = square_mm / 1000.0
    # Index [j, i] -> row j, column i of the full grid, outer corners
    # included; the inner corners are [1:-1, 1:-1].
    gj, gi = np.mgrid[0:nY + 2, 0:nX + 2]
    object_grid = np.stack([(gi - 1) * square_size, (gj - 1) * square_size,
                            np.zeros(gi.shape)], axis=-1).astype(np.float32)

# --------------------------------------------------------------------------
# Image loading / orientation
# --------------------------------------------------------------------------

def load_sensor_image(filename, flags=cv2.IMREAD_COLOR):
    """Load the image as stored by the sensor, ignoring EXIF orientation."""
    image = cv2.imread(filename, flags | cv2.IMREAD_IGNORE_ORIENTATION)
    if image is None:
        raise FileNotFoundError(filename)
    return image


# --------------------------------------------------------------------------
# Corner detection
# --------------------------------------------------------------------------

def find_inner_corners(gray):
    """Inner chessboard corners as an (nY, nX, 2) array, or None."""
    base = max(1, round(gray.shape[1] / DETECTION_WIDTH))
    for s in dict.fromkeys([base, base * 2, max(1, base // 2)]):
        small = cv2.resize(gray, None, fx=1 / s, fy=1 / s, interpolation=cv2.INTER_AREA)
        success, corners = cv2.findChessboardCorners(
            small, pattern_size, cv2.CALIB_CB_ADAPTIVE_THRESH | cv2.CALIB_CB_NORMALIZE_IMAGE)
        if not success:
            # The sector based detector copes better with distorted/tilted boards
            success, corners = cv2.findChessboardCornersSB(
                small, pattern_size, cv2.CALIB_CB_EXHAUSTIVE)
        if success:
            break
    else:
        return None
    # Refine at full resolution. A small window works best here (tested 7-30
    # px; larger windows pick up noise and the curvature of the edges).
    corners = (corners * s).astype(np.float32)
    corners = cv2.cornerSubPix(gray, corners, (11, 11), (-1, -1), subpix_criteria)
    return corners.reshape(nY, nX, 2).astype(np.float64)


def _extrapolate(grid, axis):
    """Quadratic extrapolation of the grid by one step at both ends of axis."""
    a = np.moveaxis(grid, axis, 0)
    lo = 3 * a[0] - 3 * a[1] + a[2]
    hi = 3 * a[-1] - 3 * a[-2] + a[-3]
    return np.moveaxis(np.concatenate([lo[None], a, hi[None]]), 0, axis)


def _intersect(line1, line2):
    vx1, vy1, x1, y1 = line1
    vx2, vy2, x2, y2 = line2
    A = np.array([[vx1, -vx2], [vy1, -vy2]])
    if abs(np.linalg.det(A)) < 0.3:  # nearly parallel
        return None
    t = np.linalg.solve(A, [x2 - x1, y2 - y1])[0]
    return np.array([x1 + t * vx1, y1 + t * vy1])


def _refine_square_vertices(gray, quad, vertices):
    """Measure outer vertices of one (black) border square.

    The square is segmented, and each requested vertex is computed as the
    intersection of lines fitted to the two square sides near that vertex.
    (cornerSubPix is unreliable on these L-shaped corners.)
    quad: 4x2 predicted vertex positions in cyclic order.
    Returns {vertex_index: (x, y)} for vertices that were measured reliably.
    """
    h, w = gray.shape
    side = np.mean(np.linalg.norm(quad - np.roll(quad, 1, axis=0), axis=1))
    margin = int(side * 0.35)
    x0, y0 = np.maximum(np.floor(quad.min(axis=0)).astype(int) - margin, 0)
    x1, y1 = np.minimum(np.ceil(quad.max(axis=0)).astype(int) + margin, (w - 1, h - 1))
    if x1 - x0 < side * 0.5 or y1 - y0 < side * 0.5:
        return {}

    roi = cv2.GaussianBlur(gray[y0:y1 + 1, x0:x1 + 1], (0, 0), 1.5)
    _, mask = cv2.threshold(roi, 0, 255, cv2.THRESH_BINARY_INV + cv2.THRESH_OTSU)
    # Break the thin diagonal links to neighbouring black squares
    k = max(3, int(side * 0.04)) | 1
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, np.ones((k, k), np.uint8))

    cx, cy = quad.mean(axis=0) - (x0, y0)
    if not (0 <= cx < mask.shape[1] and 0 <= cy < mask.shape[0]):
        return {}
    _, labels = cv2.connectedComponents(mask)
    label = labels[int(cy), int(cx)]
    if label == 0:  # centre is not dark -> white square
        return {}
    component = (labels == label).astype(np.uint8)
    area_ratio = component.sum() / cv2.contourArea(quad.astype(np.float32))
    if not 0.6 < area_ratio < 1.4:
        return {}

    contours, _ = cv2.findContours(component, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    pts = max(contours, key=len).reshape(-1, 2).astype(np.float64) + (x0, y0)
    # The square must be completely inside the image
    if (pts.min(axis=0) <= 1).any() or (pts[:, 0] >= w - 2).any() or (pts[:, 1] >= h - 2).any():
        return {}

    result = {}
    for vi in vertices:
        vertex = quad[vi].copy()
        for _ in range(2):  # second pass re-centres on the measured vertex
            lines = []
            for neighbour in (quad[(vi + 1) % 4], quad[(vi - 1) % 4]):
                u = (neighbour - quad[vi]) / np.linalg.norm(neighbour - quad[vi])
                n = np.array([-u[1], u[0]])
                rel = pts - vertex
                along, across = rel @ u, rel @ n
                # use only the part of the side close to the vertex, skipping
                # the rounded tip, so side curvature doesn't matter
                sel = (along > 0.06 * side) & (along < 0.4 * side) & (np.abs(across) < 0.12 * side)
                if sel.sum() < 8:
                    break
                lines.append(cv2.fitLine(pts[sel].astype(np.float32),
                                         cv2.DIST_HUBER, 0, 0.01, 0.01).ravel())
            vertex = _intersect(*lines) if len(lines) == 2 else None
            if vertex is None:
                break
        if vertex is not None and np.linalg.norm(vertex - quad[vi]) < 0.3 * side:
            result[vi] = vertex
    return result


def find_outer_corners(gray, inner):
    """Extend the (nY, nX) inner corner grid by the outer corners of the
    border squares. Returns (points (nY+2, nX+2, 2), valid mask)."""
    predicted = _extrapolate(_extrapolate(inner, 1), 0)
    points = predicted.copy()
    valid = np.zeros(predicted.shape[:2], bool)
    valid[1:-1, 1:-1] = True
    rows, cols = predicted.shape[:2]

    for r in range(rows - 1):
        for c in range(cols - 1):
            if not (r in (0, rows - 2) or c in (0, cols - 2)):
                continue  # not a border square
            idx = [(r, c), (r, c + 1), (r + 1, c + 1), (r + 1, c)]
            quad = np.array([predicted[i] for i in idx])
            outer = [k for k, (j, i) in enumerate(idx)
                     if j in (0, rows - 1) or i in (0, cols - 1)]
            for k, vertex in _refine_square_vertices(gray, quad, outer).items():
                points[idx[k]] = vertex
                valid[idx[k]] = True
    return points, valid


def list_images(directory, recursive=False):
    pattern = os.path.join(directory, '**', '*') if recursive else os.path.join(directory, '*')
    return sorted(f for f in glob.glob(pattern, recursive=recursive)
                  if os.path.isfile(f) and f.lower().endswith(IMAGE_EXTENSIONS))


def save_debug_image(out_dir, image_file, gray, points, valid):
    os.makedirs(out_dir, exist_ok=True)
    image = cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)
    radius = max(4, gray.shape[1] // 250)
    for j in range(points.shape[0]):
        for i in range(points.shape[1]):
            if valid[j, i]:
                inner = 0 < j < points.shape[0] - 1 and 0 < i < points.shape[1] - 1
                color = (0, 255, 0) if inner else (255, 0, 0)
                x, y = points[j, i]
                cv2.circle(image, (int(round(x)), int(round(y))), radius, color, radius // 2)
    cv2.imwrite(os.path.join(out_dir, os.path.basename(image_file)), image)


# --------------------------------------------------------------------------
# Calibration
# --------------------------------------------------------------------------


# --------------------------------------------------------------------------
# One model on all the folders
# --------------------------------------------------------------------------

def corner_precision(points):
    """How precisely a photo's corners were found, whatever the lens: each
    inner corner predicted from its 8 neighbours by a homography (over two
    squares the lens' distortion is negligible), the median distance in
    pixels. Sharp photos in good light give well under 1 px; noise, blur
    and a high ISO show here first (25 Nov 2026, the JP2's wide camera:
    1.2-1.4 px at ISO 2000-3800, 1/24 s, hand-held, and with it a fit's
    RMS of 3-4 px whatever the model)."""
    ny, nx = points.shape[:2]
    src = np.array([[b, a] for a in (-1, 0, 1) for b in (-1, 0, 1) if (a, b) != (0, 0)], np.float64)
    errs = []
    for j in range(2, ny - 2):
        for i in range(2, nx - 2):
            dst = np.array([points[j + a, i + b] for a in (-1, 0, 1) for b in (-1, 0, 1) if (a, b) != (0, 0)], np.float64)
            h, _ = cv2.findHomography(src, dst, 0)
            if h is None:
                continue
            pred = cv2.perspectiveTransform(np.array([[[0.0, 0.0]]]), h)[0, 0]
            errs.append(np.linalg.norm(pred - points[j, i]))
    return float(np.median(errs)) if errs else float('nan')


def field_in(ref_size, size):
    """Where a frame of `size` lies in the reference frame: the largest
    rectangle of its shape centred in it (x, y, scale)."""
    W, H = ref_size
    w, h = size
    if W / H > w / h:            # the reference is wider: the sides go
        cw = H * w / h
        return (W - cw) / 2, 0.0, cw / w
    ch = W * h / w
    return 0.0, (H - ch) / 2, W / w


def to_ref(points, ref_size, size):
    """Pixel coordinates of a frame of `size` -> the reference frame's."""
    x0, y0, s = field_in(ref_size, size)
    p = points.astype(np.float64).copy()
    p[..., 0] = x0 + (p[..., 0] + 0.5) * s - 0.5
    p[..., 1] = y0 + (p[..., 1] + 0.5) * s - 0.5
    return p.astype(np.float32)


def detect_folder(folder, debug_dir=None):
    """The corners of every usable photo of a folder, grouped by size (each
    size is a set of its own: a 12 MP and a 50 MP photo, or a 4:3 and a
    16:9, may share a folder)."""
    files = list_images(folder)
    groups = {}
    for image_file in files:
        name = os.path.basename(image_file)
        gray = load_sensor_image(image_file, cv2.IMREAD_GRAYSCALE)
        size = gray.shape[::-1]
        g = groups.setdefault(size, {'files': [], 'views': []})
        g['files'].append(image_file)
        inner = find_inner_corners(gray)
        if inner is None:
            print(f"    no chessboard found in {name}")
            continue
        if USE_OUTER_CORNERS:
            points, valid = find_outer_corners(gray, inner)
        else:
            points = np.pad(inner, ((1, 1), (1, 1), (0, 0)))
            valid = np.zeros(points.shape[:2], bool)
            valid[1:-1, 1:-1] = True
        if debug_dir:
            save_debug_image(os.path.join(debug_dir, os.path.basename(os.path.normpath(folder))),
                             image_file, gray, points, valid)
        g['views'].append({'name': name, 'object': object_grid[valid],
                           'image': points[valid].astype(np.float32).reshape(-1, 1, 2),
                           'precision': corner_precision(points)})
    if not groups:
        print(f"    {folder}: no photos")
    out = []
    for size, g in sorted(groups.items(), key=lambda kv: -kv[0][0] * kv[0][1]):
        label = folder if len(groups) == 1 else f"{folder} ({size[0]}x{size[1]})"
        print(f"    {label}: {len(g['views'])}/{len(g['files'])} photos usable, size {size[0]}x{size[1]}")
        if g['views']:
            out.append({'folder': label, 'files': g['files'], 'views': g['views'], 'size': size})
    return out


def radial_fold_radius(dist):
    """Largest distorted (normalised) radius the radial model reaches before it
    stops increasing. Beyond it pixels have no valid undistorted position."""
    k = np.zeros(8)
    k[:min(8, dist.size)] = dist.ravel()[:8]
    k1, k2, _, _, k3, k4, k5, k6 = k
    r = np.linspace(0, 4, 4001)
    r2 = r * r
    rd = r * (1 + r2 * (k1 + r2 * (k2 + r2 * k3))) / (1 + r2 * (k4 + r2 * (k5 + r2 * k6)))
    increasing = np.diff(rd) > 0
    stop = len(increasing) if increasing.all() else int(np.argmin(increasing))
    return rd[:stop + 1].max()


def fit(views, ref_size, quiet=False):
    """calibrateCamera on views (points in the reference frame): the first
    model that does not fold back inside the frame."""
    W, H = ref_size
    obj = [v['object'] for v in views]
    img = [v['ref'] for v in views]
    for model_name, flags in CALIBRATION_MODELS:
        rms, mtx, dist, _, _, _, _, per_view = cv2.calibrateCameraExtended(
            obj, img, ref_size, None, None, flags=flags, criteria=calib_criteria)
        centre = mtx[:2, 2]
        corner = np.linalg.norm([max(centre[0], W - centre[0]), max(centre[1], H - centre[1])])
        if radial_fold_radius(dist) * mtx[0, 0] >= corner:
            break
        if not quiet:
            print(f"    model '{model_name}' folds back inside the frame, trying a simpler one")
    else:
        print("    WARNING: every model folds back inside the frame; its corners will be "
              "wrong. Take more photos with the board in the corners.")
    return model_name, rms, mtx, dist.ravel(), per_view.ravel(), centre, corner


def calibrate(folders, ref=None, debug_dir=None):
    sets = []
    for folder in folders:
        sets += detect_folder(folder, debug_dir)
    if not sets:
        sys.exit("no usable photos")
    if ref is None:
        # the camera's full frame: the largest 4:3 size, else the largest
        four3 = [s['size'] for s in sets if abs(s['size'][0] / s['size'][1] - 4 / 3) < 0.01]
        ref = max(four3 or [s['size'] for s in sets], key=lambda s: s[0] * s[1])
    W, H = ref
    print(f"    reference frame: {W}x{H}")
    # Photos whose pixels are turned against the frame (portrait pixels for
    # a landscape sensor: the camera driver's JPEG on some phones, e.g. the
    # JP2's bracket and burst) cannot be mapped onto it: left out.
    turned = [s for s in sets if (s['size'][0] > s['size'][1]) != (W > H) and s['size'][0] != s['size'][1]]
    for s in turned:
        print(f"    skipping {s['folder']} ({s['size'][0]}x{s['size'][1]}): its pixels are turned against "
              f"the {W}x{H} frame (take the samples as RAWfish-ir's JPG, not a bracket or burst)")
    sets = [s for s in sets if s not in turned]
    views = []
    for s in sets:
        x0, y0, sc = field_in(ref, s['size'])
        s['scale'] = sc
        for v in s['views']:
            v['ref'] = to_ref(v['image'], ref, s['size'])
            v['set'] = s
            views.append(v)
    if len(views) < 5:
        sys.exit("need at least 5 usable photos")

    model_name, rms, mtx, dist, per_view, centre, corner = fit(views, ref)
    keep = per_view <= OUTLIER_FACTOR * np.median(per_view)
    if not keep.all() and keep.sum() >= 5:
        for e, v in zip(per_view[~keep], [v for v, k in zip(views, keep) if not k]):
            print(f"    dropping outlier {v['set']['folder']}/{v['name']} "
                  f"({e / v['set']['scale']:.2f} px, median {np.median(per_view):.2f} px of the reference)")
        views = [v for v, k in zip(views, keep) if k]
        model_name, rms, mtx, dist, per_view, centre, corner = fit(views, ref)

    print(f"    model: {model_name}")
    print(f"    fx {mtx[0, 0]:.1f}  fy {mtx[1, 1]:.1f}  cx {mtx[0, 2]:.1f}  cy {mtx[1, 2]:.1f}")
    print(f"    dist {np.array2string(dist[:8], precision=5, suppress_small=True)}")
    print(f"    RMS reprojection error: {rms:.3f} px of {W}x{H}")
    prec = np.array([v['precision'] for v in views])
    print(f"    corner precision (each corner against its neighbours, no model): median {np.nanmedian(prec):.2f} px")
    blurred = sorted(((p, v) for p, v in zip(prec, views) if p > 2.0), key=lambda t: -t[0])
    if blurred:
        print("    blurred or noisy photos (precision over 2 px): " +
              ", ".join(f"{v['name']} ({p:.1f} px)" for p, v in blurred[:8]) + (" ..." if len(blurred) > 8 else ""))
    if np.nanmedian(prec) > 1.0:
        print("    WARNING: the corners are found imprecisely (over 1 px): the photos are blurred or noisy. "
              "Retake them in good light (ISO 400 or less), the phone still (a fast shutter, or a support)")
    report = []
    for s in sets:
        e = [pv / s['scale'] for pv, v in zip(per_view, views) if v['set'] is s]
        pts = np.vstack([v['ref'] for v in views if v['set'] is s]).reshape(-1, 2)
        cov = np.linalg.norm(pts - centre, axis=1).max() / corner
        n = len(e)
        r = float(np.sqrt(np.mean(np.square(e)))) if n else 0.0
        print(f"    {s['folder']}: {n} photos, RMS {r:.2f} px of its own {s['size'][0]}x{s['size'][1]}, "
              f"reaches {100 * cov:.0f}% of the centre-to-corner distance")
        # per photo size: what the calibration covered (no local paths: the
        # file is shipped)
        report.append({'size': list(s['size']), 'images': n, 'rms_px': round(r, 3)})
        # the folder alone, to check it is the frame's middle as assumed
        own = [v for v in views if v['set'] is s]
        if len(sets) > 1 and len(own) >= 5:
            _, _, m1, _, _, _, _ = fit(own, ref, quiet=True)
            d = 100 * (m1[0, 0] / mtx[0, 0] - 1)
            print(f"      alone: fx {m1[0, 0]:.1f} ({d:+.1f}%), cx {m1[0, 2]:.1f}, cy {m1[1, 2]:.1f}"
                  + ("   <- not the frame's middle? check this folder's photos" if abs(d) > 2 else ""))
    all_pts = np.vstack([v['ref'] for v in views]).reshape(-1, 2)
    covered = np.linalg.norm(all_pts - centre, axis=1).max() / corner
    print(f"    all together reach {100 * covered:.0f}% of the centre-to-corner distance")
    if covered < 0.85:
        print("    WARNING: the samples don't reach the frame's corners; take more "
              "photos with the board in the corners and along the edges")
    return {'ref': ref, 'mtx': mtx, 'dist': dist, 'rms': rms, 'model': model_name,
            'precision': float(np.nanmedian([v['precision'] for v in views])),
            'images': len(views), 'coverage': covered, 'sets': report}


def exif_make_model(folder):
    """Make and Model of the first photo of a folder (Exif IFD0), or None."""
    for f in list_images(folder):
        try:
            data = open(f, 'rb').read(65536)
        except OSError:
            continue
        i = data.find(b'Exif\0\0')
        if i < 0:
            continue
        t = data[i + 6:]
        le = t[:2] == b'II'
        e = '<' if le else '>'

        ifd = struct.unpack(e + 'I', t[4:8])[0]
        n = struct.unpack(e + 'H', t[ifd:ifd + 2])[0]
        out = {}
        for k in range(n):
            tag, typ, cnt, val = struct.unpack(e + 'HHII', t[ifd + 2 + 12 * k:ifd + 14 + 12 * k])
            if tag in (0x010F, 0x0110) and typ == 2:
                raw = t[ifd + 10 + 12 * k:ifd + 10 + 12 * k + cnt] if cnt <= 4 else t[val:val + cnt]
                out[tag] = raw.split(b'\0')[0].decode('ascii', 'replace')
        if 0x010F in out and 0x0110 in out:
            return out[0x010F], out[0x0110]
    return None


def slug(s):
    """As the engine names files (slug_into): lowercase, others as '-'."""
    return re.sub(r'-+', '-', re.sub(r'[^a-z0-9]', '-', s.lower())).strip('-')


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(
        description="Measure a camera's lens distortion from chessboard photos and write "
                    "RAWfish-ir's lens distortion file (one per phone model and camera).")
    ap.add_argument('folders', nargs='+', help='folders of chessboard photos (one size each: '
                    'e.g. samples/wide/43 samples/wide/169)')
    ap.add_argument('--camera-id', required=True, help="the camera's id (rawfish-ir-cli cameras)")
    # --name: a label only (the report's header and the file's "name"); the
    # engine never reads it, the file is found by make, model and camera id.
    ap.add_argument('--name', default='', help='human readable name for sensor (optional), e.g. wide, std, selfie')
    ap.add_argument('--make', help='the phone maker (default: the photos\' Exif Make)')
    ap.add_argument('--model', help='the phone model (default: the photos\' Exif Model)')
    ap.add_argument('--board', default='7x10', help='squares along x and y (default 7x10)')
    ap.add_argument('--square-mm', type=float, default=23.0, help='side of a square in mm (default 23)')
    ap.add_argument('--reference', help='the full frame WxH (default: the largest 4:3 sample size)')
    ap.add_argument('--out', default=os.path.join(here, '..', 'android', 'data', 'lens-distortion'),
                    help='folder to write to (default android/data/lens-distortion), or a .json file')
    ap.add_argument('--debug', help='write the photos with the corners found into this folder')
    a = ap.parse_args()

    bx, by = (int(v) for v in a.board.lower().split('x'))
    set_board(bx, by, a.square_mm)
    mm = exif_make_model(a.folders[0])
    make = a.make or (mm[0] if mm else None)
    model = a.model or (mm[1] if mm else None)
    if not make or not model:
        sys.exit('no Make / Model in the photos\' Exif: give --make and --model')
    ref = tuple(int(v) for v in a.reference.lower().split('x')) if a.reference else None

    print(f"[{make} {model}, camera {a.camera_id}{' (' + a.name + ')' if a.name else ''}]")
    r = calibrate(a.folders, ref, a.debug)
    k = np.zeros(8)
    k[:min(8, r['dist'].size)] = r['dist'][:8]
    doc = {
        'format': FORMAT,
        '_comment': 'Lens distortion of one camera (OpenCV model, its matrix for the frame width x height, '
                    'the camera\'s full frame). Written by scripts/lens-calibrate.py; read by the engine '
                    '(android/lensdist.h). See docs/LENS-DISTORTION.md.',
        'make': make, 'model': model, 'camera_id': str(a.camera_id), 'name': a.name,
        'width': int(r['ref'][0]), 'height': int(r['ref'][1]),
        'fx': round(float(r['mtx'][0, 0]), 4), 'fy': round(float(r['mtx'][1, 1]), 4),
        'cx': round(float(r['mtx'][0, 2]), 4), 'cy': round(float(r['mtx'][1, 2]), 4),
        'dist': [float(f'{v:.8g}') for v in k],
        'fit': {'model': r['model'], 'rms_px': round(float(r['rms']), 4), 'images': r['images'],
                'corner_precision_px': round(r['precision'], 3),
                'coverage_pct': round(100 * float(r['coverage'])), 'samples': r['sets'],
                'board': {'squares': [bx, by], 'square_mm': a.square_mm}},
        'generated': datetime.date.today().isoformat(),
    }
    name = f"distortion_{slug(make)}-{slug(model)}_cam{a.camera_id}.json"
    path = a.out if a.out.endswith('.json') else os.path.join(a.out, name)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'w') as f:
        json.dump(doc, f, indent=2)
        f.write('\n')
    print(f"    written: {os.path.relpath(path)}")
    print("    check it on a photo:  rawfish-ir-cli lensfix " + os.path.relpath(path) + " IN.jpg OUT.jpg")


if __name__ == '__main__':
    main()
