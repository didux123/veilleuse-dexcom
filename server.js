/**
 * Serveur Express : proxy entre l'API Dexcom Share et les clients
 * (page web + futur ESP8266).
 *
 * Principe : le serveur poll Dexcom toutes les 60 s et met le résultat en
 * cache mémoire. Les endpoints /api/* servent UNIQUEMENT le cache — aucune
 * requête client ne déclenche d'appel Dexcom.
 */
// Charge .env puis .env.local en repli (dotenv n'écrase pas les variables déjà définies)
require('dotenv').config();
require('dotenv').config({ path: '.env.local' });
const path = require('path');
const express = require('express');
const { DexcomClient, DexcomCredentialsError, mgdlToMmol } = require('./dexcom');

const PORT = Number(process.env.PORT) || 3000;
const POLL_INTERVAL_MS = 60_000; // polling nominal : 60 s
const MAX_BACKOFF_MS = 300_000; // backoff plafonné à 5 min
const STALE_MINUTES = 15; // au-delà : données considérées périmées

// --- Validation de la config au démarrage -------------------------------
const { DEXCOM_USERNAME, DEXCOM_PASSWORD, DEXCOM_REGION } = process.env;
if (!DEXCOM_USERNAME || !DEXCOM_PASSWORD) {
  console.error('❌ DEXCOM_USERNAME et/ou DEXCOM_PASSWORD manquants dans .env');
  console.error('   Copie .env.example vers .env, remplis tes identifiants, puis relance.');
  process.exit(1);
}

const client = new DexcomClient({
  username: DEXCOM_USERNAME,
  password: DEXCOM_PASSWORD,
  region: DEXCOM_REGION || 'ous',
});

// --- Cache mémoire ------------------------------------------------------
const cache = {
  readings: [], // mesures triées récente → ancienne
  lastFetch: null, // Date du dernier poll réussi
  error: null, // message d'erreur du dernier poll échoué (null si OK)
  credentialsInvalid: false, // true = on arrête de poller (évite le lock du compte)
};

let backoffMs = POLL_INTERVAL_MS;

async function poll() {
  try {
    cache.readings = await client.getLatestGlucoseValues(1440, 288);
    cache.lastFetch = new Date();
    cache.error = null;
    backoffMs = POLL_INTERVAL_MS; // reset du backoff après un succès
  } catch (err) {
    cache.error = err.message;
    if (err instanceof DexcomCredentialsError) {
      // Identifiants refusés : on ne réessaie PAS (risque de lock du compte).
      cache.credentialsInvalid = true;
      console.error(`❌ ${err.message}`);
      console.error('   Le serveur reste en ligne mais ne pollera plus. Corrige .env et relance.');
      return; // pas de re-planification
    }
    // Erreur réseau / API : retry avec backoff exponentiel
    backoffMs = Math.min(backoffMs * 2, MAX_BACKOFF_MS);
    console.error(`⚠️  Poll Dexcom échoué : ${err.message} — prochain essai dans ${backoffMs / 1000} s`);
  }
  setTimeout(poll, backoffMs);
}

// --- Helpers ------------------------------------------------------------
function ageMinutes(date) {
  return Math.round((Date.now() - date.getTime()) / 60000);
}

/** Construit l'objet plat servi par /api/glucose (format ESP8266). */
function currentGlucose() {
  const last = cache.readings[0];
  if (!last) {
    return {
      mgdl: null,
      mmol: null,
      trend: null,
      trendArrow: null,
      timestamp: null,
      ageMinutes: null,
      status: 'error',
      error: cache.error || 'Aucune donnée disponible',
    };
  }
  const age = ageMinutes(last.timestamp);
  const payload = {
    mgdl: last.mgdl,
    mmol: mgdlToMmol(last.mgdl),
    trend: last.trend,
    trendArrow: last.trendArrow,
    timestamp: last.timestamp.toISOString(),
    ageMinutes: age,
    status: age > STALE_MINUTES ? 'stale' : 'ok',
  };
  // On expose l'erreur de poll en cours même si on sert une vieille valeur
  if (cache.error) payload.error = cache.error;
  return payload;
}

// --- Express ------------------------------------------------------------
const app = express();
app.use(express.static(path.join(__dirname, 'public')));

// Endpoint plat et léger — c'est celui que l'ESP8266 utilisera.
app.get('/api/glucose', (_req, res) => {
  res.json(currentGlucose());
});

app.get('/api/history', (req, res) => {
  const hours = Math.min(Math.max(Number(req.query.hours) || 24, 1), 24);
  const cutoff = Date.now() - hours * 3600_000;
  const points = cache.readings
    .filter((r) => r.timestamp && r.timestamp.getTime() >= cutoff)
    .map((r) => ({ mgdl: r.mgdl, mmol: mgdlToMmol(r.mgdl), timestamp: r.timestamp.toISOString() }))
    .reverse(); // ancienne → récente, pratique pour le graphe
  res.json(points);
});

// --- Démarrage ----------------------------------------------------------
app.listen(PORT, () => {
  console.log(`🩸 Serveur glycémie sur http://localhost:${PORT}`);
  console.log(`   API ESP8266 : http://localhost:${PORT}/api/glucose`);
  poll(); // premier poll immédiat, puis auto-replanifié
});
