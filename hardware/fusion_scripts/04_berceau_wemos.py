import adsk.core, adsk.fusion

# ===== Berceau Wemos D1 mini, cotes issues du F3D de la carte reelle (mm) =====
# PCB 25.0 x 34.2 x 1.0 ; USB dessous (-2.7) ; CH340 -1.0 ; bouton -2.0 ;
# module ESP dessus +3.3. Carte posee module vers le haut, USB vers -Y (paroi).
# La carte n'est portee que par ses zones nues : barre cote antenne + 2 plots
# aux coins USB. Centre ouvert. 2 levres fixes (USB) + 2 doigts a becs
# chanfreines (antenne) : insertion inclinee puis clic.
FLOOR = 2.4          # dessus du fond de la base
SEAT  = 5.8          # dessus des assises = dessous du PCB
PCBTOP = 6.8         # SEAT + 1.0
OPEN_X = 12.85       # demi-largeur d'ouverture (jeu 0.35/cote)
OPEN_Y1, OPEN_Y2 = -34.35, 0.35

def mm(v):
    return v * 0.1

def run(_context: str):
    app = adsk.core.Application.get()
    design = adsk.fusion.Design.cast(app.activeProduct)
    comp = design.rootComponent
    base = None
    for i in range(comp.bRepBodies.count):
        if comp.bRepBodies.item(i).name == "Base":
            base = comp.bRepBodies.item(i)

    JOIN = adsk.fusion.FeatureOperations.JoinFeatureOperation
    CUT  = adsk.fusion.FeatureOperations.CutFeatureOperation
    ext = comp.features.extrudeFeatures

    def rects(name, rect_list, op, z0, dz):
        sk = comp.sketches.add(comp.xYConstructionPlane)
        sk.name = name
        for (x1, y1, x2, y2) in rect_list:
            sk.sketchCurves.sketchLines.addTwoPointRectangle(
                adsk.core.Point3D.create(mm(x1), mm(y1), 0),
                adsk.core.Point3D.create(mm(x2), mm(y2), 0))
        coll = adsk.core.ObjectCollection.create()
        for i in range(sk.profiles.count):
            coll.add(sk.profiles.item(i))
        inp = ext.createInput(coll, op)
        inp.startExtent = adsk.fusion.OffsetStartDefinition.create(
            adsk.core.ValueInput.createByReal(mm(z0)))
        inp.setDistanceExtent(False, adsk.core.ValueInput.createByReal(mm(dz)))
        inp.participantBodies = [base]
        f = ext.add(inp)
        f.name = name
        return f

    # 1. Assise cote antenne (bord +Y, dessous nu) + mur d'appui
    rects("AssiseAntenne", [(-13.5, OPEN_Y2, 13.5, OPEN_Y2 + 1.8)], JOIN, FLOOR, SEAT - FLOOR)
    rects("AssiseAntenneLevre", [(-13.5, OPEN_Y2 - 1.2, 13.5, OPEN_Y2)], JOIN, FLOOR, SEAT - FLOOR)
    # 2. Plots d'assise cote USB (coins nus ; le milieu reste ouvert pour l'USB
    #    -2.7 et le bouton dessous en x[-10.7,-8.2] est evite)
    rects("PlotsUSB", [(11.0, OPEN_Y1, OPEN_X + 1.7, OPEN_Y1 + 2.6),
                       (-OPEN_X - 1.7, OPEN_Y1, -11.0, OPEN_Y1 + 2.6)], JOIN, FLOOR, SEAT - FLOOR)
    # 3. Parois de guidage laterales (pas d'appui sous la carte : pins libres)
    rects("GuidesLateraux", [(OPEN_X, OPEN_Y1, OPEN_X + 1.7, OPEN_Y2),
                             (-OPEN_X - 1.7, OPEN_Y1, -OPEN_X, OPEN_Y2)], JOIN, FLOOR, 9.0 - FLOOR)
    # 4. Levres fixes cote USB (au-dessus des coins du PCB)
    rects("LevresUSB", [(10.8, OPEN_Y1, OPEN_X + 1.7, OPEN_Y1 + 1.8),
                        (-OPEN_X - 1.7, OPEN_Y1, -10.8, OPEN_Y1 + 1.8)],
          JOIN, PCBTOP + 0.25, 9.0 - (PCBTOP + 0.25))
    # 5. Clips flexibles cote antenne : 2 doigts + bec 0.8 sur le PCB
    rects("ClipsDoigts", [(8.5, OPEN_Y2, 12.5, OPEN_Y2 + 1.2),
                          (-12.5, OPEN_Y2, -8.5, OPEN_Y2 + 1.2)], JOIN, FLOOR, 8.3 - FLOOR)
    rects("ClipsBecs", [(8.5, OPEN_Y2 - 0.8, 12.5, OPEN_Y2 + 0.3),
                        (-12.5, OPEN_Y2 - 0.8, -8.5, OPEN_Y2 + 0.3)],
          JOIN, PCBTOP + 0.25, 8.3 - (PCBTOP + 0.25))
    # 6. Fente USB : au niveau reel du connecteur (sous la carte)
    rects("FenteUSB2", [(-8.6, -42.0, 8.6, -33.0)], CUT, FLOOR, 9.5 - FLOOR)

    # 7. Chanfrein d'entree sous les becs (sinon la carte bute a plat)
    zt, yt = mm(PCBTOP + 0.25), mm(OPEN_Y2 - 0.8)
    edges = adsk.core.ObjectCollection.create()
    for e in base.edges:
        try:
            p1 = e.startVertex.geometry
            p2 = e.endVertex.geometry
        except Exception:
            continue
        if abs(p1.z - zt) < 1e-3 and abs(p2.z - zt) < 1e-3 and \
           abs(p1.y - yt) < 1e-3 and abs(p2.y - yt) < 1e-3 and \
           abs(p1.x) > 0.8 and abs(p1.x) < 1.3:
            edges.add(e)
    if edges.count:
        ch = comp.features.chamferFeatures
        ci = ch.createInput2()
        ci.chamferEdgeSets.addEqualDistanceChamferEdgeSet(
            edges, adsk.core.ValueInput.createByReal(0.06), False)
        ch.add(ci).name = "ChanfreinBecs"

    print("Berceau OK — vol base %.1f cm3" % base.volume)
