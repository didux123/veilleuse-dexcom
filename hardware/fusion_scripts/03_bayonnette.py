import adsk.core, adsk.fusion
import math

# ============ PARAMETRES (mm / degres) ============
# Renfort interieur de la levre (la paroi passe de 1,8 a 2,8)
RING_R1, RING_R2 = 35.0, 36.0
RING_Z1, RING_Z2 = 24.0, 30.0
# Rainures baionnette (creusees dans la face ext. de la levre, r 37,8)
GRV_RIN  = 36.6      # fond de rainure (profondeur 1,2)
GRV_ROUT = 37.9      # depasse la face ext. pour couper net
ENTRY_Z1, ENTRY_Z2 = 25.5, 30.5   # entree verticale (ouverte en haut)
CHAN_Z1,  CHAN_Z2  = 25.5, 28.0   # canal horizontal (plafond = verrou)
ENTRY_ANG = 9.0      # largeur angulaire de l'entree (deg, sens horaire)
CHAN_ANG  = 28.0     # longueur du canal
BUMP_START, BUMP_ANG = 18.0, 3.0  # bosse de cliquetage
BUMP_R = 36.95       # crete de la bosse (0,15 d'interference avec l'ergot)
# Ergots du diffuseur
NUB_R1, NUB_R2 = 36.8, 38.5       # du bout de l'ergot jusque dans la paroi
NUB_HALF_W = 2.0                  # demi-largeur tangentielle (largeur 4)
NUB_ANG = -4.5                    # centre sur l'entree
NUB_Z1, NUB_H = 25.75, 2.0

def mm(v):
    return v * 0.1

def run(_context: str):
    app = adsk.core.Application.get()
    design = adsk.fusion.Design.cast(app.activeProduct)
    comp = design.rootComponent
    JOIN = adsk.fusion.FeatureOperations.JoinFeatureOperation
    CUT  = adsk.fusion.FeatureOperations.CutFeatureOperation

    bodies = {comp.bRepBodies.item(i).name: comp.bRepBodies.item(i)
              for i in range(comp.bRepBodies.count)}
    base, dif = bodies["Base"], bodies["Diffuseur"]

    # --- 1. Renfort de levre ---
    sk = comp.sketches.add(comp.xYConstructionPlane)
    sk.name = "esq_renfort_levre"
    c = adsk.core.Point3D.create(0, 0, 0)
    sk.sketchCurves.sketchCircles.addByCenterRadius(c, mm(RING_R1))
    sk.sketchCurves.sketchCircles.addByCenterRadius(c, mm(RING_R2))
    ring = None
    for i in range(sk.profiles.count):
        if sk.profiles.item(i).profileLoops.count == 2:
            ring = sk.profiles.item(i)
    ext = comp.features.extrudeFeatures
    inp = ext.createInput(ring, JOIN)
    inp.startExtent = adsk.fusion.OffsetStartDefinition.create(
        adsk.core.ValueInput.createByReal(mm(RING_Z1)))
    inp.setDistanceExtent(False, adsk.core.ValueInput.createByReal(mm(RING_Z2 - RING_Z1)))
    inp.participantBodies = [base]
    ext.add(inp).name = "RenfortLevre"

    # --- helpers revolution ---
    revs = comp.features.revolveFeatures
    z_axis = comp.zConstructionAxis

    def radial_rect_profile(name, r1, r2, z1, z2, plane=None):
        """Rectangle dans le plan XZ (ou un plan incline), via conversion modele->esquisse."""
        skp = comp.sketches.add(plane if plane else comp.xZConstructionPlane)
        skp.name = name
        pts_model = [(r1, z1), (r2, z1), (r2, z2), (r1, z2)]
        sk_pts = []
        for (r, z) in pts_model:
            p = adsk.core.Point3D.create(mm(r), 0, mm(z)) if plane is None else None
            if plane is None:
                sk_pts.append(skp.modelToSketchSpace(p))
        lines = skp.sketchCurves.sketchLines
        for i in range(4):
            lines.addByTwoPoints(sk_pts[i], sk_pts[(i + 1) % 4])
        return skp.profiles.item(0)

    def revolve_cut(profile, angle_deg, name, participants, op=CUT):
        ri = revs.createInput(profile, z_axis, op)
        ri.setAngleExtent(False, adsk.core.ValueInput.createByReal(math.radians(angle_deg)))
        ri.participantBodies = participants
        f = revs.add(ri)
        f.name = name
        return f

    def angled_plane(angle_deg, name):
        pi = comp.constructionPlanes.createInput()
        pi.setByAngle(z_axis, adsk.core.ValueInput.createByReal(math.radians(angle_deg)),
                      comp.xZConstructionPlane)
        pl = comp.constructionPlanes.add(pi)
        pl.name = name
        return pl

    pattern_feats = []

    # --- 2. Entree verticale [0 .. -9deg] ---
    p_entry = radial_rect_profile("esq_bayo_entree", GRV_RIN, GRV_ROUT, ENTRY_Z1, ENTRY_Z2)
    pattern_feats.append(revolve_cut(p_entry, -ENTRY_ANG, "BayoEntree", [base]))

    # --- 3. Canal horizontal [0 .. -28deg] ---
    p_chan = radial_rect_profile("esq_bayo_canal", GRV_RIN, GRV_ROUT, CHAN_Z1, CHAN_Z2)
    pattern_feats.append(revolve_cut(p_chan, -CHAN_ANG, "BayoCanal", [base]))

    # --- 4. Bosse de cliquetage [-18 .. -21deg] (rajout de matiere au fond du canal) ---
    pl_bump = angled_plane(-BUMP_START, "plan_bosse")
    sk_b = comp.sketches.add(pl_bump)
    sk_b.name = "esq_bayo_bosse"
    corners = [(GRV_RIN, CHAN_Z1), (BUMP_R, CHAN_Z1), (BUMP_R, CHAN_Z2), (GRV_RIN, CHAN_Z2)]
    a = math.radians(-BUMP_START)
    sk_pts = []
    for (r, z) in corners:
        pm = adsk.core.Point3D.create(mm(r) * math.cos(a), mm(r) * math.sin(a), mm(z))
        sk_pts.append(sk_b.modelToSketchSpace(pm))
    for i in range(4):
        sk_b.sketchCurves.sketchLines.addByTwoPoints(sk_pts[i], sk_pts[(i + 1) % 4])
    pattern_feats.append(revolve_cut(sk_b.profiles.item(0), -BUMP_ANG, "BayoBosse",
                                     [base], op=JOIN))

    # --- 5. Ergot du diffuseur (centre sur l'entree, -4,5deg) ---
    sk_n = comp.sketches.add(comp.xYConstructionPlane)
    sk_n.name = "esq_ergot"
    an = math.radians(NUB_ANG)
    u = (math.cos(an), math.sin(an))
    v = (-math.sin(an), math.cos(an))
    pts = []
    for (r, t) in [(NUB_R1, -NUB_HALF_W), (NUB_R2, -NUB_HALF_W),
                   (NUB_R2, NUB_HALF_W), (NUB_R1, NUB_HALF_W)]:
        pts.append(adsk.core.Point3D.create(mm(r * u[0] + t * v[0]),
                                            mm(r * u[1] + t * v[1]), 0))
    for i in range(4):
        sk_n.sketchCurves.sketchLines.addByTwoPoints(pts[i], pts[(i + 1) % 4])
    inp = ext.createInput(sk_n.profiles.item(0), JOIN)
    inp.startExtent = adsk.fusion.OffsetStartDefinition.create(
        adsk.core.ValueInput.createByReal(mm(NUB_Z1)))
    inp.setDistanceExtent(False, adsk.core.ValueInput.createByReal(mm(NUB_H)))
    inp.participantBodies = [dif]
    f_nub = ext.add(inp)
    f_nub.name = "Ergot"
    pattern_feats.append(f_nub)

    # --- 6. Repetition circulaire x3 (120 deg) ---
    circ = comp.features.circularPatternFeatures
    coll = adsk.core.ObjectCollection.create()
    for f in pattern_feats:
        coll.add(f)
    ci = circ.createInput(coll, z_axis)
    ci.quantity = adsk.core.ValueInput.createByReal(3)
    ci.totalAngle = adsk.core.ValueInput.createByReal(math.radians(360))
    ci.isSymmetric = False
    circ.add(ci).name = "Bayonnette_x3"

    print("Baionnette OK")
    for n in ("Base", "Diffuseur"):
        b = bodies[n]
        print("- %s : vol %.1f cm3, faces %d" % (n, b.volume, b.faces.count))
