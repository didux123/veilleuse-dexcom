import adsk.core, adsk.fusion

OUT = "/Users/maxence/Documents/PYTHON/DEXCOM_LAMP/DEXCOM_API/hardware"

def run(_context: str):
    app = adsk.core.Application.get()
    design = adsk.fusion.Design.cast(app.activeProduct)
    root = design.rootComponent

    for i in range(root.bRepBodies.count):
        b = root.bRepBodies.item(i)
        b.isVisible = True

    em = design.exportManager
    for i in range(root.bRepBodies.count):
        b = root.bRepBodies.item(i)
        opts = em.createSTLExportOptions(b, OUT + "/veilleuse_" + b.name.lower() + ".stl")
        opts.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementHigh
        em.execute(opts)
        print("STL exporte:", b.name)

    arch = em.createFusionArchiveExportOptions(OUT + "/veilleuse_dexcom.f3d")
    em.execute(arch)
    print("Archive F3D exportee")

    app.activeViewport.fit()
