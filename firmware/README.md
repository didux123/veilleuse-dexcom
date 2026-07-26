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

## Configuration

```bash
cp include/secrets.h.example include/secrets.h
```

| Fichier / constante | Rôle |
|---|---|
| `secrets.h` → `DEXCOM_USERNAME/PASSWORD` | Compte Dexcom **publieur** (app G7, partage actif) |
| `secrets.h` → `WIFI_SSID/PASSWORD` | Optionnel : WiFi initial (sinon portail captif) |
| `secrets.h` → `OTA_PASSWORD` | Mot de passe OTA (= `--auth` de `platformio.ini`) |
| `HYPO_MGDL` / `HYPER_MGDL` | Seuils (70 / 180 mg/dL) |
| `NIGHT_START_H` / `NIGHT_END_H` / `NIGHT_DIM` | Mode nuit (22 h → 7 h, facteur 0,22) |
| `STALE_MIN` | Fraîcheur max des données (15 min) |
| `DEMO_MODE` | `1` = cycle de tous les états (6 s chacun), sans requête Dexcom |

## Flasher

```bash
pio run -t upload                 # par USB (premier flash)
pio run -e d1_mini_ota -t upload  # par WiFi ensuite (veilleuse.local)
```

Pendant un flash OTA, l'anneau affiche une barre de progression bleue.

## WiFi

- Au boot : connexion au **meilleur réseau connu** (liste persistée en
  LittleFS, `/wifis.txt`, max 10 réseaux — tout réseau rejoint est mémorisé).
- Échec → **portail captif** : la lampe émet `Veilleuse-Dexcom` (LEDs
  violettes), s'y connecter au téléphone et choisir son réseau. Le portail
  alterne avec des reconnexions automatiques (utile après une panne de box).

## Anti-verrouillage Dexcom

Le compte Dexcom peut être bloqué par des authentifications en rafale. Le
firmware : garde `accountId` et session en RAM, re-login **une seule fois**
sur session expirée, backoff exponentiel 60 s → 5 min sur erreur, et arrêt
définitif (motif rouge/bleu) sur identifiants refusés. Ne pas « améliorer »
ça avec des retries agressifs.

## États lumineux

Voir le [README principal](../README.md#code-couleur). Logique dans
`currentState()` + `render()` : priorité aux erreurs (jamais de faux vert),
hypo à pleine puissance même en mode nuit.
