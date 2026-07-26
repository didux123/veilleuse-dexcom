# Veilleuse Dexcom — Boîtier 3D (Fusion 360)

Boîtier en deux pièces imprimées 3D pour la veilleuse ESP8266 + WS2812.
Modélisé via l'API Python de Fusion 360 (scripts dans `fusion_scripts/`,
exécutés en live par le serveur MCP Fusion, port 27182).

🖨️ Aussi publié sur **MakerWorld** avec profil d'impression Bambu Lab :
[makerworld.com/en/models/3097564-veilleuse-dexcom](https://makerworld.com/en/models/3097564-veilleuse-dexcom#profileId-3491186)

## Fichiers

| Fichier | Contenu |
|---|---|
| `veilleuse_base.stl` | Base opaque : logement Wemos D1 mini, taquets de retenue, baïonnette (imprimer en PLA **noir/opaque**) |
| `veilleuse_diffuseur.stl` | Capot diffuseur avec 3 ergots de verrouillage (imprimer en PLA/PETG **blanc naturel ou transparent**) |
| `veilleuse_bambulab.3mf` | Projet Bambu Studio prêt à trancher (les 2 pièces sur 2 plateaux) |
| `veilleuse_dexcom.f3d` | Archive Fusion 360 complète (timeline paramétrique) |
| `fusion_scripts/` | Scripts API qui reconstruisent tout le modèle (cotes en tête de fichier) |

## Architecture (Ø 80 mm × h 72 mm)

- **Base** (z 0 → 30 mm) : pot cylindrique Ø 80, paroi 4 mm, fond 2,4 mm.
  - **Lèvre baïonnette** Ø 75,6 × h 6 (renforcée à 2,8 mm de paroi) : 3 rainures
    en L à 120° — entrée verticale, canal horizontal avec plafond de verrouillage,
    bosse de cliquetage (0,15 mm), butée de fin de course.
  - **Berceau Wemos** (cotes mesurées sur le F3D du D1 mini réel, PCB
    25,0 × 34,2 × 1,0) : la carte n'est portée que par ses zones nues —
    barre d'assise sous le bord antenne + 2 plots aux coins USB. Tout le
    centre est **ouvert** (USB −2,7 mm, CH340, bouton sous la carte pendent
    librement). Parois de guidage latérales, pins/soudures dans le vide.
  - **Clips** : 2 lèvres fixes au-dessus des coins USB + 2 doigts flexibles
    à becs chanfreinés côté antenne → insertion inclinée puis clic.
  - **Fente USB** 17,2 × 7,1 mm à travers la paroi, au niveau réel du
    connecteur (sous la carte).
  - **Montage LED en anneau** : coller le ruban (IPIXEL S010060CA3SA1,
    SK6812 60 LED/m, largeur 10 mm) sur la **paroi intérieure** de la cavité
    (Ø 72, circonférence ≈ 226 mm), LEDs vers le centre. 8 LED = 133 mm
    (~60 % du tour) ; 12 LED = 200 mm (presque complet — mettre `NUM_LEDS 12`
    dans le firmware). 3 fils vers la carte : 5V → 5V, GND → G, DIN → **D1**
    (avec ~330 Ω en série ; condensateur 470-1000 µF entre 5V et GND).
- **Diffuseur** (z 24 → 72) : tube Ø 80 ext, **paroi 1,0 mm** (pour laisser
  passer la lumière en PLA blanc mat), congé r 2. **Chapeau : centre 0,4 mm
  (2 couches) sur Ø 74**, couronne périphérique 1,2 mm pour la jonction avec
  le mur. La **jupe d'emboîtement reste à 2,0 mm** (z 24 → 32) : elle porte
  les 3 ergots de la baïonnette et encaisse le couple de verrouillage.
  Transition en chanfrein 45°. ⚠️ Ne pas appuyer fort au centre du chapeau.

## Montage

- **Carte ESP** (module vers le haut, USB vers la paroi) :
  1. Incliner légèrement la carte, glisser les coins USB sous les 2 lèvres fixes.
  2. Appuyer sur le bord antenne : les 2 doigts s'écartent et **cliquent**
     par-dessus la carte.
  Démontage : écarter les 2 doigts avec l'ongle et soulever le bord antenne.
- **Diffuseur** (baïonnette) : aligner les 3 ergots sur les entrées de la lèvre,
  poser, puis **tourner d'environ 20° dans le sens horaire** (vu de dessus)
  jusqu'au clic. Le plafond des canaux verrouille le capot verticalement ;
  la bosse le retient en rotation. Déverrouillage : tourner en sens antihoraire.

## Impression

- Base : PLA opaque, 0,2 mm, 3 périmètres, infill 15 %, sans supports
  (fente USB = petit pont de 16 mm ; taquets = surplombs ≤ 1,5 mm — OK).
- Diffuseur : PLA blanc (mat OK vu la paroi de 1 mm), chapeau posé sur le
  plateau (ouverture vers le haut), **2 périmètres avec largeur de ligne
  0,5 mm** (= paroi pleine de 1,0, pas de gap fill), 0 % infill. Les ergots
  sont des surplombs horizontaux de 1,2 mm près de l'ouverture — sans support.
- Si la baïonnette est trop dure/molle : jouer sur `BUMP_R` (crête de la bosse)
  et `NUB_R1` (saillie de l'ergot) dans `03_bayonnette.py`, ± 0,1 mm.

## Rejouer / modifier le modèle

Toutes les cotes sont des constantes en tête de chaque script. Deux façons :

1. **MCP Fusion** (Préférences ▸ Général ▸ API ▸ Fusion MCP Server activé) :
   envoyer le contenu du script à l'outil `fusion_mcp_execute`.
2. **Scripts and Add-Ins** dans Fusion : coller le contenu dans un nouveau
   script Python (le point d'entrée `run(_context)` est déjà conforme).

Ordre : `01_base` → `02_diffuseur` → `03_bayonnette` → `04_taquets_esp` →
`05_apparences` → `06_export`, sur un document pièce vierge (unités mm).
