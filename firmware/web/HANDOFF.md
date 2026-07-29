# Veilleuse Dexcom — handoff web embarqué (ESP8266)

Fichier à intégrer : `veilleuse-config.html` — 15,5 Ko brut, ~4,4 Ko gzip.
Un seul fichier, tout inline, zéro requête externe. Sert le portail captif ET
la page locale `veilleuse-xxxxxx.local` — même document, l'état vient de l'API.

## À faire côté firmware

1. **Stocker la page** en PROGMEM (`static const char INDEX_HTML[] PROGMEM = R"rawliteral(...)rawliteral";`).
   Attention : la page contient des `)` mais pas la séquence `)rawliteral"`, elle passe telle quelle.
2. **Servir en gzip** de préférence : gzipper le fichier, l'inclure en tableau d'octets,
   répondre avec `Content-Encoding: gzip` + `Content-Type: text/html`. Gain ~11 Ko de flash.
3. **Portail captif** : DNS wildcard vers l'AP + répondre `200` avec la page sur
   `/generate_204`, `/hotspot-detect.html`, `/ncsi.txt`, `/redirect`, et `onNotFound`.
4. Implémenter les 4 endpoints ci-dessous. La page dégrade proprement si un
   endpoint manque (elle reste utilisable, elle n'affiche juste pas l'état).

## API attendue

### `GET /api/status` → JSON
```json
{ "state":"ok", "label":"Dans la plage", "mgdl":118, "trend":"→",
  "age":40, "fw":"1.4.2", "host":"veilleuse-a1b2c3.local" }
```
`state` ∈ `ok | hi | hs | lo | un | su | bo | er` — pilote directement la couleur et
l'animation du SVG (mêmes noms que les états LED du firmware) :

| state | sens | LED réelle | rendu page |
|---|---|---|---|
| `ok` | dans la plage | `0,36,5` | vert fixe |
| `hi` | hyper > seuil | `66,18,0` | orange fixe |
| `hs` | hyper sévère | `140,38,0` | bloc de 6 qui tourne, 1,6 s |
| `lo` | **hypo** | `255,0,0` | pulsation 1,2 s, pleine intensité |
| `un` | données périmées / réseau coupé | `30,30,30` | blanc fixe — **jamais vert** |
| `su` | attente de configuration | violet respirant | respiration 3 s |
| `bo` | démarrage / OTA | `0,0,28` | point bleu qui tourne |
| `er` | identifiants refusés | violet ⇄ rouge | alternance 600 ms + toast |

`age` en secondes depuis la dernière valeur reçue. Si `age > 900` → renvoyer `state:"un"`.

### `GET /api/config` → JSON (jamais les mots de passe)
```json
{ "configured":true, "ssid":"Freebox-Marion", "region":"eu",
  "email":"marion@exemple.fr", "lo":70, "hi":180, "sev":300,
  "nightOn":"22:00", "nightOff":"07:00", "dim":22, "ota":true,
  "host":"veilleuse-a1b2c3.local" }
```
`configured:true` fait ouvrir directement l'écran réglages (pas l'accueil captif).

### `GET /api/scan` → JSON
```json
{ "networks":[ {"ssid":"Freebox-Marion","rssi":-48}, {"ssid":"Livebox-8A21","rssi":-81} ] }
```
Trier par `rssi` décroissant, dédoublonner les SSID, 8 max (la page tronque au-delà).
Scan asynchrone : si le scan est en cours, répondre `{"networks":[]}` plutôt qu'attendre.

### `POST /api/save` — `application/x-www-form-urlencoded`
Champs : `ssid, wpw, email, dpw, region, lo, hi, sev, nightOn, nightOff, dim, ota`.
`ota` = `1` / `0` (mises à jour automatiques). Les installer uniquement pendant la
plage nuit et **jamais** quand l'état courant est `lo` ou `hs`.
`wpw` / `dpw` vides = « garder l'existant ». Répondre `200` avant de redémarrer
(la page affiche la confirmation puis re-poll `/api/status` à +6 s).
Valider côté firmware : `40 ≤ lo < hi < sev ≤ 400`, `5 ≤ dim ≤ 100`.

## Règles de DA à ne pas casser dans le code

- **`un` est blanc, jamais vert** : un silence ne doit pas ressembler à un « tout va bien ».
- **L'hypo ignore le mode nuit** : ni luminosité réduite, ni durée raccourcie.
- **La gravité passe par le mouvement** : fixe = calme, respiration = attente,
  rotation = alerte, pulsation rapide = urgence. Ne pas ajouter de luminosité pour
  « monter » une alerte, ajouter du rythme.
- Une seule couleur pleine à la fois sur le dôme ; aucun graphique, courbe ni jauge.
- Les tokens sont les variables CSS du `:root` — toute nouvelle vue les réutilise.
- L'adresse `http://<host>/` est affichée en clair sur l'écran réglages : `host` doit
  être renvoyé par `/api/config` **et** `/api/status` (mDNS `veilleuse-<chipid>.local`).

## Contraintes de la page

- Budget ~20 Ko : pas de police à charger (stack système), pas d'icônes (formes CSS/SVG),
  pas de framework, JS vanilla ES5-safe (~2 Ko).
- Mobile d'abord, cibles ≥ 46 px, thème sombre comme cas principal.
- `prefers-reduced-motion` respecté : l'hypo reste visible (opacité pleine, sans pulsation).

## Reste à faire (non couvert par la page)

- OTA : la page ne pilote pas l'upload ; prévoir `/update` séparé si besoin.
- Reset usine : bouton physique ou endpoint `POST /api/forget` (à habiller si voulu).
