import adsk.core, adsk.fusion

# ============ PARAMETRES (mm) ============
BASE_OD    = 80.0   # diametre exterieur base
BASE_H     = 24.0   # hauteur section principale
LIP_OD     = 75.6   # diametre de la levre (emboitement diffuseur)
LIP_H      = 6.0    # hauteur de la levre (z 24 -> 30)
CAV_D      = 72.0   # diametre cavite interieure
FLOOR_T    = 2.4    # epaisseur du fond
# Le support de la carte (berceau, fente USB) est dans 04_berceau_wemos.py

def mm(v):
    return v * 0.1  # API Fusion en cm

def P(x, y):
    return adsk.core.Point3D.create(mm(x), mm(y), 0)

def run(_context: str):
    app = adsk.core.Application.get()
    design = adsk.fusion.Design.cast(app.activeProduct)
    root = design.rootComponent

    comp = root
    sketches = comp.sketches
    extrudes = comp.features.extrudeFeatures
    NEW  = adsk.fusion.FeatureOperations.NewBodyFeatureOperation
    JOIN = adsk.fusion.FeatureOperations.JoinFeatureOperation
    CUT  = adsk.fusion.FeatureOperations.CutFeatureOperation

    def extrude(profiles, op, start_mm, dist_mm, name):
        inp = extrudes.createInput(profiles, op)
        inp.startExtent = adsk.fusion.OffsetStartDefinition.create(
            adsk.core.ValueInput.createByReal(mm(start_mm)))
        inp.setDistanceExtent(False, adsk.core.ValueInput.createByReal(mm(dist_mm)))
        f = extrudes.add(inp)
        f.name = name
        return f

    def circle_sketch(r_mm, name):
        sk = sketches.add(comp.xYConstructionPlane)
        sk.name = name
        sk.sketchCurves.sketchCircles.addByCenterRadius(P(0, 0), mm(r_mm))
        return sk.profiles.item(0)

    def rect_sketch(rects, name):
        sk = sketches.add(comp.xYConstructionPlane)
        sk.name = name
        for (x1, y1, x2, y2) in rects:
            sk.sketchCurves.sketchLines.addTwoPointRectangle(P(x1, y1), P(x2, y2))
        if sk.profiles.count == 1:
            return sk.profiles.item(0)
        coll = adsk.core.ObjectCollection.create()
        for i in range(sk.profiles.count):
            coll.add(sk.profiles.item(i))
        return coll

    # 1. Corps principal
    extrude(circle_sketch(BASE_OD / 2, "esq_corps"), NEW, 0, BASE_H, "Corps")
    # 2. Levre d'emboitement
    extrude(circle_sketch(LIP_OD / 2, "esq_levre"), JOIN, BASE_H, LIP_H, "Levre")
    # 3. Cavite interieure (laisse le fond)
    extrude(circle_sketch(CAV_D / 2, "esq_cavite"), CUT,
            BASE_H + LIP_H, -(BASE_H + LIP_H - FLOOR_T), "Cavite")
    body = comp.bRepBodies.item(0)
    body.name = "Base"
    bb = body.boundingBox
    print("Base OK — corps:", comp.bRepBodies.count)
    print("BBox mm: x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f]" % (
        bb.minPoint.x * 10, bb.maxPoint.x * 10,
        bb.minPoint.y * 10, bb.maxPoint.y * 10,
        bb.minPoint.z * 10, bb.maxPoint.z * 10))
    print("Volume cm3: %.1f" % body.volume)
