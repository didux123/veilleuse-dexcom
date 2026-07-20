/**
 * Client pour l'API Dexcom Share (non officielle).
 *
 * C'est l'API utilisée par l'app Dexcom Follow, pydexcom et Nightscout.
 * Elle peut changer sans préavis — voir README.
 *
 * Flow d'authentification en 3 étapes :
 *   1. AuthenticatePublisherAccount  → accountId (stable, caché à vie du process)
 *   2. LoginPublisherAccountById     → sessionId (caché, renouvelé si expiré)
 *   3. ReadPublisherLatestGlucoseValues → mesures de glycémie
 */

// Application ID publique utilisée par l'app Dexcom officielle (même valeur que pydexcom/Nightscout)
const APPLICATION_ID = 'd89443d2-327c-4a6f-89e5-496bbb0317db';

const BASE_URLS = {
  us: 'https://share2.dexcom.com/ShareWebServices/Services',
  ous: 'https://shareous1.dexcom.com/ShareWebServices/Services',
};

// UUID nul renvoyé par Dexcom quand l'auth échoue silencieusement
const NULL_UUID = '00000000-0000-0000-0000-000000000000';

const TREND_ARROWS = {
  DoubleUp: '↑↑',
  SingleUp: '↑',
  FortyFiveUp: '↗',
  Flat: '→',
  FortyFiveDown: '↘',
  SingleDown: '↓',
  DoubleDown: '↓↓',
  None: '–',
  NotComputable: '?',
  RateOutOfRange: '?',
};

/** Erreur fatale : identifiants invalides — inutile de réessayer (risque de lock du compte). */
class DexcomCredentialsError extends Error {
  constructor(message) {
    super(message);
    this.name = 'DexcomCredentialsError';
  }
}

/** Erreur de session : le sessionId a expiré, un re-login suffit. */
class DexcomSessionError extends Error {
  constructor(message) {
    super(message);
    this.name = 'DexcomSessionError';
  }
}

class DexcomClient {
  /**
   * @param {object} opts
   * @param {string} opts.username  Username legacy, email, ou téléphone au format +33...
   * @param {string} opts.password
   * @param {string} [opts.region]  'ous' (Europe / hors US) ou 'us'
   */
  constructor({ username, password, region = 'ous' }) {
    if (!username || !password) {
      throw new DexcomCredentialsError('DEXCOM_USERNAME et DEXCOM_PASSWORD sont requis');
    }
    if (!BASE_URLS[region]) {
      throw new Error(`Région inconnue "${region}" — valeurs possibles : ${Object.keys(BASE_URLS).join(', ')}`);
    }
    this.username = username;
    this.password = password;
    this.baseUrl = BASE_URLS[region];
    this.accountId = null; // ne change jamais pour un compte donné
    this.sessionId = null; // expire côté Dexcom, renouvelé à la demande
  }

  /** POST JSON vers l'API Share ; retourne { ok, status, body } sans jeter sur les erreurs HTTP. */
  async _post(path, payload) {
    const res = await fetch(this.baseUrl + path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
      body: payload !== undefined ? JSON.stringify(payload) : undefined,
    });
    const text = await res.text();
    return { ok: res.ok, status: res.status, text };
  }

  /** Analyse une réponse d'erreur Dexcom et jette le type d'erreur approprié. */
  _throwFromError(status, text, context) {
    let code = '';
    try {
      code = JSON.parse(text)?.Code || '';
    } catch {
      /* corps non-JSON, on garde le texte brut */
    }
    if (code === 'AccountPasswordInvalid' || code === 'SSO_AuthenticateAccountNotFound' ||
        code === 'AccountNotFound' || code === 'SSO_InternalError') {
      throw new DexcomCredentialsError(
        `Identifiants Dexcom refusés (${code || status}). Vérifie DEXCOM_USERNAME / DEXCOM_PASSWORD ` +
        `(compte qui PUBLIE, pas le follower) et DEXCOM_REGION.`
      );
    }
    if (code === 'SessionIdNotFound' || code === 'SessionNotValid') {
      throw new DexcomSessionError(`Session Dexcom expirée (${code})`);
    }
    throw new Error(`Dexcom ${context} : HTTP ${status} — ${text.slice(0, 300)}`);
  }

  /** Étape 1 : récupère l'accountId (UUID). Caché en mémoire pour toute la vie du process. */
  async authenticate() {
    if (this.accountId) return this.accountId;
    const { ok, status, text } = await this._post('/General/AuthenticatePublisherAccount', {
      accountName: this.username,
      password: this.password,
      applicationId: APPLICATION_ID,
    });
    if (!ok) this._throwFromError(status, text, 'authenticate');
    // La réponse est un UUID en plaintext entouré de guillemets
    const accountId = text.replace(/"/g, '').trim();
    if (!accountId || accountId === NULL_UUID) {
      throw new DexcomCredentialsError('Dexcom a renvoyé un accountId nul — identifiants probablement invalides');
    }
    this.accountId = accountId;
    return accountId;
  }

  /** Étape 2 : ouvre une session (sessionId). Caché en mémoire jusqu'à expiration. */
  async login() {
    const accountId = await this.authenticate();
    const { ok, status, text } = await this._post('/General/LoginPublisherAccountById', {
      accountId,
      password: this.password,
      applicationId: APPLICATION_ID,
    });
    if (!ok) this._throwFromError(status, text, 'login');
    const sessionId = text.replace(/"/g, '').trim();
    if (!sessionId || sessionId === NULL_UUID) {
      throw new DexcomCredentialsError('Dexcom a renvoyé un sessionId nul — identifiants probablement invalides');
    }
    this.sessionId = sessionId;
    return sessionId;
  }

  /**
   * Étape 3 : lit les dernières mesures.
   * Sur session expirée : re-login puis UN SEUL retry (jamais de boucle,
   * pour éviter un lock du compte côté Dexcom).
   *
   * @returns {Promise<Array<{mgdl: number, trend: string, trendArrow: string, timestamp: Date}>>}
   *          triées de la plus récente à la plus ancienne
   */
  async getLatestGlucoseValues(minutes = 1440, maxCount = 288) {
    if (!this.sessionId) await this.login();

    let result = await this._readGlucose(minutes, maxCount);
    if (result.sessionExpired) {
      // La session a expiré : on la jette, on se re-loggue, et on retente une fois.
      this.sessionId = null;
      await this.login();
      result = await this._readGlucose(minutes, maxCount);
      if (result.sessionExpired) {
        throw new DexcomSessionError('Session Dexcom toujours invalide après re-login');
      }
    }
    return result.readings;
  }

  /** Appel brut de lecture. Retourne { sessionExpired: true } au lieu de jeter sur erreur de session. */
  async _readGlucose(minutes, maxCount) {
    const path = `/Publisher/ReadPublisherLatestGlucoseValues` +
      `?sessionId=${encodeURIComponent(this.sessionId)}&minutes=${minutes}&maxCount=${maxCount}`;
    const { ok, status, text } = await this._post(path);
    if (!ok) {
      try {
        this._throwFromError(status, text, 'lecture glycémie');
      } catch (err) {
        if (err instanceof DexcomSessionError) return { sessionExpired: true };
        throw err;
      }
    }
    const raw = JSON.parse(text);
    return { readings: raw.map(parseReading) };
  }
}

/** Transforme un objet brut Dexcom { WT, Value, Trend } en mesure exploitable. */
function parseReading(entry) {
  // WT est au format "Date(1691455258000)" — epoch en millisecondes
  const match = /Date\((\d+)/.exec(entry.WT || '');
  const timestamp = match ? new Date(Number(match[1])) : null;
  return {
    mgdl: entry.Value,
    trend: entry.Trend,
    trendArrow: TREND_ARROWS[entry.Trend] || '?',
    timestamp,
  };
}

/** Conversion mg/dL → mmol/L, arrondie à 1 décimale. */
function mgdlToMmol(mgdl) {
  return Math.round((mgdl / 18.0182) * 10) / 10;
}

module.exports = {
  DexcomClient,
  DexcomCredentialsError,
  DexcomSessionError,
  mgdlToMmol,
  TREND_ARROWS,
};
