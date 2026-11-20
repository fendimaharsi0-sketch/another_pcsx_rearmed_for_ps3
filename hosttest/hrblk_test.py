"""Host mirror of the 2x dirty-block sync in plugins/dfxvideo/gpulib_if.c.
No C compiler here, so this transliterates the same operations and checks that
hr_sync downsamples (and stops tracking) exactly the stale blocks inside the rect.
"""

HR_BW, HR_BH = 64, 32
HR_BB = HR_BW * HR_BH
HR_BBYTES = (HR_BB + 7) // 8
blk = bytearray(HR_BBYTES)
down_calls = []


def hr_blk_range(b0, b1, on):
    if b0 < 0:
        b0 = 0
    if b1 > HR_BB:
        b1 = HR_BB
    if b1 <= b0:
        return
    while b0 < b1 and (b0 & 7):
        blk[b0 >> 3] = (blk[b0 >> 3] | (1 << (b0 & 7))) if on else (blk[b0 >> 3] & ~(1 << (b0 & 7)))
        b0 += 1
    while b0 + 8 <= b1:
        blk[b0 >> 3] = 0xFF if on else 0x00
        b0 += 8
    while b0 < b1:
        blk[b0 >> 3] = (blk[b0 >> 3] | (1 << (b0 & 7))) if on else (blk[b0 >> 3] & ~(1 << (b0 & 7)))
        b0 += 1


def hr_blk_set(x0, y0, x1, y1):
    bx0, bx1 = x0 >> 4, (x1 - 1) >> 4
    by0, by1 = y0 >> 4, (y1 - 1) >> 4
    if bx1 < bx0:
        bx1 = bx0
    if by1 < by0:
        by1 = by0
    for by in range(by0, by1 + 1):
        hr_blk_range(by * HR_BW + bx0, by * HR_BW + bx1 + 1, 1)


def hr_blk_clear(x0, y0, x1, y1):
    bx0, bx1 = x0 >> 4, (x1 - 1) >> 4
    by0, by1 = y0 >> 4, (y1 - 1) >> 4
    if bx1 < bx0:
        bx1 = bx0
    if by1 < by0:
        by1 = by0
    for by in range(by0, by1 + 1):
        hr_blk_range(by * HR_BW + bx0, by * HR_BW + bx1 + 1, 0)


def hr_blk_any():
    return any(blk)


def hr_down(x0, y0, x1, y1):
    down_calls.append((x0, y0, x1, y1))


def hr_sync_blocks(x, y, w, h):
    if x < 0:
        w += x; x = 0
    if y < 0:
        h += y; y = 0
    if x + w > 1024:
        w = 1024 - x
    if y + h > 512:
        h = 512 - y
    if w <= 0 or h <= 0:
        return
    bx0, bx1 = x >> 4, (x + w - 1) >> 4
    by0, by1 = y >> 4, (y + h - 1) >> 4
    for by in range(by0, by1 + 1):
        run = -1
        for bx in range(bx0, bx1 + 2):
            s = (bx <= bx1) and (blk[(by * HR_BW + bx) >> 3] & (1 << ((by * HR_BW + bx) & 7)))
            if s and run < 0:
                run = bx
            if not s and run >= 0:
                hr_down(run << 4, by << 4, bx << 4, (by << 4) + 16)
                hr_blk_clear(run << 4, by << 4, bx << 4, (by << 4) + 16)
                run = -1


def hr_sync(x, y, w, h):
    if not hr_blk_any():
        return
    hr_sync_blocks(x, y, w, h)


def hr_sync_all():
    if not hr_blk_any():
        return
    hr_sync_blocks(0, 0, 1024, 512)
    blk[:] = bytes(HR_BBYTES)


def blocks():
    return sorted(((b >> 6), (b & 63)) for b in range(HR_BB) if blk[b >> 3] & (1 << (b & 7)))


def px(calls):
    return sum((x1 - x0) * (y1 - y0) for x0, y0, x1, y1 in calls)


fails = 0


def chk(name, got, want):
    global fails
    if got != want:
        print("FAIL %s: got %r want %r" % (name, got, want))
        fails += 1
    else:
        print("ok   %s = %r" % (name, got))


def reset():
    blk[:] = bytes(HR_BBYTES)
    del down_calls[:]


# 1. rect = blocks x4..5, y2..3: copied as one run per block row
reset()
hr_blk_set(64, 32, 128, 96)          # blocks x4..7, y2..5
hr_sync(64, 32, 32, 32)              # blocks x4..5, y2..3
chk("1 calls", down_calls, [(64, 32, 96, 48), (64, 48, 96, 64)])
chk("1 pixels", px(down_calls), 1024)
chk("1 remaining", blocks(), [(2, 6), (2, 7), (3, 6), (3, 7), (4, 4), (4, 5), (4, 6), (4, 7), (5, 4), (5, 5), (5, 6), (5, 7)])

# 2. same rect again: nothing left, no second drain
hr_sync(64, 32, 32, 32)
chk("2 no new calls", len(down_calls), 2)

# 3. a rect with no stale block: nothing
hr_sync(512, 256, 64, 64)
chk("3 no calls", len(down_calls), 2)

# 4. partial overlap: only the runs that touch a stale block
hr_sync(80, 32, 48, 96)              # blocks x5..7, y2..7
chk("4 calls", down_calls[2:], [(96, 32, 128, 48), (96, 48, 128, 64), (80, 64, 128, 80), (80, 80, 128, 96)])

# 5. hole: syncing the hole copies nothing
reset()
hr_blk_set(0, 0, 1024, 512)
hr_blk_clear(256, 128, 768, 384)
hr_sync(256, 128, 512, 256)
chk("5 hole no calls", down_calls, [])

# 6. sync_all covers every stale run and clears the map
hr_sync_all()
chk("6 any after all", hr_blk_any(), False)
chk("6 hole not copied", (256, 128, 768, 144) in down_calls, False)
chk("6 beside hole copied", (0, 128, 256, 144) in down_calls, True)
chk("6 above hole copied", (0, 0, 1024, 16) in down_calls, True)

# 7. two disjoint runs in one block row -> two hr_down calls, not a bounding box
reset()
hr_blk_set(0, 0, 64, 16)
hr_blk_set(960, 0, 1024, 16)
hr_sync(0, 0, 1024, 16)
chk("7 two runs", down_calls, [(0, 0, 64, 16), (960, 0, 1024, 16)])
chk("7 pixels", px(down_calls), 2048)

# 8. far edge: two block rows
reset()
hr_blk_set(1000, 480, 1024, 512)
hr_sync(1000, 480, 24, 32)
chk("8 far edge", down_calls, [(992, 480, 1024, 496), (992, 496, 1024, 512)])

# 9. a rect hanging off the VRAM is clipped, then clears
reset()
hr_blk_set(1000, 480, 1024, 512)
hr_sync(992, 480, 200, 200)
chk("9 clipped", down_calls, [(992, 480, 1024, 496), (992, 496, 1024, 512)])
chk("9 cleared", blocks(), [])

print("\n%d FAILURE(S)" % fails if fails else "\nall checks passed")
raise SystemExit(1 if fails else 0)
