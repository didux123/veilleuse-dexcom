# Landing page « Halo » + direction artistique

Site vitrine de la veilleuse, issu d'une maquette Claude Design. Ce dossier
contient du HTML statique : pas de build ni de dépendance, et aucune requête
externe à part Google Fonts (Space Grotesk).

| Fichier | Rôle |
|---|---|
| `index.html` | La landing page : hero, démo interactive des 8 états, comment ça marche, l'objet, FAQ, précommande. |
| `direction-artistique.html` | La DA : les 8 états lumineux, tokens, kit de composants, page de config, piste vitrine. Marquée `noindex`. |
| `img/` | Photo de mise en scène, recompressée en WebP et JPEG (900 et 1600 px), plus l'image Open Graph. |
| `design-source/` | Les exports Claude Design d'origine (`.dc.html` + `support.js` + PNG pleine résolution), conservés pour pouvoir rouvrir et retoucher la maquette. |

## Voir en local

```bash
cd landing && python3 -m http.server 8000
```

Puis ouvrir http://localhost:8000.

## Différences avec la maquette d'origine

Les `.dc.html` ont besoin du runtime Claude Design (`support.js`), qui charge
React et Babel depuis unpkg au moment de l'affichage. Les pages publiées ici en
sont des conversions statiques :

- `style-hover` est remplacé par des règles CSS `:hover` ;
- la démo des 8 états reprend tel quel le script d'origine, branché sur une
  vingtaine de lignes de JS vanilla au lieu de React ;
- ajouts : adaptation au téléphone (une colonne, navigation masquée sous
  760 px), focus clavier visible, meta description, Open Graph et favicon ;
- la photo de 4,4 Mo passe à environ 30 Ko en WebP.

Si la maquette est retouchée dans Claude Design, il faut refaire la conversion
plutôt que modifier `index.html` à la main.

## À trancher avant publication

La maquette présente un produit commercial (« Halo »). Plusieurs points ne
correspondent pas encore à la réalité :

- les boutons « Précommander » renvoient vers `#commander`, c'est-à-dire vers
  la section où ils se trouvent : il n'existe aucun tunnel de commande ;
- promesses commerciales : garantie 2 ans, expédition sous 15 jours, fabrication
  en France, test d'une nuit avant envoi ;
- « alimentation USB-C » : la Wemos D1 mini est en micro-USB ;
- « moins de 1 W » : chiffre non mesuré.

Ce qui est conforme au firmware : nuit de 22 h à 7 h à 22 %, relevé toutes les
60 s, hypo à pleine puissance qui ignore le mode nuit, gyrophare au-dessus de
300 mg/dL, rythmes des animations (1,2 s, 3 s, 600 ms), Ø 80 mm.
