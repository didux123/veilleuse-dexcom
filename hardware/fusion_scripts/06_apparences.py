import adsk.core, adsk.fusion

def run(_context: str):
    app = adsk.core.Application.get()
    design = adsk.fusion.Design.cast(app.activeProduct)
    root = design.rootComponent

    lib = None
    for i in range(app.materialLibraries.count):
        ml = app.materialLibraries.item(i)
        if "apparences" in ml.name.lower():
            lib = ml
    print("Bibliotheque:", lib.name if lib else "introuvable")

    def find_appearance(keywords):
        for i in range(lib.appearances.count):
            a = lib.appearances.item(i)
            n = a.name.lower()
            if all(k in n for k in keywords):
                return a
        return None

    dark = find_appearance(["plastique", "mat", "noir"]) or \
           find_appearance(["plastic", "matte", "black"]) or \
           find_appearance(["noir"]) or find_appearance(["black"])
    trans = find_appearance(["translucide", "blanc"]) or \
            find_appearance(["translucent", "white"]) or \
            find_appearance(["translucide"]) or find_appearance(["translucent"])
    print("Base ->", dark.name if dark else "?")
    print("Diffuseur ->", trans.name if trans else "?")

    bodies = {root.bRepBodies.item(i).name: root.bRepBodies.item(i)
              for i in range(root.bRepBodies.count)}
    if dark:
        bodies["Base"].appearance = design.appearances.addByCopy(dark, dark.name)
    if trans:
        bodies["Diffuseur"].appearance = design.appearances.addByCopy(trans, trans.name)

    app.activeViewport.fit()
    print("Apparences appliquees")
