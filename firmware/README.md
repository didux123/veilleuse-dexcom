# Firmware veilleuse — ESP8266 (Wemos D1 mini)

PlatformIO, framework Arduino. Tout le comportement est dans
[`src/main.cpp`](src/main.cpp) ; l'interface de configuration est la page
[`web/veilleuse-config.html`](web/veilleuse-config.html).

## Câblage

```
Ruban SK6812/WS2812B          Wemos D1 mini
   5V  ──────────────────────  5V
   GND ──────────────────────  G
   DIN ──────────────────────  D1 (GPIO5)
```

- 12 LED (`NUM_LEDS`), ruban 60 LED/m collé en anneau dans le boîtier.
- Optionnel mais recommandé : ~330 Ω en série sur DIN, condensateur
  470–1000 µF entre 5V et GND.
- Consommation max réelle ≈ 0,3 A (rouge pur ×12) : n'importe quel
  chargeur USB correct suffit.

## Configuration : rien à compiler

**Aucun identifiant n'est compilé dans le binaire.** Au premier démarrage (ou
si le WiFi est inconnu), la lampe émet le réseau **ouvert** `Veilleuse-XXXX` :
on s'y connecte depuis un téléphone et **la page s'ouvre toute seule**, comme
un portail WiFi d'hôtel. Un seul formulaire regroupe :

| Champ | Rôle |
|---|---|
| Réseau WiFi + mot de passe | Connexion (jusqu'à 10 réseaux mémorisés) |
| Email + mot de passe Dexcom | Compte **publieur** (app G7, partage actif) |
| Région | Europe (`shareous1`) ou US (`share2`) |
| Seuils hypo / hyper / gyrophare | mg/dL, validés `40 ≤ hypo < hyper < gyrophare ≤ 400` |
| Mode nuit : début, fin, luminosité | Horaires **HH:MM** et % (5-100) |
| Mises à jour automatiques | Activables / désactivables |

Les champs de mot de passe laissés vides ne modifient rien (pratique pour ne
changer qu'un seuil). Tout est persisté dans `/config.cfg` en flash.

**La même page reste accessible en permanence** sur le réseau local à
`http://veilleuse-xxxxxx.local/` : c'est là qu'on revient pour modifier les
réglages. L'adresse est affichée à la fin du parcours d'installation.

Pour le développement, `include/secrets.h` (copié depuis `secrets.h.example`)
permet de pré-remplir WiFi et compte Dexcom. La config du portail est toujours
prioritaire sur `secrets.h`.

## Identité unique

Chaque lampe dérive son identité de sa puce (`ESP.getChipId()`) : hostname
mDNS/OTA `veilleuse-xxxxxx`, SSID de portail `Veilleuse-XXXX`. Plusieurs lampes
cohabitent sans collision. Visible dans `GET /api/diag` ou en série au boot :

```
[id] chipid 5FB874 | hostname veilleuse-5fb874 | AP Veilleuse-B874 | fw 1.2.0
```

## Endpoints HTTP

| Endpoint | Rôle |
|---|---|
| `GET /` | La page (gzip depuis PROGMEM, ~6 Ko) |
| `GET /api/status` | `state`, `label`, `mgdl`, `trend`, `age`, `fw`, `host` |
| `GET /api/config` | Réglages courants — **jamais de mot de passe** |
| `GET /api/scan` | Réseaux vus (cache ; relance un scan en fond) |
| `POST /api/save` | Enregistre (x-www-form-urlencoded) puis redémarre |
| `GET /api/diag` | Heap, uptime, `lps`, WiFi, état — diagnostic |
| `GET /api/log` | Les ~1000 derniers caractères du journal |
| `GET /api/portal?on=1` | Force l'ouverture du portail (test sans couper le WiFi) |

`state` ∈ `ok|hi|hs|lo|un|su|bo|er`, calqué sur les états LED.
Les sondes de détection captive (`/generate_204`, `/hotspot-detect.html`,
`/ncsi.txt`…) renvoient un 302 vers l'IP du point d'accès quand le portail est
ouvert : c'est ce qui déclenche l'ouverture automatique de la page.

## Flasher

```bash
pio run -t upload                                                      # USB
pio run -e d1_mini_ota -t upload --upload-port veilleuse-xxxxxx.local    # WiFi
```

Le port série est auto-détecté ; l'OTA accepte aussi une adresse IP. Pendant un
flash, l'anneau affiche une barre bleue. **Une fenêtre OTA de 3 s est garantie à
chaque démarrage**, avant l'initialisation du serveur web : si un firmware
défectueux est publié, il reste ce créneau pour reflasher sans démonter la lampe.

## La page web

`web/veilleuse-config.html` est la source éditable (17 Ko). Avant chaque
compilation, `tools/gen_web.py` (pre-script PlatformIO) la gzippe et génère
`include/web_page.h` (~6 Ko en PROGMEM, non versionné). Éditer la page suffit,
le build s'occupe du reste.

Pour itérer sur le design sans flasher : servir la page depuis un petit serveur
local qui renvoie des JSON figés sur les 4 endpoints.

## Mises à jour automatiques

Chaque lampe consulte les [GitHub Releases](https://github.com/didux123/veilleuse-dexcom/releases)
du projet et s'installe la nouvelle version toute seule :

- **Quand** : après le premier relevé réussi au démarrage, puis chaque nuit
  vers 4 h (la minute dépend du chipid, ce qui étale les téléchargements).
- **Quoi** : n'installe que des versions **strictement plus récentes**
  (comparaison sémantique `X.Y.Z`). ⚠️ Une simple inégalité serait tentante
  pour permettre les retours en arrière, mais elle **rétrograde** toute lampe
  portant un firmware plus récent que la release publiée — mesuré en vrai :
  une lampe en 1.2.0 s'est réinstallé la 1.1.0 publiée, perdant son serveur
  web. Pour revenir en arrière, **republier l'ancien code sous un numéro plus
  haut**.
- **Garde-fous** : désactivable depuis la page ; jamais pendant une hypo ou un
  gyrophare ; jamais si quelqu'un consulte la page (moins de 60 s) ou est
  connecté au portail ; pas si la RAM libre est insuffisante ; une version qui
  échoue est retentée 3 fois au maximum (suivi dans `/update.txt`).

Chaque lampe publie aussi sa version en mDNS :
`dns-sd -L veilleuse-xxxxxx _arduino._tcp local` affiche `fw=1.2.0`.

⚙️ Détail à connaître : `objects.githubusercontent.com` n'annonce pas de
`max_fragment_length` et envoie des enregistrements TLS pleine taille. Le
téléchargement du binaire renvoie **zéro octet** avec un buffer de réception de
1 ou 4 Ko — il faut **16 Ko** (`TLS_RX_BULK`). `version.txt` se contente de 4 Ko.

⚠️ L'ESP8266 n'a pas de partition de secours : un firmware qui ne démarre pas
oblige à un reflash de chaque lampe. D'où l'étape canari du script de release.

## Publier une version

```bash
./scripts/release.sh
```

Le script refuse un arbre git sale, un HEAD non poussé ou un tag existant ;
il compile, génère `version.txt`, **impose de valider la version sur sa propre
lampe** (canari) puis publie la release. Incrémenter `FW_VERSION` dans
`main.cpp` avant de le lancer.

Pour tester la chaîne d'auto-update sans exposer le parc : publier une
**prerelease** (invisible de `latest`) et flasher une lampe de test avec
l'environnement `d1_mini_testupd`.

## Fichiers en flash (LittleFS)

| Fichier | Contenu |
|---|---|
| `/wifis.txt` | Réseaux connus, `ssid<TAB>motdepasse`, plus récent en tête |
| `/config.cfg` | `ver=2`, identifiants, région, seuils, mode nuit (minutes), `ota` |
| `/update.txt` | Dernière version tentée + nombre d'essais (anti-boucle) |

Le format v1 (horaires de nuit en heures entières) est migré automatiquement
vers v2 (minutes) au premier démarrage.

## États lumineux

Voir le [README principal](../README.md#code-couleur). Logique dans
`currentState()` + `render()` : priorité aux erreurs (jamais de faux vert),
hypo à pleine puissance même en mode nuit. `DEMO_MODE 1` en tête de
`main.cpp` fait défiler tous les états (6 s chacun) sans appeler Dexcom.

**Violet/rouge alterné** : identifiants Dexcom refusés ou absents. La lampe
ouvre son portail pour permettre la correction sans reflash, et n'envoie plus
aucune requête à Dexcom tant que de nouveaux identifiants n'ont pas été
enregistrés : une action humaine = un essai (protège le compte d'un
verrouillage).

## Anti-verrouillage Dexcom

Le compte peut être bloqué par des authentifications en rafale. Le firmware
garde `accountId` et la session en RAM, re-login **une seule fois** sur session
expirée, backoff exponentiel 60 s → 5 min sur erreur réseau, et arrêt complet
des requêtes sur identifiants refusés. Ne pas « améliorer » ça avec des
retries agressifs.

## Ordonnancement (à ne pas casser)

`server.handleClient()` et `dns.processNextRequest()` sont **en tête de
`loop()`**. Les opérations bloquantes restantes sont encadrées de garde-fous :
requête TLS Dexcom (2-6 s, différée si quelqu'un consulte la page ou si un scan
tourne), `wifiMulti.run()` (≤ 8 s, jamais si un client est sur le portail),
téléchargement d'une mise à jour (30-60 s, serveur et AP fermés le temps du
flash). Le scan WiFi est **asynchrone** — un scan synchrone appelle
`esp_suspend()` et figerait la boucle, donc les LED et le serveur web.
`GET /api/diag` expose `lps` (itérations de `loop()` par seconde, ~12 000 en
régime normal) : une chute signale un gel.
