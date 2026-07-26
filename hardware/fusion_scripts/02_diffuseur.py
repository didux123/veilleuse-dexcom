import adsk.core, adsk.fusion

# ============ PARAMETRES (mm) ============
DIF_OD   = 80.0    # diametre exterieur (affleure la base)
DIF_ID   = 76.0    # diametre interieur (glisse sur la levre OD 75.6)
DIF_Z0   = 24.0    # repose sur l'epaulement de la base
TUBE_H   = 46.0    # hauteur du tube (24 -> 70)
CAP_T    = 2.0     # epaisseur du chapeau (70 -> 72)

def mm(v):
    return v * 0.1

def P(x, y):
    return adsk.core.Point3D.create(mm(x), mm(y), 0)

def run(_context: str):
    app = adsk.core.Application.get()
    design = adsk.fusion.Design.cast(app.activeProduct)
    comp = design.rootComponent
    sketches = comp.sketches
    extrudes = comp.features.extrudeFeatures
    NEW  = adsk.fusion.FeatureOperations.NewBodyFeatureOperation
    JOIN = adsk.fusion.FeatureOperations.JoinFeatureOperation

    def extrude(profile, op, start_mm, dist_mm, name, participants=None):
        inp = extrudes.createInput(profile, op)
        inp.startExtent = adsk.fusion.OffsetStartDefinition.create(
            adsk.core.ValueInput.createByReal(mm(start_mm)))
        inp.setDistanceExtent(False, adsk.core.ValueInput.createByReal(mm(dist_mm)))
        if participants is not None:
            inp.participantBodies = participants
        f = extrudes.add(inp)
        f.name = name
        return f

    # Tube : anneau OD/ID
    sk = sketches.add(comp.xYConstructionPlane)
    sk.name = "esq_diffuseur_tube"
    sk.sketchCurves.sketchCircles.addByCenterRadius(P(0, 0), mm(DIF_OD / 2))
    sk.sketchCurves.sketchCircles.addByCenterRadius(P(0, 0), mm(DIF_ID / 2))
    ring = None
    for i in range(sk.profiles.count):
        if sk.profiles.item(i).profileLoops.count == 2:
            ring = sk.profiles.item(i)
    f1 = extrude(ring, NEW, DIF_Z0, TUBE_H, "DiffuseurTube")
    dif_body = f1.bodies.item(0)

    # Chapeau plein (join uniquement sur le corps diffuseur)
    sk2 = sketches.add(comp.xYConstructionPlane)
    sk2.name = "esq_diffuseur_chapeau"
    sk2.sketchCurves.sketchCircles.addByCenterRadius(P(0, 0), mm(DIF_OD / 2))
    extrude(sk2.profiles.item(0), JOIN, DIF_Z0 + TUBE_H, CAP_T,
            "DiffuseurChapeau", participants=[dif_body])

    dif_body.name = "Diffuseur"

    # Conge sur l'arete superieure exterieure
    top_z = mm(DIF_Z0 + TUBE_H + CAP_T)
    edges = adsk.core.ObjectCollection.create()
    for e in dif_body.edges:
        bb = e.boundingBox
        if abs(bb.minPoint.z - top_z) < 1e-4 and abs(bb.maxPoint.z - top_z) < 1e-4:
            g = e.geometry
            if g.curveType == adsk.core.Curve3DTypes.Circle3DCurveType and \
               abs(g.radius - mm(DIF_OD / 2)) < 1e-4:
                edges.add(e)
    if edges.count > 0:
        fils = comp.features.filletFeatures
        fi = fils.createInput()
        fi.edgeSetInputs.addConstantRadiusEdgeSet(
            edges, adsk.core.ValueInput.createByReal(mm(2.0)), True)
        fils.add(fi).name = "CongeChapeau"
        print("Conge applique sur", edges.count, "arete(s)")

    print("Corps dans le design:", comp.bRepBodies.count)
    for i in range(comp.bRepBodies.count):
        b = comp.bRepBodies.item(i)
        bb = b.boundingBox
        print("- %s : z[%.1f, %.1f] mm, vol %.1f cm3" % (
            b.name, bb.minPoint.z * 10, bb.maxPoint.z * 10, b.volume))
