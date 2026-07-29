# Firmware veilleuse — ESP8266 (Wemos D1 mini)

PlatformIO, framework Arduino. Tout le comportement est dans
[`src/main.cpp`](src/main.cpp), réglages en tête de fichier.

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

**En production, aucun identifiant n'est compilé dans le binaire.** Au premier
démarrage (ou si le WiFi est inconnu), la lampe émet le réseau
`Veilleuse-XXXX` (LEDs violettes) — on s'y connecte depuis un téléphone et le
portail s'ouvre. Un seul formulaire regroupe :

| Champ | Rôle |
|---|---|
| Réseau WiFi + mot de passe | Connexion (jusqu'à 10 réseaux mémorisés) |
| Email + mot de passe Dexcom | Compte **publieur** (app G7, partage actif) |
| Compte US | Coché = `share2.dexcom.com`, sinon Europe `shareous1` |
| Seuils hypo / hyper / gyrophare | mg/dL (50-90 / 120-250 / 200-400) |
| Nuit : début, fin, luminosité | Heures locales et % (5-100) |

Les champs Dexcom laissés vides ne modifient pas les identifiants existants
(pratique pour ne changer que le WiFi ou les seuils). Tout est persisté dans
`/config.cfg` en flash.

Pour le développement, `include/secrets.h` (copié depuis `secrets.h.example`)
permet de pré-remplir WiFi et compte Dexcom afin d'éviter le portail à chaque
flash. La config du portail est toujours prioritaire sur `secrets.h`.

## Identité unique

Chaque lampe dérive son identité de sa puce (`ESP.getChipId()`) : hostname
mDNS/OTA `veilleuse-xxxxxx` et SSID de portail `Veilleuse-XXXX`. Plusieurs
lampes cohabitent donc sans collision. Le hostname s'affiche en série au boot :

```
[id] chipid 5FB874 | hostname veilleuse-5fb874 | AP Veilleuse-B874 | fw 1.1.0
```

## Flasher

```bash
pio run -t upload                                                    # USB
pio run -e d1_mini_ota -t upload --upload-port veilleuse-xxxxxx.local  # WiFi
```

Le port série est auto-détecté ; l'OTA accepte aussi une adresse IP (utile si
le mDNS traîne). Pendant un flash, l'anneau affiche une barre bleue.

## Mises à jour automatiques

Chaque lampe vérifie les [GitHub Releases](https://github.com/didux123/veilleuse-dexcom/releases)
du projet et s'installe la version publiée toute seule :

- **Quand** : après le premier relevé réussi au démarrage, puis chaque nuit
  vers 4 h (la minute dépend du chipid, ce qui étale les téléchargements).
- **Quoi** : compare `FW_VERSION` du firmware au fichier `version.txt` de la
  release `latest`. Comparaison par **inégalité** — republier une version
  antérieure fait donc redescendre le parc (rollback volontaire possible).
- **Garde-fous** : jamais pendant une hypo, jamais si quelqu'un est connecté
  au portail, pas si la RAM libre est trop basse, et une version dont le flash
  a échoué n'est **jamais** retentée (mémorisée dans `/update.txt`).

⚠️ L'ESP8266 n'a pas de partition de secours : un firmware qui ne démarre pas
oblige à un reflash USB de chaque lampe. D'où l'étape canari du script de
release.

## Publier une version

```bash
./scripts/release.sh
```

Le script refuse un arbre git sale, un HEAD non poussé ou un tag existant ;
il compile, génère `version.txt`, **impose de valider la version sur sa propre
lampe** (canari) puis publie la release. Incrémenter `FW_VERSION` dans
`main.cpp` avant de le lancer.

Pour tester la chaîne d'auto-update sans exposer le parc : publier une
**prerelease** (`gh release create v0.0.0-test firmware.bin version.txt
--prerelease`, invisible de `latest`) et flasher une lampe de test avec
l'environnement `d1_mini_testupd`.

## Fichiers en flash (LittleFS)

| Fichier | Contenu |
|---|---|
| `/wifis.txt` | Réseaux connus, `ssid<TAB>motdepasse`, plus récent en tête |
| `/config.cfg` | Identifiants Dexcom, région et réglages (`clé=valeur`) |
| `/update.txt` | Dernière version de mise à jour tentée (anti-boucle) |

## États lumineux

Voir le [README principal](../README.md#code-couleur). Logique dans
`currentState()` + `render()` : priorité aux erreurs (jamais de faux vert),
hypo à pleine puissance même en mode nuit. `DEMO_MODE 1` en tête de
`main.cpp` fait défiler tous les états (6 s chacun) sans appeler Dexcom.

Cas particulier — **violet/rouge alterné** : identifiants Dexcom refusés ou
absents. La lampe ouvre alors son portail pour permettre la correction sans
reflash. Aucune requête n'est renvoyée à Dexcom tant que de nouveaux
identifiants n'ont pas été enregistrés : une action humaine = un essai
(protège le compte Dexcom d'un verrouillage).

## Anti-verrouillage Dexcom

Le compte peut être bloqué par des authentifications en rafale. Le firmware
garde `accountId` et la session en RAM, re-login **une seule fois** sur session
expirée, backoff exponentiel 60 s → 5 min sur erreur réseau, et arrêt complet
des requêtes sur identifiants refusés. Ne pas « améliorer » ça avec des
retries agressifs.
