"""Five armed goblin anatomies, authored in metres on the creature lattice.

Facing +X, Z up. Overlapping continuous ellipsoids and tapered limbs retain
joint volume; equipment is held in actual hands, and every occupied voxel has
an append-only anatomical part ID. No pruning or post-hoc connectivity repair.
"""
from __future__ import annotations
import numpy as np
from .grid import VoxelGrid
from . import materials, parts
from .spec import get

ROLES = ("raider", "spearhunter", "stalker", "hexer", "brute")


def build(spec, rng, voxel_m, out=None):
    role = get(spec, "goblin.role")
    if role not in ROLES:
        raise ValueError(f"Unknown goblin archetype: {role}")
    h = float(get(spec, "goblin.height_m"))
    bulk = float(get(spec, "goblin.bulk"))
    # Geometry below uses adult height as its unit; weapons can extend above it.
    origin = np.floor(np.array([-.50, -.70, -.03]) * h / voxel_m).astype(int)
    shape = np.ceil(np.array([1.30, 1.40, 1.55]) * h / voxel_m).astype(int)
    grid = VoxelGrid(tuple(shape), origin, voxel_m)
    tags = np.zeros(grid.shape, np.uint8)
    skin = materials.resolve(get(spec, "goblin.skin"))
    cloth = materials.resolve(get(spec, "goblin.cloth"))
    dark = materials.resolve("skin_dark")
    leather = materials.resolve("skin_brown")
    iron = materials.resolve("beak_horn")
    edge = materials.resolve("skin_silver")
    bone = materials.resolve("skin_pale")
    eye = materials.resolve("skin_yellow")
    wood = materials.resolve("deadwood")

    def coords(lo, hi):
        lo = np.maximum(np.floor(np.array(lo)*h/voxel_m-origin).astype(int)-1, 0)
        hi = np.minimum(np.ceil(np.array(hi)*h/voxel_m-origin).astype(int)+2, shape)
        if np.any(hi <= lo):
            return None, None
        sl = tuple(slice(a, b) for a,b in zip(lo,hi))
        xyz = np.ogrid[tuple(slice(int(a),int(b)) for a,b in zip(lo,hi))]
        return sl, [(v+origin[i]+.5)*voxel_m/h for i,v in enumerate(xyz)]

    def ell(c, r, mat=skin, pid=parts.P_BODY):
        c=np.array(c);r=np.array(r)
        sl,v=coords(c-r,c+r)
        if sl is None: return
        mask=sum(((v[i]-c[i])/r[i])**2 for i in range(3))<=1
        grid.data[sl][mask]=mat; tags[sl][mask]=pid if mat else 0

    def rod(a,b,ra,rb=None,mat=skin,pid=parts.P_BODY):
        a=np.array(a);b=np.array(b);rb=ra if rb is None else rb
        radius=max(ra,rb,voxel_m/h)
        sl,v=coords(np.minimum(a,b)-radius,np.maximum(a,b)+radius)
        if sl is None:return
        d=b-a; t=np.clip(sum((v[i]-a[i])*d[i] for i in range(3))/max(float(d@d),1e-9),0,1)
        rr=np.maximum(ra+(rb-ra)*t, .88*voxel_m/h)
        mask=sum((v[i]-a[i]-d[i]*t)**2 for i in range(3))<=rr**2
        grid.data[sl][mask]=mat;tags[sl][mask]=pid

    # Pelvis, tapered abdomen, rib cage and separate shoulder/trapezius masses.
    ell((-.025,0,.46),(.095,.125*bulk,.105))
    ell((-.012,0,.59),(.080,.105*bulk,.15))
    ell((-.035,0,.705),(.105,.158*bulk,.135))
    for side in (-1,1):
        ell((.020,side*.072*bulk,.715),(.088,.087*bulk,.078))
        rod((-.055,side*.14*bulk,.76),(.010,side*.045,.825),.065,.047)
    rod((-.015,0,.765),(.046,0,.852),.068,.065,pid=parts.P_NECK)
    # Broad cheekbones, low prognathic face, hooked nose and projecting chin.
    ell((.062,0,.889),(.093,.092,.108),pid=parts.P_HEAD)
    ell((.098,0,.843),(.093,.079,.052),pid=parts.P_JAW)
    ell((.155,0,.867),(.050,.044,.043),pid=parts.P_JAW)
    ell((.148,0,.903),(.041,.025,.048),pid=parts.P_HEAD)
    ell((.181,0,.876),(.030,.029,.022),pid=parts.P_HEAD)
    for side in (-1,1):
        earid=parts.P_EAR+(parts.SIDE_STRIDE if side>0 else 0)
        # Swept pointed ears, thick root and tapered blade, with inner cartilage.
        rod((.037,side*.075,.897),(-.027,side*.206,.957),.040,.004,pid=earid)
        rod((.051,side*.104,.909),(-.018,side*.182,.947),.016,.003,mat=leather,pid=earid)
        ell((.123,side*.071,.868),(.045,.034,.039),pid=parts.P_HEAD)
        # Eye socket faces front and sideways, iris and pupil remain visible.
        ell((.139,side*.058,.908),(.024,.024,.018),dark,parts.P_HEAD)
        ell((.154,side*.063,.909),(.014,.012,.010),eye,parts.P_HEAD)
        ell((.164,side*.063,.909),(.006,.005,.008),dark,parts.P_HEAD)
        rod((.154,side*.032,.925),(.121,side*.087,.936),.014,.019,pid=parts.P_HEAD)
        ell((.196,side*.014,.870),(.009,.009,.006),dark,parts.P_HEAD)
    # Incised mouth framed by lips; two small upward tusks.
    rod((.171,-.048,.843),(.184,0,.839),.006,mat=dark,pid=parts.P_JAW)
    rod((.184,0,.839),(.171,.048,.843),.006,mat=dark,pid=parts.P_JAW)
    for side in (-1,1):
        rod((.175,side*.039,.830),(.194,side*.035,.862),.011,.003,mat=bone,pid=parts.P_JAW)

    # Bowed thighs, bony knees, long tibiae and splayed three-toed bare feet.
    hands={}
    for side in (-1,1):
        stride=parts.SIDE_STRIDE if side>0 else 0
        hip=(-.020,side*.080*bulk,.45)
        knee=(.037,side*.122*bulk,.255)
        ankle=(-.010,side*.132*bulk,.080)
        rod(hip,knee,.063*bulk,.040,pid=parts.P_LEG+stride)
        ell(knee,(.042,.041,.042),pid=parts.P_LEG+stride)
        rod(knee,ankle,.036,.027,pid=parts.P_SHIN+stride)
        rod((.024,side*.13*bulk,.08),(.089,side*.137*bulk,.038),.038,.040,pid=parts.P_FOOT+stride)
        for toe in (-1,0,1):
            rod((.070,side*.137*bulk+toe*.024,.036),(.144-abs(toe)*.013,side*.137*bulk+toe*.026,.030),.015,.012,pid=parts.P_FOOT+stride)
        # Asymmetric arms in a relaxed fighting guard; hands wrap the weapon handles.
        shoulder=(-.035,side*.167*bulk,.742)
        elbow=(.015,side*.222*bulk,.570)
        hand=(.142,side*.245*bulk,.488 if side<0 else .54)
        if role in ("spearhunter","hexer") and side<0:
            hand=(.16,side*.245*bulk,.57)
        hands[side]=hand
        rod(shoulder,elbow,.056*bulk,.033,pid=parts.P_ARM+stride)
        ell(shoulder,(.060,.068*bulk,.068),pid=parts.P_ARM+stride)
        rod(elbow,hand,.040*bulk,.026,pid=parts.P_FOREARM+stride)
        ell(hand,(.043,.032,.043),pid=parts.P_HAND+stride)
        for finger in range(3):
            rod((hand[0]+.024,hand[1]+(finger-1)*.018,hand[2]+.01),
                (hand[0]+.029,hand[1]+(finger-1)*.018,hand[2]-.029),.009,pid=parts.P_HAND+stride)
        # Shin bindings and forearm bracers follow their anatomical part IDs.
        for z in (.13,.155,.18):
            t=(z-.08)/.175
            ell((-.01+.047*t,side*(.132-.010*t)*bulk,z),(.034,.032,.012),leather,parts.P_SHIN+stride)
        rod(np.array(elbow)*.35+np.array(hand)*.65,np.array(elbow)*.18+np.array(hand)*.82,.037,.032,mat=leather,pid=parts.P_FOREARM+stride)
    # Belt, buckle, layered loincloth panels. Cloth is attached at the hips.
    ell((-.016,0,.486),(.107,.139*bulk,.026),leather)
    ell((.092,0,.486),(.019,.026,.027),iron)
    for side in (-1,0,1):
        rod((.069,side*.063*bulk,.468),(.085,side*.067*bulk,.348+abs(side)*.018),.034,.023,mat=cloth)
    # A diagonal leather harness crosses the chest, with metal studs.
    rod((.050,-.12*bulk,.78),(.084,.09*bulk,.51),.019,mat=leather)
    for t in (.15,.45,.75):
        ell((.05+.034*t,(-.12+.21*t)*bulk,.78-.27*t),(.020,.010,.010),iron)

    def weapon(side):return parts.P_WEAPON+(parts.SIDE_STRIDE if side>0 else 0)
    a=np.array(hands[-1]); b=np.array(hands[1])
    if role=="raider":
        # Broad iron axe and battered oval shield.
        rod(a-[0,0,.18],a+[0,0,.26],.016,mat=wood,pid=weapon(-1))
        ell(a+[.036,0,.205],(.10,.021,.068),iron,weapon(-1))
        ell(a+[.097,0,.205],(.020,.023,.075),edge,weapon(-1))
        ell(b+[.044,0,0],(.035,.125,.175),wood,weapon(1))
        ell(b+[.073,0,0],(.018,.128,.178),iron,weapon(1))
        ell(b+[.092,0,0],(.041,.043,.043),edge,weapon(1))
        # One overlapping shoulder plate gives an unmistakable armored silhouette.
        ell((-.02,-.17*bulk,.775),(.077,.091,.052),iron,parts.P_ARM)
    elif role=="spearhunter":
        rod(a-[0,0,.50],a+[.035,0,.57],.014,mat=wood,pid=weapon(-1))
        rod(a+[.035,0,.54],a+[.045,0,.76],.034,.002,mat=edge,pid=weapon(-1))
        # Back quiver and visible fletched darts remain attached to the torso.
        rod((-.12,.075,.52),(-.16,.10,.78),.043,mat=leather)
        for i in range(3):
            rod((-.155+i*.023,.09,.65),(-.19+i*.023,.11,.91),.006,mat=wood)
            ell((-.182+i*.023,.109,.87),(.013,.012,.036),bone)
    elif role=="stalker":
        for side,hand in hands.items():
            hand=np.array(hand)
            rod(hand-[0,0,.07],hand+[0,0,.045],.015,mat=leather,pid=weapon(side))
            rod(hand+[.014,0,.038],hand+[.10,0,.23],.026,.002,mat=edge,pid=weapon(side))
        # Close cowl at rear leaves the face open.
        ell((-.023,0,.917),(.060,.090,.098),cloth,parts.P_HEAD)
        rod((-.10,0,.77),(-.13,0,.56),.068,.043,mat=cloth)
    elif role=="hexer":
        rod(a-[0,0,.52],a+[.015,0,.54],.019,.014,mat=wood,pid=weapon(-1))
        for side in (-1,1):
            rod(a+[.015,0,.48],a+[.035,side*.073,.63],.017,.007,mat=bone,pid=weapon(-1))
        ell(a+[.023,0,.548],(.035,.037,.05),materials.resolve("skin_blue"),weapon(-1))
        # Mantle plus ragged robe panels, with space between the legs.
        for side in (-1,1):
            rod((-.035,side*.12,.72),(-.06,side*.125,.33),.068,.044,mat=cloth)
            for i in range(3):
                ell((.073,side*(.035+i*.035),.735-i*.013),(.017,.015,.023),bone)
        rod((-.02,0,.961),(-.063,0,1.13),.066,.006,mat=cloth,pid=parts.P_HEAD)
    elif role=="brute":
        # Heavy spiked war club; offhand knuckle armor.
        rod(a-[0,0,.16],a+[.018,0,.35],.025,.037,mat=wood,pid=weapon(-1))
        ell(a+[.017,0,.28],(.076,.068,.15),iron,weapon(-1))
        for z in (.22,.31,.38):
            for side in (-1,1):
                rod(a+[.017,side*.045,z],a+[.040,side*.113,z+.018],.017,.003,mat=bone,pid=weapon(-1))
        ell(b+[.03,0,0],(.035,.047,.044),iron,parts.P_HAND+parts.SIDE_STRIDE)
        for side in (-1,1):
            stride=parts.SIDE_STRIDE if side>0 else 0
            ell((-.033,side*.175*bulk,.789),(.080,.104,.059),iron,parts.P_ARM+stride)
            for i in range(3):
                rod((-.06+i*.037,side*.18*bulk,.817),(-.07+i*.04,side*.23*bulk,.907),.019,.002,mat=bone,pid=parts.P_ARM+stride)
    # Surface scars stay on the face and never introduce disconnected geometry.
    if role in ("brute","raider"):
        rod((.133,.080,.88),(.133,.074,.92),.004,mat=materials.resolve("skin_red"),pid=parts.P_HEAD)
    if out is not None:
        out["tags"] = tags
    return grid
