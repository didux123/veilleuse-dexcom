# Moniteur glycémie Dexcom G7

Mini web app qui affiche en temps réel la glycémie d'un capteur **Dexcom G7** via l'**API Dexcom Share** (non officielle — celle utilisée par pydexcom et Nightscout). Le serveur Node.js fait proxy : il poll Dexcom toutes les 60 s, met le résultat en cache, et le sert à la page web ainsi qu'à un endpoint JSON minimaliste destiné à un **ESP8266** (projet lampe d'alerte à venir).

> ⚠️ **Avertissement médical** : cet outil est un affichage de confort. Il peut être en retard, périmé ou en panne. **Les alarmes de l'app Dexcom officielle restent la seule référence médicale.**

> 📄 **Contexte complet du projet** (vision, API Dexcom en détail, projet de veilleuse ESP8266, modélisation 3D du boîtier) : voir **[CONTEXT.md](CONTEXT.md)**.

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

## Comment ça marche

### Authentification (dexcom.js)

L'API Share s'utilise en 3 étapes, toutes en `POST` JSON sur
`https://shareous1.dexcom.com/ShareWebServices/Services` (région `ous`) :

1. **`/General/AuthenticatePublisherAccount`** avec `{ accountName, password, applicationId }`
   → renvoie l'**accountId** (UUID en plaintext, guillemets à retirer). Il est
   dérivé automatiquement de l'email/mot de passe et ne change jamais pour un
   compte donné — il est gardé en mémoire pour toute la vie du process.
2. **`/General/LoginPublisherAccountById`** avec `{ accountId, password, applicationId }`
   → renvoie le **sessionId** (UUID), gardé en mémoire jusqu'à expiration.
3. **`/Publisher/ReadPublisherLatestGlucoseValues?sessionId=...&minutes=1440&maxCount=288`**
   → tableau de mesures `{ WT: "Date(1691455258000)", Value: 85, Trend: "Flat" }`.
   `WT` est un epoch en millisecondes, `Value` est en mg/dL.

L'`applicationId` (`d89443d2-...`) est une constante publique commune à
pydexcom/Nightscout, pas un secret.

**Expiration de session** : si l'étape 3 renvoie `SessionIdNotFound` /
`SessionNotValid`, le client refait un login puis retente **une seule fois**
(jamais de boucle — des tentatives répétées avec un mauvais mot de passe
peuvent verrouiller le compte Dexcom).

**Erreurs distinguées** : `DexcomCredentialsError` (identifiants refusés →
fatal, on arrête de poller) vs erreurs réseau (→ retry avec backoff côté
serveur).

### Serveur (server.js)

- Poll Dexcom **toutes les 60 s**, résultat dans un cache mémoire.
- Les endpoints ne servent **que le cache** — aucune requête client ne
  déclenche d'appel Dexcom (protège des rafales et du rate limit).
- Échec réseau → backoff exponentiel 60 s → 2 min → … → 5 min max, reset au
  premier succès. La dernière valeur connue reste servie pendant la panne.
- `status` calculé à la volée : `ok`, `stale` (> 15 min), `error` (aucune donnée).

### UI (public/index.html)

- Fetch `/api/glucose` toutes les 60 s, graphe via `/api/history` toutes les 5 min,
  « il y a X min » recalculé côté client toutes les 10 s.
- Seuils hypo/hyper stockés en `localStorage` (défauts 70/180 mg/dL) ;
  ils pilotent la couleur de fond et les lignes pointillées du graphe
  (dessinées par un petit plugin Chart.js inline).
- Conversion : `mmol/L = mg/dL ÷ 18.0182`.
