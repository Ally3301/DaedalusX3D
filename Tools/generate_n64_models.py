import math
import os
import struct
import zlib

# ==============================================================================
# Blender-Grade 3D Parametric Mesh Generator for Nintendo 64 Game Pak Cartridge
# Generates:
#   1. Source/SysCTR/Resources/n64_cartridge.obj (Wavefront OBJ 3D model)
#   2. Source/SysCTR/UI/N64CartridgeMesh.h       (C++ 3D mesh header for Citro3D)
#   3. scratch/cartridge_3d_preview.png          (Rendered 3D verification preview)
# ==============================================================================

MAT_SHELL_MAIN    = 0  # Authentic N64 warm medium gray plastic (#949AA6)
MAT_SHELL_WING    = 1  # Slightly deeper gray outer wings (#848A96)
MAT_SHELL_BEVEL   = 2  # Smooth molded chamfer/bevel (#A6ACB8)
MAT_SHELL_GROOVE  = 3  # Deep recessed grip rib / parting seam (#484E5A)
MAT_CARTOUCHE     = 4  # Recessed top Nintendo oval & bottom arrow (#686E7A)
MAT_STICKER_RIM   = 5  # Recessed 45-deg sticker cavity rim (#5C626E)
MAT_PCB_CHAMBER   = 6  # Dark bottom PCB slot opening (#1C2028)
MAT_PCB_PINS      = 7  # Gold/copper N64 PCB edge connector pins (#D4A838)
MAT_STICKER_TEX   = 8  # UV-mapped game cover sticker (0..1 UV)

def v_sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])

def v_add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])

def v_cross(a, b):
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )

def v_dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

def v_norm(a):
    l = math.sqrt(max(1e-12, v_dot(a, a)))
    return (a[0] / l, a[1] / l, a[2] / l)

class MeshBuilder:
    def __init__(self):
        self.verts = []
        self.tris  = []

    def add_vert(self, x, y, z):
        idx = len(self.verts)
        self.verts.append((float(x), float(y), float(z)))
        return idx

    def add_tri(self, p0, p1, p2, mat, uv0=(0.0,0.0), uv1=(0.0,0.0), uv2=(0.0,0.0), n_override=None):
        i0 = self.add_vert(*p0)
        i1 = self.add_vert(*p1)
        i2 = self.add_vert(*p2)
        if n_override is not None:
            n0, n1, n2 = n_override
        else:
            fn = v_norm(v_cross(v_sub(p2, p0), v_sub(p1, p0)))
            n0 = n1 = n2 = fn
        self.tris.append((i0, i1, i2, n0, n1, n2, uv0, uv1, uv2, mat))

    def add_quad(self, tl, tr, br, bl, mat,
                 uv_tl=(0.0,0.0), uv_tr=(1.0,0.0), uv_br=(1.0,1.0), uv_bl=(0.0,1.0),
                 n_tl=None, n_tr=None, n_br=None, n_bl=None):
        if n_tl is not None:
            self.add_tri(tl, tr, br, mat, uv_tl, uv_tr, uv_br, (n_tl, n_tr, n_br))
            self.add_tri(tl, br, bl, mat, uv_tl, uv_br, uv_bl, (n_tl, n_br, n_bl))
        else:
            self.add_tri(tl, tr, br, mat, uv_tl, uv_tr, uv_br)
            self.add_tri(tl, br, bl, mat, uv_tl, uv_br, uv_bl)


def build_n64_cartridge():
    m = MeshBuilder()

    HW_TOP   = 50.0   # Top shoulder half-width
    HW_BOT   = 48.8   # Subtle authentic bottom taper
    TW_X     = 30.5   # Central raised label tower half-width
    STEP_W   = 2.2    # Width of 3D angled bevel step between center tower and wings
    Y_BOT    = 32.5   # Bottom edge of outer wings (feet)
    Y_BOT_IN = 30.6   # Stepped-up bottom center edge above PCB connector

    Z_FRONT_TOWER = -10.2
    Z_FRONT_WING  = -8.2
    Z_CHAMFER_RIM = -5.6
    Z_BACK_CHAMF  =  5.6

    # Smooth N64 top arch with true rounded corner fillets at |x| -> HW_TOP
    def arch_y(x):
        u = min(1.0, abs(x) / HW_TOP)
        # Crown at x=0 is -37.5; curves smoothly down to -25.0 at the outer shoulder corners!
        dome = 5.5 * (1.0 - (u ** 2.0))
        fillet = 7.0 * (max(0.0, (u - 0.70) / 0.30) ** 2.2)
        return -32.0 - dome + fillet

    def side_x(y_norm, sign):
        return sign * (HW_TOP + (HW_BOT - HW_TOP) * y_norm)

    def front_z_base(x):
        ax = abs(x)
        if ax <= TW_X:
            t = ax / TW_X
            return Z_FRONT_TOWER - 0.50 * (1.0 - t * t)
        elif ax <= TW_X + STEP_W:
            t = (ax - TW_X) / STEP_W
            return Z_FRONT_TOWER + (Z_FRONT_WING - Z_FRONT_TOWER) * t
        else:
            t = (ax - (TW_X + STEP_W)) / max(1e-4, (HW_TOP - (TW_X + STEP_W)))
            return Z_FRONT_WING + 1.25 * (t ** 1.6)

    def back_z_base(x):
        return -front_z_base(x) * 0.94

    # Denser X grid around the outer shoulders for silky-smooth rounded top corners
    xs = [
        -50.0, -49.0, -47.4, -45.0, -41.5, -37.5, -32.7,
        -30.5, -27.5, -21.0, -14.0, -7.0, 0.0, 7.0, 14.0, 21.0, 27.5, 30.5,
        32.7, 37.5, 41.5, 45.0, 47.4, 49.0, 50.0
    ]

    def front_normal(x, y_slope=0.0):
        eps = 0.35
        dzdx = (front_z_base(x + eps) - front_z_base(x - eps)) / (2.0 * eps)
        return v_norm((dzdx, y_slope, -1.0))

    # ==========================================================================
    # PART A: 3D Curved Top Arch Clamshell with Rounded Corner Fillets
    # ==========================================================================
    BEVEL_Y = 2.5
    for i in range(len(xs) - 1):
        x0, x1 = xs[i], xs[i + 1]
        xm = 0.5 * (x0 + x1)
        y0, y1 = arch_y(x0), arch_y(x1)
        zf0, zf1 = front_z_base(x0), front_z_base(x1)
        zb0, zb1 = back_z_base(x0), back_z_base(x1)

        mat_top = MAT_SHELL_BEVEL if abs(xm) <= TW_X + STEP_W else MAT_SHELL_WING

        # Smooth tangent-derived normal at x0 and x1 so the rounded shoulder is G1 smooth!
        def arch_n(x):
            eps = 0.25
            dy = arch_y(min(HW_TOP, x + eps)) - arch_y(max(-HW_TOP, x - eps))
            dx = min(HW_TOP, x + eps) - max(-HW_TOP, x - eps)
            return v_norm((dy, -dx, 0.0))

        n0_mid = arch_n(x0)
        n1_mid = arch_n(x1)
        n0_front = v_norm((n0_mid[0] * 0.65, n0_mid[1] * 0.65, -0.75))
        n1_front = v_norm((n1_mid[0] * 0.65, n1_mid[1] * 0.65, -0.75))
        n0_back  = v_norm((n0_mid[0] * 0.65, n0_mid[1] * 0.65,  0.75))
        n1_back  = v_norm((n1_mid[0] * 0.65, n1_mid[1] * 0.65,  0.75))

        tl = (x0, y0, Z_CHAMFER_RIM)
        tr = (x1, y1, Z_CHAMFER_RIM)
        br = (x1, y1 + BEVEL_Y, zf1)
        bl = (x0, y0 + BEVEL_Y, zf0)
        m.add_quad(tl, tr, br, bl, mat_top,
                   n_tl=n0_mid, n_tr=n1_mid, n_br=n1_front, n_bl=n0_front)

        m.add_quad((x0, y0, -0.45), (x1, y1, -0.45), tr, tl, mat_top,
                   n_tl=n0_mid, n_tr=n1_mid, n_br=n1_mid, n_bl=n0_mid)

        m.add_quad((x0, y0, 0.45), (x1, y1, 0.45), (x1, y1, -0.45), (x0, y0, -0.45),
                   MAT_SHELL_GROOVE, n_tl=n0_mid, n_tr=n1_mid, n_br=n1_mid, n_bl=n0_mid)

        tl_b = (x0, y0, Z_BACK_CHAMF)
        tr_b = (x1, y1, Z_BACK_CHAMF)
        m.add_quad(tl_b, tr_b, (x1, y1, 0.45), (x0, y0, 0.45), mat_top,
                   n_tl=n0_mid, n_tr=n1_mid, n_br=n1_mid, n_bl=n0_mid)

        m.add_quad((x0, y0 + BEVEL_Y, zb0), (x1, y1 + BEVEL_Y, zb1), tr_b, tl_b, mat_top,
                   n_tl=n0_back, n_tr=n1_back, n_br=n1_mid, n_bl=n0_mid)

    # ==========================================================================
    # PART B: Sculpted Front Shell (Outer Wings + 3D Step Chamfers + Grip Grooves on Outer Rim)
    # ==========================================================================
    y_side_top = arch_y(HW_TOP) + BEVEL_Y
    y_side_bot = Y_BOT

    # Vertical Y segmentation matching the 5 horizontal grip ribs so ribs wrap onto the outer wing edge!
    wing_y_pts = [0.0]
    wing_rib_type = [] # 0 = normal wing, 1 = groove top bevel, 2 = groove dark floor, 3 = groove bot highlight
    for g in range(5):
        # Normalized vertical positions of the 5 grip ribs
        t_g0 = 0.14 + g * 0.155
        t_g1 = t_g0 + 0.018
        t_g2 = t_g0 + 0.062
        t_g3 = t_g0 + 0.080
        wing_y_pts.extend([t_g0, t_g1, t_g2, t_g3])
        wing_rib_type.extend([0, 1, 2, 3])
    wing_y_pts.append(1.0)
    wing_rib_type.append(0)

    for i in range(len(xs) - 1):
        x0, x1 = xs[i], xs[i + 1]
        xm = 0.5 * (x0 + x1)
        if abs(xm) < 27.5:
            continue

        y_top0 = arch_y(x0) + BEVEL_Y
        y_top1 = arch_y(x1) + BEVEL_Y
        y_bot0 = Y_BOT if abs(x0) >= TW_X else Y_BOT_IN
        y_bot1 = Y_BOT if abs(x1) >= TW_X else Y_BOT_IN

        zf0, zf1 = front_z_base(x0), front_z_base(x1)
        n_col0 = front_normal(x0)
        n_col1 = front_normal(x1)

        is_outer_grip_strip = (abs(xm) > 45.0)
        if abs(xm) <= TW_X:
            base_mat = MAT_SHELL_MAIN
        elif abs(xm) <= TW_X + STEP_W:
            base_mat = MAT_SHELL_BEVEL
        else:
            base_mat = MAT_SHELL_WING

        for b in range(len(wing_y_pts) - 1):
            t0 = wing_y_pts[b]
            t1 = wing_y_pts[b + 1]
            rtype = wing_rib_type[b]

            ya0 = y_top0 if b == 0 else (y_side_top + (y_bot0 - y_side_top) * t0)
            ya1 = y_top1 if b == 0 else (y_side_top + (y_bot1 - y_side_top) * t0)
            yb0 = y_side_top + (y_bot0 - y_side_top) * t1
            yb1 = y_side_top + (y_bot1 - y_side_top) * t1

            s_taper0 = 1.0 + ((HW_BOT - HW_TOP) / HW_TOP) * t0 if abs(xm) > TW_X else 1.0
            s_taper1 = 1.0 + ((HW_BOT - HW_TOP) / HW_TOP) * t1 if abs(xm) > TW_X else 1.0

            mat_cell = base_mat
            n0_cell, n1_cell = n_col0, n_col1
            if is_outer_grip_strip and rtype != 0:
                if rtype == 1:
                    mat_cell = MAT_SHELL_GROOVE
                    n0_cell = v_norm((n_col0[0], 0.55, n_col0[2]))
                    n1_cell = v_norm((n_col1[0], 0.55, n_col1[2]))
                elif rtype == 2:
                    mat_cell = MAT_SHELL_GROOVE
                elif rtype == 3:
                    mat_cell = MAT_SHELL_BEVEL
                    n0_cell = v_norm((n_col0[0], -0.65, n_col0[2]))
                    n1_cell = v_norm((n_col1[0], -0.65, n_col1[2]))

            tl = (x0 * s_taper0, ya0, zf0)
            tr = (x1 * s_taper0, ya1, zf1)
            br = (x1 * s_taper1, yb1, zf1)
            bl = (x0 * s_taper1, yb0, zf0)
            m.add_quad(tl, tr, br, bl, mat_cell, n_tl=n0_cell, n_tr=n1_cell, n_br=n1_cell, n_bl=n0_cell)

    # ==========================================================================
    # PART C: High-Detail Central Tower Front (x in [-27.5, +27.5])
    # ==========================================================================
    ST_X   = 25.5
    ST_IN  = 24.0
    ST_Y0  = -24.8
    ST_Y1  =  15.2
    ST_IY0 = -23.3
    ST_IY1 =  13.7
    Z_ST_DEPTH = Z_FRONT_TOWER + 1.35

    tower_xs = [-27.5, -25.5, -16.0, -10.0, 0.0, 10.0, 16.0, 25.5, 27.5]
    for i in range(len(tower_xs) - 1):
        x0, x1 = tower_xs[i], tower_xs[i + 1]
        xm = 0.5 * (x0 + x1)
        yt0 = arch_y(x0) + BEVEL_Y
        yt1 = arch_y(x1) + BEVEL_Y
        zf0, zf1 = front_z_base(x0), front_z_base(x1)
        n0, n1 = front_normal(x0), front_normal(x1)

        if abs(xm) < 16.0:
            cy0, cy1 = -32.2, -27.4
            m.add_quad((x0, yt0, zf0), (x1, yt1, zf1), (x1, cy0, zf1), (x0, cy0, zf0),
                       MAT_SHELL_MAIN, n_tl=n0, n_tr=n1, n_br=n1, n_bl=n0)
            zc0, zc1 = zf0 + 0.85, zf1 + 0.85
            m.add_quad((x0, cy0, zf0), (x1, cy0, zf1), (x1, cy0 + 0.9, zc1), (x0, cy0 + 0.9, zc0),
                       MAT_STICKER_RIM)
            m.add_quad((x0, cy0 + 0.9, zc0), (x1, cy0 + 0.9, zc1), (x1, cy1 - 0.9, zc1), (x0, cy1 - 0.9, zc0),
                       MAT_CARTOUCHE, n_tl=n0, n_tr=n1, n_br=n1, n_bl=n0)
            m.add_quad((x0, cy1 - 0.9, zc0), (x1, cy1 - 0.9, zc1), (x1, cy1, zf1), (x0, cy1, zf0),
                       MAT_SHELL_BEVEL)
            m.add_quad((x0, cy1, zf0), (x1, cy1, zf1), (x1, ST_Y0, zf1), (x0, ST_Y0, zf0),
                       MAT_SHELL_MAIN, n_tl=n0, n_tr=n1, n_br=n1, n_bl=n0)
        else:
            m.add_quad((x0, yt0, zf0), (x1, yt1, zf1), (x1, ST_Y0, zf1), (x0, ST_Y0, zf0),
                       MAT_SHELL_MAIN, n_tl=n0, n_tr=n1, n_br=n1, n_bl=n0)

    for sign in (-1.0, 1.0):
        xa = -27.5 if sign < 0 else ST_X
        xb = -ST_X if sign < 0 else 27.5
        zfa, zfb = front_z_base(xa), front_z_base(xb)
        na, nb = front_normal(xa), front_normal(xb)
        m.add_quad((xa, ST_Y0, zfa), (xb, ST_Y0, zfb), (xb, ST_Y1, zfb), (xa, ST_Y1, zfa),
                   MAT_SHELL_MAIN, n_tl=na, n_tr=nb, n_br=nb, n_bl=na)

    # 3D 45-Degree Recessed Bevel Frame around Sticker Cavity
    m.add_quad((-ST_X, ST_Y0, front_z_base(-ST_X)), (ST_X, ST_Y0, front_z_base(ST_X)),
               (ST_IN, ST_IY0, Z_ST_DEPTH), (-ST_IN, ST_IY0, Z_ST_DEPTH),
               MAT_STICKER_RIM)
    m.add_quad((-ST_IN, ST_IY1, Z_ST_DEPTH), (ST_IN, ST_IY1, Z_ST_DEPTH),
               (ST_X, ST_Y1, front_z_base(ST_X)), (-ST_X, ST_Y1, front_z_base(-ST_X)),
               MAT_SHELL_BEVEL)
    m.add_quad((-ST_X, ST_Y0, front_z_base(-ST_X)), (-ST_IN, ST_IY0, Z_ST_DEPTH),
               (-ST_IN, ST_IY1, Z_ST_DEPTH), (-ST_X, ST_Y1, front_z_base(-ST_X)),
               MAT_STICKER_RIM)
    m.add_quad((ST_IN, ST_IY0, Z_ST_DEPTH), (ST_X, ST_Y0, front_z_base(ST_X)),
               (ST_X, ST_Y1, front_z_base(ST_X)), (ST_IN, ST_IY1, Z_ST_DEPTH),
               MAT_SHELL_BEVEL)

    # 6x5 Perspective-Subdivided Curved UV Sticker Mesh
    ST_COLS = 6
    ST_ROWS = 5
    for r in range(ST_ROWS):
        v0 = r / float(ST_ROWS)
        v1 = (r + 1) / float(ST_ROWS)
        sy0 = ST_IY0 + (ST_IY1 - ST_IY0) * v0
        sy1 = ST_IY0 + (ST_IY1 - ST_IY0) * v1
        for c in range(ST_COLS):
            u0 = c / float(ST_COLS)
            u1 = (c + 1) / float(ST_COLS)
            sx0 = -ST_IN + (2.0 * ST_IN) * u0
            sx1 = -ST_IN + (2.0 * ST_IN) * u1
            sz0 = front_z_base(sx0) + 1.25
            sz1 = front_z_base(sx1) + 1.25
            n0 = front_normal(sx0)
            n1 = front_normal(sx1)
            m.add_quad((sx0, sy0, sz0), (sx1, sy0, sz1), (sx1, sy1, sz1), (sx0, sy1, sz0),
                       MAT_STICKER_TEX,
                       uv_tl=(u0, v0), uv_tr=(u1, v0), uv_br=(u1, v1), uv_bl=(u0, v1),
                       n_tl=n0, n_tr=n1, n_br=n1, n_bl=n0)

    # Lower Tower Area below Sticker (y = ST_Y1 .. PCB_Y)
    PCB_Y = 26.8
    for i in range(len(tower_xs) - 1):
        x0, x1 = tower_xs[i], tower_xs[i + 1]
        zf0, zf1 = front_z_base(x0), front_z_base(x1)
        n0, n1 = front_normal(x0), front_normal(x1)
        m.add_quad((x0, ST_Y1, zf0), (x1, ST_Y1, zf1), (x1, PCB_Y, zf1), (x0, PCB_Y, zf0),
                   MAT_SHELL_MAIN, n_tl=n0, n_tr=n1, n_br=n1, n_bl=n0)

    # Iconic 3D Embossed Directional Arrow Triangle (▼) below the label (placed at front_z - 0.12 so it renders on the front face!)
    arr_z = front_z_base(0.0) - 0.14
    m.add_tri((-5.4, 18.0, arr_z), (5.4, 18.0, arr_z), (0.0, 24.4, arr_z),
              MAT_CARTOUCHE, n_override=((0.0, 0.35, -0.94), (0.0, 0.35, -0.94), (0.0, -0.25, -0.97)))
    # Crisp bottom highlight edge on the ▼ arrow
    m.add_tri((-5.4, 18.0, arr_z - 0.05), (5.4, 18.0, arr_z - 0.05), (0.0, 18.8, arr_z - 0.05),
              MAT_STICKER_RIM)

    # Bottom Dark PCB Connector Chamber + Gold PCB Edge Pins (y = PCB_Y .. Y_BOT_IN)
    z_pcb = Z_FRONT_TOWER + 3.2
    m.add_quad((-27.5, PCB_Y, front_z_base(-27.5)), (27.5, PCB_Y, front_z_base(27.5)),
               (27.5, PCB_Y + 1.2, z_pcb), (-27.5, PCB_Y + 1.2, z_pcb),
               MAT_STICKER_RIM)
    m.add_quad((-27.5, PCB_Y + 1.2, z_pcb), (27.5, PCB_Y + 1.2, z_pcb),
               (27.5, Y_BOT_IN, z_pcb), (-27.5, Y_BOT_IN, z_pcb),
               MAT_PCB_CHAMBER)
    for p in range(10):
        px0 = -21.5 + p * 4.5
        px1 = px0 + 2.4
        m.add_quad((px0, PCB_Y + 1.6, z_pcb - 0.3), (px1, PCB_Y + 1.6, z_pcb - 0.3),
                   (px1, Y_BOT_IN - 0.3, z_pcb - 0.3), (px0, Y_BOT_IN - 0.3, z_pcb - 0.3),
                   MAT_PCB_PINS)

    # ==========================================================================
    # PART D: Clean Sculpted Left & Right Side Walls with 5 Molded Grip Ribs
    # ==========================================================================
    zf_edge = front_z_base(HW_TOP)
    zb_edge = back_z_base(HW_TOP)

    for sign in (-1.0, 1.0):
        for b in range(len(wing_y_pts) - 1):
            t0 = wing_y_pts[b]
            t1 = wing_y_pts[b + 1]
            rtype = wing_rib_type[b]

            ya = y_side_top + (y_side_bot - y_side_top) * t0
            yb = y_side_top + (y_side_bot - y_side_top) * t1

            # Subtle 0.65-unit inward rib indent inside the 5 grip grooves
            inda = 0.65 if rtype in (2, 3) else 0.0
            indb = 0.65 if rtype in (1, 2) else 0.0

            x_out_a = side_x(t0, sign)
            x_out_b = side_x(t1, sign)
            xa = x_out_a - sign * inda
            xb = x_out_b - sign * indb

            if rtype == 2:
                mat_s = MAT_SHELL_GROOVE
                ny_s = 0.0
            elif rtype == 1:
                mat_s = MAT_SHELL_GROOVE
                ny_s = 0.65
            elif rtype == 3:
                mat_s = MAT_SHELL_BEVEL
                ny_s = -0.75
            else:
                mat_s = MAT_SHELL_WING
                ny_s = 0.0

            n_side_f = v_norm((sign * 0.72, ny_s, -0.68))
            n_side_m = v_norm((sign * 0.96, ny_s,  0.0))
            n_side_b = v_norm((sign * 0.72, ny_s,  0.68))

            if sign > 0:
                m.add_quad((x_out_a, ya, zf_edge), (xa, ya, Z_CHAMFER_RIM),
                           (xb, yb, Z_CHAMFER_RIM), (x_out_b, yb, zf_edge),
                           mat_s, n_tl=n_side_f, n_tr=n_side_m, n_br=n_side_m, n_bl=n_side_f)
                m.add_quad((xa, ya, Z_CHAMFER_RIM), (xa, ya, -0.45),
                           (xb, yb, -0.45), (xb, yb, Z_CHAMFER_RIM),
                           mat_s, n_tl=n_side_m, n_tr=n_side_m, n_br=n_side_m, n_bl=n_side_m)
                m.add_quad((xa, ya, -0.45), (xa, ya, 0.45),
                           (xb, yb, 0.45), (xb, yb, -0.45),
                           MAT_SHELL_GROOVE, n_tl=n_side_m, n_tr=n_side_m, n_br=n_side_m, n_bl=n_side_m)
                m.add_quad((xa, ya, 0.45), (xa, ya, Z_BACK_CHAMF),
                           (xb, yb, Z_BACK_CHAMF), (xb, yb, 0.45),
                           mat_s, n_tl=n_side_m, n_tr=n_side_m, n_br=n_side_m, n_bl=n_side_m)
                m.add_quad((xa, ya, Z_BACK_CHAMF), (x_out_a, ya, zb_edge),
                           (x_out_b, yb, zb_edge), (xb, yb, Z_BACK_CHAMF),
                           mat_s, n_tl=n_side_m, n_tr=n_side_b, n_br=n_side_b, n_bl=n_side_m)
            else:
                m.add_quad((x_out_a, ya, zb_edge), (xa, ya, Z_BACK_CHAMF),
                           (xb, yb, Z_BACK_CHAMF), (x_out_b, yb, zb_edge),
                           mat_s, n_tl=n_side_b, n_tr=n_side_m, n_br=n_side_m, n_bl=n_side_b)
                m.add_quad((xa, ya, Z_BACK_CHAMF), (xa, ya, 0.45),
                           (xb, yb, 0.45), (xb, yb, Z_BACK_CHAMF),
                           mat_s, n_tl=n_side_m, n_tr=n_side_m, n_br=n_side_m, n_bl=n_side_m)
                m.add_quad((xa, ya, 0.45), (xa, ya, -0.45),
                           (xb, yb, -0.45), (xb, yb, 0.45),
                           MAT_SHELL_GROOVE, n_tl=n_side_m, n_tr=n_side_m, n_br=n_side_m, n_bl=n_side_m)
                m.add_quad((xa, ya, -0.45), (xa, ya, Z_CHAMFER_RIM),
                           (xb, yb, Z_CHAMFER_RIM), (xb, yb, -0.45),
                           mat_s, n_tl=n_side_m, n_tr=n_side_m, n_br=n_side_m, n_bl=n_side_m)
                m.add_quad((xa, ya, Z_CHAMFER_RIM), (x_out_a, ya, zf_edge),
                           (x_out_b, yb, zf_edge), (xb, yb, Z_CHAMFER_RIM),
                           mat_s, n_tl=n_side_m, n_tr=n_side_f, n_br=n_side_f, n_bl=n_side_m)

    return m


def export_obj(mesh, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("# Nintendo 64 Game Pak 3D Model for DaedalusX3D\n")
        for x, y, z in mesh.verts:
            f.write(f"v {x:.4f} {-y:.4f} {-z:.4f}\n")
        for idx, (i0, i1, i2, n0, n1, n2, uv0, uv1, uv2, mat) in enumerate(mesh.tris):
            f.write(f"vt {uv0[0]:.4f} {1.0 - uv0[1]:.4f}\n")
            f.write(f"vt {uv1[0]:.4f} {1.0 - uv1[1]:.4f}\n")
            f.write(f"vt {uv2[0]:.4f} {1.0 - uv2[1]:.4f}\n")
            f.write(f"vn {n0[0]:.4f} {-n0[1]:.4f} {-n0[2]:.4f}\n")
            f.write(f"vn {n1[0]:.4f} {-n1[1]:.4f} {-n1[2]:.4f}\n")
            f.write(f"vn {n2[0]:.4f} {-n2[1]:.4f} {-n2[2]:.4f}\n")
        for idx, (i0, i1, i2, n0, n1, n2, uv0, uv1, uv2, mat) in enumerate(mesh.tris):
            base = idx * 3 + 1
            f.write(f"f {i0+1}/{base}/{base} {i1+1}/{base+1}/{base+1} {i2+1}/{base+2}/{base+2}\n")


def export_cpp_header(mesh, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write("// Auto-generated 3D Nintendo 64 Game Pak Cartridge Mesh (Blender-style pipeline)\n")
        f.write("// Do not edit manually; generated by generate_n64_models.py\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("namespace N64CartMesh {\n\n")
        f.write("enum ECartMat : uint8_t {\n")
        f.write("\tMAT_SHELL_MAIN  = 0,\n")
        f.write("\tMAT_SHELL_WING  = 1,\n")
        f.write("\tMAT_SHELL_BEVEL = 2,\n")
        f.write("\tMAT_SHELL_GROOVE= 3,\n")
        f.write("\tMAT_CARTOUCHE   = 4,\n")
        f.write("\tMAT_STICKER_RIM = 5,\n")
        f.write("\tMAT_PCB_CHAMBER = 6,\n")
        f.write("\tMAT_PCB_PINS    = 7,\n")
        f.write("\tMAT_STICKER_TEX = 8\n")
        f.write("};\n\n")
        f.write("struct CartTri {\n")
        f.write("\tfloat x0, y0, z0;\n")
        f.write("\tfloat x1, y1, z1;\n")
        f.write("\tfloat x2, y2, z2;\n")
        f.write("\tfloat nx0, ny0, nz0;\n")
        f.write("\tfloat nx1, ny1, nz1;\n")
        f.write("\tfloat nx2, ny2, nz2;\n")
        f.write("\tfloat u0, v0, u1, v1, u2, v2;\n")
        f.write("\tuint8_t mat;\n")
        f.write("};\n\n")
        f.write(f"static constexpr int kNumCartTris = {len(mesh.tris)};\n")
        f.write("static const CartTri kCartTris[kNumCartTris] = {\n")
        for i0, i1, i2, n0, n1, n2, uv0, uv1, uv2, mat in mesh.tris:
            p0 = mesh.verts[i0]
            p1 = mesh.verts[i1]
            p2 = mesh.verts[i2]
            f.write(
                f"\t{{ {p0[0]:.2f}f,{p0[1]:.2f}f,{p0[2]:.2f}f, "
                f"{p1[0]:.2f}f,{p1[1]:.2f}f,{p1[2]:.2f}f, "
                f"{p2[0]:.2f}f,{p2[1]:.2f}f,{p2[2]:.2f}f, "
                f"{n0[0]:.3f}f,{n0[1]:.3f}f,{n0[2]:.3f}f, "
                f"{n1[0]:.3f}f,{n1[1]:.3f}f,{n1[2]:.3f}f, "
                f"{n2[0]:.3f}f,{n2[1]:.3f}f,{n2[2]:.3f}f, "
                f"{uv0[0]:.3f}f,{uv0[1]:.3f}f, {uv1[0]:.3f}f,{uv1[1]:.3f}f, {uv2[0]:.3f}f,{uv2[1]:.3f}f, "
                f"{mat} }},\n"
            )
        f.write("};\n\n} // namespace N64CartMesh\n")


def render_preview(mesh, out_png):
    W, H = 640, 360
    buf = bytearray(W * H * 3)
    for y in range(H):
        row = y * W * 3
        for x in range(W):
            buf[row + x*3]     = 18
            buf[row + x*3 + 1] = 22
            buf[row + x*3 + 2] = 30
    zbuf = [1e9] * (W * H)

    base_colors = {
        MAT_SHELL_MAIN:   (150, 156, 168),
        MAT_SHELL_WING:   (134, 140, 152),
        MAT_SHELL_BEVEL:  (166, 172, 184),
        MAT_SHELL_GROOVE: (56,  62,  72),
        MAT_CARTOUCHE:    (92,  98,  110),
        MAT_STICKER_RIM:  (74,  80,  92),
        MAT_PCB_CHAMBER:  (22,  26,  34),
        MAT_PCB_PINS:     (224, 178, 52),
        MAT_STICKER_TEX:  (36,  118, 228),
    }

    def rot_yx(p, yaw, pitch):
        cy, sy = math.cos(yaw), math.sin(yaw)
        x1 = p[0] * cy + p[2] * sy
        z1 = -p[0] * sy + p[2] * cy
        cp, sp = math.cos(pitch), math.sin(pitch)
        y2 = p[1] * cp - z1 * sp
        z2 = p[1] * sp + z1 * cp
        return (x1, y2, z2)

    def proj(p, cx, cy, fov=460.0, dist=250.0):
        z = p[2] + dist
        s = fov / max(24.0, z)
        return (cx + p[0] * s, cy + p[1] * s, z)

    L_key = v_norm((-0.42, -0.58, -0.69))
    L_rim = v_norm((0.76, -0.25, 0.60))
    V_dir = (0.0, 0.0, -1.0)
    H_vec = v_norm(v_add(L_key, V_dir))

    def shade(n, mat, uv):
        if mat == MAT_STICKER_TEX:
            r_b = int(42 + 185 * uv[0])
            g_b = int(95 + 115 * (1.0 - uv[1]))
            b_b = int(230 - 125 * uv[0])
            base = (r_b, g_b, b_b)
        else:
            base = base_colors[mat]
        ndotl = max(0.0, v_dot(n, L_key))
        ndotr = max(0.0, v_dot(n, L_rim))
        ndoth = max(0.0, v_dot(n, H_vec))
        spec = (ndoth ** 16) * (0.45 if mat != MAT_SHELL_GROOVE else 0.05)
        diff = 0.36 + 0.58 * ndotl + 0.22 * ndotr
        r = min(255.0, base[0] * diff + 255.0 * spec)
        g = min(255.0, base[1] * diff + 248.0 * spec)
        b = min(255.0, base[2] * diff + 255.0 * spec)
        return (r, g, b)

    poses = [(-0.42, 0.16, 175, 185), (0.40, 0.15, 465, 185)]
    for yaw, pitch, cx, cy in poses:
        for i0, i1, i2, n0, n1, n2, uv0, uv1, uv2, mat in mesh.tris:
            p0 = rot_yx(mesh.verts[i0], yaw, pitch)
            p1 = rot_yx(mesh.verts[i1], yaw, pitch)
            p2 = rot_yx(mesh.verts[i2], yaw, pitch)
            s0 = proj(p0, cx, cy)
            s1 = proj(p1, cx, cy)
            s2 = proj(p2, cx, cy)
            area = (s1[0] - s0[0]) * (s2[1] - s0[1]) - (s1[1] - s0[1]) * (s2[0] - s0[0])
            if area <= 0.0:
                continue
            c0 = shade(v_norm(rot_yx(n0, yaw, pitch)), mat, uv0)
            c1 = shade(v_norm(rot_yx(n1, yaw, pitch)), mat, uv1)
            c2 = shade(v_norm(rot_yx(n2, yaw, pitch)), mat, uv2)

            min_x = max(0, int(math.floor(min(s0[0], s1[0], s2[0]))))
            max_x = min(W - 1, int(math.ceil(max(s0[0], s1[0], s2[0]))))
            min_y = max(0, int(math.floor(min(s0[1], s1[1], s2[1]))))
            max_y = min(H - 1, int(math.ceil(max(s0[1], s1[1], s2[1]))))
            inv_area = 1.0 / area
            for py in range(min_y, max_y + 1):
                fy = py + 0.5
                for px in range(min_x, max_x + 1):
                    fx = px + 0.5
                    w0 = ((s2[0] - s1[0]) * (fy - s1[1]) - (s2[1] - s1[1]) * (fx - s1[0])) * inv_area
                    w1 = ((s0[0] - s2[0]) * (fy - s2[1]) - (s0[1] - s2[1]) * (fx - s2[0])) * inv_area
                    w2 = 1.0 - w0 - w1
                    if w0 >= -1e-4 and w1 >= -1e-4 and w2 >= -1e-4:
                        z = w0 * s0[2] + w1 * s1[2] + w2 * s2[2]
                        idx = py * W + px
                        if z < zbuf[idx]:
                            zbuf[idx] = z
                            buf[idx*3]     = int(w0*c0[0] + w1*c1[0] + w2*c2[0])
                            buf[idx*3 + 1] = int(w0*c0[1] + w1*c1[1] + w2*c2[1])
                            buf[idx*3 + 2] = int(w0*c0[2] + w1*c1[2] + w2*c2[2])

    os.makedirs(os.path.dirname(out_png), exist_ok=True)
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        raw.extend(buf[y*W*3:(y+1)*W*3])
    def chunk(tag, data):
        return struct.pack("!I", len(data)) + tag + data + struct.pack("!I", zlib.crc32(tag + data) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack("!IIBBBBB", W, H, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    with open(out_png, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    mesh = build_n64_cartridge()
    print(f"Generated 3D N64 Cartridge Mesh: {len(mesh.verts)} vertices, {len(mesh.tris)} triangles")
    export_obj(mesh, r"c:\Users\santo\Downloads\DaedalusX3D\Source\SysCTR\Resources\n64_cartridge.obj")
    export_cpp_header(mesh, r"c:\Users\santo\Downloads\DaedalusX3D\Source\SysCTR\UI\N64CartridgeMesh.h")
    render_preview(mesh, r"C:\Users\santo\.gemini\antigravity\brain\99293830-759a-48a1-9c0c-013b820301e8\scratch\cartridge_3d_preview.png")
