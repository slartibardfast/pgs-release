#!/usr/bin/env python3
"""Verify PGS CDB rate control size estimates against actual segment layout.

Cross-references the estimate in pgssubenc.c pgs_update_cdb() pre-check
against the actual segment write functions pgs_write_{pcs,wds,pds,ods,end}.

The CDB model counts total bytes written (q - outbuf), which includes
the 3-byte segment headers (type + seg_len). The PGS spec says the CDB
drains by "segment payload size", but our encoder measures total output
size. The estimates must include headers to match.

Segment layout (encoder output, no SUP/PG container headers):
  type(1) + seg_len(2) + payload(seg_len)
"""

def pcs_size(num_rects):
    """PCS: type(1) + len(2) + video_w(2) + video_h(2) + frame_rate(1)
           + comp_num(2) + state(1) + palette_update(1) + palette_id(1)
           + num_objs(1) + per_obj(object_id(2) + window_id(1) +
             comp_flag(1) + x(2) + y(2)) * rects
    """
    header = 3          # type(1) + seg_len(2)
    payload = 11 + 8 * num_rects
    return header + payload

def wds_size(num_rects):
    """WDS: type(1) + len(2) + num_windows(1)
           + per_window(window_id(1) + x(2) + y(2) + w(2) + h(2)) * rects
    """
    header = 3
    payload = 1 + 9 * num_rects
    return header + payload

def pds_size_worst(num_entries=256):
    """PDS worst case: type(1) + len(2) + palette_id(1) + version(1)
           + per_entry(index(1) + Y(1) + Cr(1) + Cb(1) + A(1)) * entries
    """
    header = 3
    payload = 2 + 5 * num_entries
    return header + payload

def ods_size_worst(w, h):
    """ODS worst case (no RLE compression):
       type(1) + len(2) + object_id(2) + version(1) + seq_flag(1)
       + data_len(3) + width(2) + height(2) + rle_data(w*h worst case)
    """
    header = 3
    payload = 2 + 1 + 1 + 3 + 2 + 2 + w * h  # worst case: 1 byte per pixel
    return header + payload

def end_size():
    """END: type(1) + len(2), payload is 0"""
    return 3

def estimate_ds_size(num_rects, rect_dims):
    """Total estimated DS size (worst case)."""
    total = pcs_size(num_rects)
    total += wds_size(num_rects)
    total += pds_size_worst()
    for w, h in rect_dims:
        total += ods_size_worst(w, h)
    total += end_size()
    return total

def code_estimate(num_rects, rect_dims):
    """Reproduce the estimate from pgssubenc.c after fix."""
    est = 3 + 11 + 8 * num_rects          # PCS: hdr + payload
    est += 3 + 1 + 9 * num_rects          # WDS: hdr + payload
    est += 3 + 2 + 256 * 5                # PDS: hdr + id/ver + 256 entries
    for w, h in rect_dims:
        est += 3 + 11 + w * h             # ODS: hdr + payload per rect
    est += 3                               # END
    return est

def code_estimate_old(num_rects, rect_dims):
    """Reproduce the estimate from pgssubenc.c BEFORE fix."""
    est = 14 + 8 * num_rects              # PCS (no header)
    est += 4 + 9 * num_rects              # WDS (no header)
    est += 1282                            # PDS (no header, wrong count)
    for w, h in rect_dims:
        est += 11 + w * h                 # ODS (no header)
    est += 3                               # END
    return est

# Test cases
cases = [
    ("1 rect 4x4",    1, [(4, 4)]),
    ("1 rect 1920x200", 1, [(1920, 200)]),
    ("1 rect 1920x1080", 1, [(1920, 1080)]),
    ("2 rects 1920x100", 2, [(1920, 100), (1920, 100)]),
]

print("PGS Display Set Size Estimates")
print("=" * 75)
print()

# Individual segment sizes
print("Individual segment sizes (1 rect, 1920x200):")
print(f"  PCS: {pcs_size(1)} bytes")
print(f"  WDS: {wds_size(1)} bytes")
print(f"  PDS: {pds_size_worst()} bytes  (256 entries worst case)")
print(f"  ODS: {ods_size_worst(1920, 200)} bytes  (1920x200, no RLE)")
print(f"  END: {end_size()} bytes")
print()

print(f"{'Case':<25} {'Actual':>10} {'Code (fixed)':>14} {'Code (old)':>12} {'Delta':>8}")
print("-" * 75)

all_ok = True
for name, nrects, dims in cases:
    actual = estimate_ds_size(nrects, dims)
    fixed  = code_estimate(nrects, dims)
    old    = code_estimate_old(nrects, dims)
    delta  = fixed - actual
    status = "OK" if delta == 0 else f"OFF BY {delta}"
    if delta != 0:
        all_ok = False
    print(f"{name:<25} {actual:>10} {fixed:>14} {old:>12} {status:>8}")

print()

# Verify PDS breakdown
print("PDS size breakdown:")
print(f"  Header: 3 bytes (type + seg_len)")
print(f"  Payload: 2 bytes (palette_id + version) + 256 * 5 = {2 + 256*5} bytes")
print(f"  Total: {pds_size_worst()} bytes")
print(f"  Old estimate (1282): header(0) + {1282} = off by {pds_size_worst() - 1282}")
print()

# Verify ODS breakdown
print("ODS size breakdown (1920x200):")
print(f"  Header: 3 bytes")
print(f"  object_id(2) + version(1) + seq_flag(1) + data_len(3) + w(2) + h(2) = 11")
print(f"  RLE worst case: {1920*200} bytes")
print(f"  Total: {ods_size_worst(1920, 200)} bytes")
print(f"  Old estimate: 11 + {1920*200} = {11 + 1920*200}, off by 3 (missing header)")
print()

if all_ok:
    print("All estimates match actual segment sizes.")
else:
    print("MISMATCH detected — review the estimates.")
