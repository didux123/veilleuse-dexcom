# Moniteur glycémie Dexcom G7

Mini web app qui affiche en temps réel la glycémie d'un capteur **Dexcom G7** via l'**API Dexcom Share** (non officielle — celle utilisée par pydexcom et Nightscout). Le serveur Node.js fait proxy : il poll Dexcom toutes les 60 s, met le résultat en cache, et le sert à la page web ainsi qu'à un endpoint JSON minimaliste destiné à un **ESP8266** (projet lampe d'alerte à venir).

> ⚠️ **Avertissement médical** : cet outil est un affichage de confort. Il peut être en retard, périmé ou en panne. **Les alarmes de l'app Dexcom officielle restent la seule référence médicale.**

## Prérequis côté Dexcom

- Le **partage doit être activé** dans l'app Dexcom G7, avec **au moins un follower** invité (sinon l'API Share ne renvoie rien).
- On utilise les identifiants du **compte qui publie** les données (le compte de l'app G7 sur le téléphone), **pas** ceux d'un follower.
- L'API Share est **non officielle et non documentée** : Dexcom peut la modifier ou la couper sans préavis.

## Installation

```bash
npm install
cp .env.example .env
# puis remplir .env avec tes identifiants
```

Nécessite **Node.js ≥ 18** (fetch natif).

### Configuration `.env`

| Variable | Description |
|---|---|
| `DEXCOM_USERNAME` | Username, email ou téléphone (`+33...`) du compte qui publie |
| `DEXCOM_PASSWORD` | Mot de passe de ce compte |
| `DEXCOM_REGION` | `ous` (Europe / hors US) ou `us` |
| `PORT` | Port du serveur (défaut `3000`) |

Le fichier `.env` est dans `.gitignore` — les identifiants ne quittent jamais le serveur et ne transitent **jamais** côté client.

## Lancement

1. **Valider l'authentification** d'abord :

   ```bash
   node test-dexcom.js
   ```

   Le script fait les 3 étapes d'auth et affiche la dernière glycémie. En cas d'identifiants invalides, il l'indique clairement sans réessayer (pour éviter un verrouillage du compte Dexcom).

2. **Démarrer le serveur** :

   ```bash
   npm start
   ```

   Puis ouvrir <http://localhost:3000>.

## Endpoints

| Endpoint | Description |
|---|---|
| `GET /` | Page web (valeur en gros, graphe 24 h, seuils réglables) |
| `GET /api/glucose` | JSON plat pour l'ESP8266 (voir ci-dessous) |
| `GET /api/history?hours=24` | Historique pour le graphe (1–24 h) |

### `GET /api/glucose` — format ESP8266

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

- `status` : `ok` | `stale` (dernière mesure > 15 min) | `error` (aucune donnée).
- Sans authentification pour l'instant (usage réseau local uniquement — ne pas exposer sur Internet).
- Les endpoints servent un **cache** : aucune requête client ne déclenche d'appel Dexcom.

## Comportement en cas de problème

- **Identifiants invalides** : message clair au démarrage, le serveur reste en ligne mais arrête de poller (pas de crash loop, pas de risque de lock du compte).
- **API Dexcom injoignable** : retry avec backoff exponentiel (60 s → 5 min max), l'UI affiche l'erreur en bandeau.
- **Pas de données récentes** (warmup du capteur, téléphone hors de portée…) : la dernière valeur connue reste affichée, grisée, avec un bandeau « données périmées ». Jamais d'écran vide.

## Structure

```
server.js        # Express : endpoints, cache, polling 60 s + backoff
dexcom.js        # Client API Share : auth 3 étapes, gestion de session, parsing
test-dexcom.js   # Test CLI de l'authentification
public/index.html# UI (vanilla JS + Chart.js via CDN)
```
