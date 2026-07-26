import adsk.core, adsk.fusion

# ===== Amincissement du diffuseur pour PLA blanc mat (mm) =====
# Paroi 2.0 -> 1.0 au-dessus de la jupe baionnette (qui reste a 2.0),
# chapeau : centre 0.4 (2 couches) sur O74, couronne 1.2 en peripherie.
NEW_IR   = 39.0    # nouveau rayon interieur du tube (ID 78, paroi 1.0)
Z_STEP   = 32.0    # fin de la jupe epaisse / debut de l'amincissement
Z_CAPCUT = 70.8    # dessous du chapeau une fois aminci (chapeau 1.2)
CAP_IR   = 37.0    # rayon du disque central aminci du chapeau (O74)
CAP_Z1, CAP_Z2 = 70.8, 71.6   # coupe -> centre du chapeau 72-71.6 = 0.4

def mm(v):
    return v * 0.1

def run(_context: str):
    app = adsk.core.Application.get()
    design = adsk.fusion.Design.cast(app.activeProduct)
    comp = design.rootComponent
    dif = None
    for i in range(comp.bRepBodies.count):
        if comp.bRepBodies.item(i).name == "Diffuseur":
            dif = comp.bRepBodies.item(i)

    CUT = adsk.fusion.FeatureOperations.CutFeatureOperation
    ext = comp.features.extrudeFeatures

    def cut_disc(name, r, z0, dz):
        sk = comp.sketches.add(comp.xYConstructionPlane)
        sk.name = "esq_" + name
        sk.sketchCurves.sketchCircles.addByCenterRadius(
            adsk.core.Point3D.create(0, 0, 0), mm(r))
        inp = ext.createInput(sk.profiles.item(0), CUT)
        inp.startExtent = adsk.fusion.OffsetStartDefinition.create(
            adsk.core.ValueInput.createByReal(mm(z0)))
        inp.setDistanceExtent(False, adsk.core.ValueInput.createByReal(mm(dz)))
        inp.participantBodies = [dif]
        ext.add(inp).name = name

    # 1. Paroi 1.0 au-dessus de la jupe + chapeau ramene a 1.2
    cut_disc("AmincissementDiffuseur", NEW_IR, Z_STEP, Z_CAPCUT - Z_STEP)

    # chanfrein 45 sur l'arete interne du redan (impression chapeau en bas)
    edges = adsk.core.ObjectCollection.create()
    for e in dif.edges:
        g = e.geometry
        if g.curveType == adsk.core.Curve3DTypes.Circle3DCurveType:
            if abs(g.center.z - mm(Z_STEP)) < 1e-4 and abs(g.radius - mm(NEW_IR)) < 1e-4:
                edges.add(e)
    if edges.count:
        ch = comp.features.chamferFeatures
        ci = ch.createInput2()
        ci.chamferEdgeSets.addEqualDistanceChamferEdgeSet(
            edges, adsk.core.ValueInput.createByReal(mm(0.8)), False)
        ch.add(ci).name = "ChanfreinRedan"

    # 2. Centre du chapeau a 0.4 (2 couches), couronne 1.2 conservee
    cut_disc("ChapeauFin", CAP_IR, CAP_Z1, CAP_Z2 - CAP_Z1)

    print("Diffuseur aminci — vol %.1f cm3" % dif.volume)
