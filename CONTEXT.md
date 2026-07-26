# CONTEXTE DU PROJET — Moniteur de glycémie Dexcom G7 + veilleuse d'alerte

> Document de référence unique du projet. À lire en premier pour reprendre le
> contexte (humain ou IA). Contient : la vision, l'API Dexcom, l'app serveur
> déjà faite, le projet de veilleuse ESP, et la modélisation 3D du boîtier.
>
> ⚠️ **Aucun identifiant Dexcom ne doit figurer dans ce fichier ni dans le
> repo.** Les identifiants vivent uniquement dans `.env` / `.env.local`
> (gitignorés).

---

## 1. Vision

Projet perso autour d'un capteur **Dexcom G7**, en deux parties :

1. **Web app Node.js** (FAITE) — lit la glycémie via l'API Dexcom Share et
   l'affiche en temps réel dans le navigateur, avec un endpoint JSON.
2. **Veilleuse connectée** (À FAIRE) — un objet lumineux à base d'**ESP8266 +
   LEDs adressables WS2812** qui change de couleur selon la glycémie. C'est le
   vrai but final : un objet posé dans la chambre / le salon.

**Objectif humain** : un coup d'œil suffit, même de loin et la nuit, pour
connaître son état glycémique — sans sortir le téléphone.

**Cadre médical** : outil de **confort uniquement**. L'API Share est non
officielle, peut être en retard ou tomber en panne. **Les alarmes de l'app
Dexcom officielle restent la seule référence médicale.**

**Prérequis Dexcom** : le partage doit être activé dans l'app G7 avec au moins
un follower ; on utilise les identifiants du compte qui **PUBLIE** les données,
pas ceux d'un follower.

---

## 2. L'API Dexcom Share (non officielle)

C'est l'API utilisée par l'app Dexcom Follow, **pydexcom** et **Nightscout**.
Non documentée, peut changer sans préavis.

- **Base URL (Europe / hors US, région `ous`)** :
  `https://shareous1.dexcom.com/ShareWebServices/Services`
- **Base URL (US, région `us`)** :
  `https://share2.dexcom.com/ShareWebServices/Services`
- **applicationId** (constante publique, commune à pydexcom/Nightscout, **pas un
  secret**) : `d89443d2-327c-4a6f-89e5-496bbb0317db`
- **Headers** : `Content-Type: application/json`, `Accept: application/json`

### Flow d'authentification en 3 étapes

1. **`POST /General/AuthenticatePublisherAccount`**
   Body : `{ accountName, password, applicationId }`
   → renvoie l'**accountId** (UUID en plaintext, entouré de guillemets à
   retirer). Dérivé de l'email/mot de passe, ne change jamais pour un compte
   donné. `accountName` peut être un username legacy, un email, ou un téléphone
   au format `+33...`.

2. **`POST /General/LoginPublisherAccountById`**
   Body : `{ accountId, password, applicationId }`
   → renvoie le **sessionId** (UUID, guillemets à retirer aussi).

3. **`POST /Publisher/ReadPublisherLatestGlucoseValues?sessionId=...&minutes=1440&maxCount=288`**
   → tableau JSON : `[{ "WT": "Date(1691455258000)", "Value": 85, "Trend": "Flat" }]`
   - `WT` = timestamp epoch en **millisecondes**, à parser depuis le format
     `Date(...)`.
   - `Value` = glycémie en **mg/dL**.
   - `Trend` ∈ { DoubleUp, SingleUp, FortyFiveUp, Flat, FortyFiveDown,
     SingleDown, DoubleDown, None, NotComputable, RateOutOfRange }.

### Règles de robustesse (IMPORTANTES)

- **Session expirée** : si l'étape 3 renvoie une erreur (typiquement 500 avec
  `SessionIdNotFound` ou `SessionNotValid`), refaire le login puis réessayer
  **UNE SEULE FOIS**. **JAMAIS de boucle infinie** sur les erreurs d'auth —
  risque de **verrouillage du compte** Dexcom.
- **Identifiants invalides** (`AccountPasswordInvalid`,
  `SSO_AuthenticateAccountNotFound`, etc.) = erreur **fatale**, pas de retry.
- **accountId nul** (`00000000-0000-0000-0000-000000000000`) = identifiants
  probablement invalides.
- **Erreurs réseau** = à traiter avec un backoff, pas comme une erreur d'auth.

### Conversion des unités

`mmol/L = mg/dL ÷ 18.0182` (arrondi à 1 décimale).

### Mapping des flèches de tendance

| Trend | Flèche |
|---|---|
| DoubleUp | ↑↑ |
| SingleUp | ↑ |
| FortyFiveUp | ↗ |
| Flat | → |
| FortyFiveDown | ↘ |
| SingleDown | ↓ |
| DoubleDown | ↓↓ |
| None | – |
| NotComputable / RateOutOfRange | ? |

---

## 3. PARTIE 1 — Serveur web (terminé)

**Stack** : Node.js ≥ 18 (fetch natif), Express, dotenv. Zéro framework front.
Sur GitHub : `didux123/veilleuse-dexcom`.

### Fichiers

| Fichier | Rôle |
|---|---|
| `dexcom.js` | Client API Share : classe `DexcomClient`, auth 3 étapes, gestion de session (1 retry), parsing, erreurs typées (`DexcomCredentialsError`, `DexcomSessionError`), `mgdlToMmol`. |
| `server.js` | Express. Poll Dexcom toutes les **60 s**, cache mémoire, backoff exponentiel (60 s → 2 min → … → 5 min max) sur erreur réseau. Les endpoints ne servent QUE le cache. |
| `test-dexcom.js` | Test CLI de l'auth (`npm test`) — à lancer avant tout. |
| `public/index.html` | UI vanilla + Chart.js (CDN). Mode sombre, valeur géante, mmol/L, flèche, graphe 24 h, seuils réglables (localStorage), fond rouge/orange/vert, états "stale" et "erreur". |

### Endpoints

| Endpoint | Description |
|---|---|
| `GET /` | Page web. |
| `GET /api/glucose` | **JSON plat, pensé pour l'ESP.** Voir ci-dessous. |
| `GET /api/history?hours=24` | Historique pour le graphe (1–24 h). |

**`GET /api/glucose`** :
```json
{
  "mgdl": 85,
  "mmol": 4.7,
  "trend": "Flat",
  "trendArrow": "→",
  "timestamp": "2026-07-20T14:32:18.000Z",
  "ageMinutes": 3,
  "status": "ok"
}
```
`status` : `ok` | `stale` (dernière mesure > 15 min) | `error` (aucune donnée).
Pas d'authentification (usage réseau local uniquement).

### Comportement d'erreur

- Identifiants invalides → message clair, serveur reste en ligne mais arrête de
  poller (pas de crash loop, pas de risque de lock).
- API injoignable → retry avec backoff, l'UI affiche l'erreur.
- Pas de données récentes → dernière valeur connue affichée + état "stale",
  jamais d'écran vide.

### Config

Variables dans `.env` (ou `.env.local` en repli) :
`DEXCOM_USERNAME`, `DEXCOM_PASSWORD`, `DEXCOM_REGION=ous`, `PORT=3000`.

---

## 4. PARTIE 2 — Veilleuse ESP (à faire)

### Décision d'architecture : PAS de serveur

L'ESP fait **tout en autonomie** — il parle directement à l'API Dexcom Share en
HTTPS (les mêmes 3 étapes), parse le JSON, pilote les LEDs. Aucune machine à
laisser allumée. On le branche sur une prise USB avec du WiFi et il vit sa vie.

**Compromis assumés :**
- **TLS sur la puce** : l'API est en HTTPS. On skippe la vérif du certificat
  (`setInsecure`) — acceptable pour un objet perso, un peu moins "propre".
  C'est la partie la plus lourde en RAM sur ESP8266.
- **Identifiants stockés en flash** sur l'objet (qui a l'objet peut
  potentiellement les extraire — acceptable pour un usage personnel).
- **Même discipline anti-lock** que le serveur : re-login UNE fois, jamais de
  boucle.
- Reflasher pour changer la logique (mitigé par l'OTA une fois en place).

### Matériel réellement disponible

- **1× ESP8266** — marquage `ESP8266MOD` (= module ESP-12, type NodeMCU /
  Wemos D1 mini). ⚠️ **PAS un ESP32.**
- **LEDs adressables WS2812** (NeoPixel).
- Rien d'autre : pas d'écran, pas de bouton, pas d'ESP32.

**À préciser avant de coder le firmware :**
- Forme exacte de la carte (Wemos D1 mini carré ? NodeMCU long à 2 rangées ?)
  → pour le brochage.
- Nombre de LEDs sur le ruban / l'anneau WS2812.

### Note HomeKit

HomeKit natif propre (**HomeSpan**) est **ESP32 UNIQUEMENT**. Sur ESP8266, faire
tourner HomeKit + TLS Dexcom + LEDs sur la même puce = RAM très tendue,
expérimental. Donc **pour l'instant : veilleuse standalone SANS HomeKit**. Un
ESP32 à quelques euros débloquerait HomeKit proprement plus tard, sans jeter le
firmware (la logique se réutilise).

### Comportement lumineux voulu

- 🟢 **Vert doux** = dans la plage.
- 🟠 **Orange** = hyper (> seuil haut).
- 🔴 **Rouge** = hypo (< seuil bas).
- **Pulsation "respiration"** en hypo (bien plus alertant qu'une couleur fixe).
- 🔵⚪ **Bleu / blanc clignotant** = problème : données périmées
  (`status: "stale"`), Dexcom injoignable, WiFi perdu. **JAMAIS de faux vert
  muet** — on doit toujours savoir si l'info est fiable.
- **Seuils codés en dur** au début (ex. hypo < 70, hyper > 180 mg/dL). Pas de
  serveur nécessaire pour ça.

### Plan par étapes

- **Étape 1 (cœur)** : WiFi + auth Dexcom directe + poll 60 s + couleurs +
  pulsation hypo + gestion des états stale/erreur. Seuils et identifiants en
  tête de fichier.
- **Étape 2 (confort, zéro matériel en plus)** : mode nuit (luminosité réduite
  la nuit sauf hypo), nuance selon la flèche de tendance (descente rapide =
  alerte précoce même dans la plage), page web de config embarquée
  (`http://...local`) pour régler seuils/luminosité/WiFi sans reflasher, **OTA**
  (mise à jour sans fil).
- **Étape 3 (quand matériel dispo / ESP32)** : bouton snooze (appui = "vu,
  éteins 30 min"), écran OLED (valeur + flèche + âge), HomeKit.

### Pistes techniques firmware (indicatif)

- IDE Arduino ou PlatformIO.
- Libs : `ESP8266WiFi` / `WiFiClientSecure` (TLS), `ArduinoJson` (parsing),
  `Adafruit_NeoPixel` ou `FastLED` (WS2812), `ESP8266WebServer` (page config),
  `ArduinoOTA` (mises à jour), NTP pour l'âge des mesures.
- Alim WS2812 : sur le 5V (VIN/5V de l'ESP via USB) pour quelques LEDs ; data
  sur une broche GPIO (souvent D4/GPIO2). Résistance ~330 Ω sur la data et
  condensateur ~1000 µF sur l'alim recommandés pour la fiabilité.

---

## 5. Modélisation 3D — le boîtier / diffuseur

Le but : un objet **joli et diffusant**, pas un boîtier technique. La lumière
doit être douce et homogène, sans points chauds (on ne veut pas voir chaque LED).

### Concept général

Deux pièces imprimées :
1. **Le socle (base)** — opaque, tient l'électronique (ESP + anneau LED),
   avec une sortie pour le câble USB. Bois clair, blanc, ou couleur au choix.
2. **Le diffuseur (dôme / abat-jour)** — translucide, se pose ou se visse sur le
   socle. C'est lui qui donne le rendu lumineux.

### Idées de formes de diffuseur

- **Dôme opalin** (verre dépoli imité en PLA blanc) — classique, doux, élégant.
- **Œuf / galet** lisse — moderne, organique.
- **Coque géométrique low-poly** — look "maker" assumé, joue avec les facettes.
- **Cylindre dépoli** — simple, épuré, façon bougie.
- **Nuage / lune** — plus ludique, adapté à une chambre d'enfant.

### Règles pour une bonne diffusion (importantes)

- **PLA blanc naturel** diffuse remarquablement bien la lumière. Le PETG clair
  aussi. Éviter le PLA "cristal" transparent (on voit les LEDs).
- **Parois fines** : ~1 à 1,5 mm (2 à 3 périmètres). Assez fin pour laisser
  passer la lumière, assez épais pour rester opaque aux points chauds.
- **Distance LED → paroi** : garder **10–20 mm** entre les LEDs et la surface du
  diffuseur. Trop près = on voit des taches lumineuses ; cet espace laisse la
  lumière se fondre.
- **Mode vase (spiralize)** possible pour un diffuseur fin en une seule paroi
  continue, sans couture visible.
- **Orientation des LEDs** : anneau WS2812 posé à plat, face vers le haut, qui
  éclaire l'intérieur du dôme ; ou ruban enroulé en spirale contre la paroi
  interne du socle.

### À prévoir dans le modèle

- **Canal / encoche pour le câble USB** en sortie du socle.
- **Supports (standoffs)** pour visser ou clipser la carte ESP.
- **Logement pour l'anneau LED** (rainure ou plots de centrage).
- **Emboîtement socle ↔ diffuseur** : ajustement par friction, baïonnette, ou
  pas de vis imprimé. Prévoir un léger jeu (~0,2 mm) pour l'impression.
- **Aération** discrète (peu critique avec peu de LEDs, mais utile).
- **Anti-fuite de lumière** à la jonction socle/diffuseur (chicane ou
  recouvrement) pour éviter un halo à la base.

### Outils de conception possibles

- **Tinkercad** — le plus simple pour débuter (navigateur, gratuit).
- **Fusion 360** — puissant, gratuit en usage perso, idéal pour les emboîtements
  précis.
- **FreeCAD** — open source, paramétrique.
- **OpenSCAD** — paramétrique par code (pratique pour générer un diffuseur dont
  on change diamètre / hauteur / nombre de facettes par variables).
- **Blender** — pour les formes organiques (nuage, lune, galet).

### Contraintes à garder en tête

- Dimensions pilotées par : diamètre de l'anneau LED + encombrement de l'ESP +
  l'espace de diffusion (10–20 mm). Mesurer les composants avant de modéliser.
- Impression sans support si possible (dôme = surplomb ; concevoir pour
  s'imprimer à l'endroit ou en deux moitiés).
- Prototype d'abord en PLA blanc bon marché pour valider la diffusion avant de
  soigner l'esthétique.

---

## 6. Environnement de dev

- **Repo local** : `/Users/mdemonteynard/Documents/CODE/PERSO/DEXCOM_API`
- **Remote** : `github.com/didux123/veilleuse-dexcom`
- Serveur Node testé et fonctionnel avec de vraies données.
- Firmware ESP : **pas encore commencé** (discussion / conception uniquement).
