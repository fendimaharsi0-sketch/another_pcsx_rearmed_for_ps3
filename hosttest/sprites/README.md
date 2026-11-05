Sprite polygons (normal, mirrored, flipped rectangles, tiled) checked against the
software renderer at 1x, 2x and 4x, subpixel by subpixel.

    python3 gen.py t.bin
    ../gp0replay/gp0replay t.bin soft.vram                  # software renderer
    for s in 1 2 4; do HWR_GL=$s ../gp0replay/gp0replay t.bin gl$s.vram gl$s.ppm; done
    python3 compare.py 1 2 4                                # 0 differing everywhere

(gp0replay built with hwr_gl.c, hwr_trace.c and hwr_core.c, as in build.sh.)
Before build 100 every mirrored case was one texel off, and at 2x and above
every sprite polygon was half a pixel off.
