/**
 * Script de test CLI : valide l'authentification Dexcom Share et affiche
 * les dernières mesures. À lancer AVANT de démarrer le serveur :
 *
 *   node test-dexcom.js
 */
require('dotenv').config();
require('dotenv').config({ path: '.env.local' });
const { DexcomClient, DexcomCredentialsError, mgdlToMmol } = require('./dexcom');

function maskUuid(uuid) {
  return uuid.slice(0, 8) + '-****-****-****-************';
}

function ageMinutes(date) {
  return Math.round((Date.now() - date.getTime()) / 60000);
}

async function main() {
  const { DEXCOM_USERNAME, DEXCOM_PASSWORD, DEXCOM_REGION } = process.env;

  if (!DEXCOM_USERNAME || !DEXCOM_PASSWORD) {
    console.error('❌ DEXCOM_USERNAME et/ou DEXCOM_PASSWORD manquants.');
    console.error('   Copie .env.example vers .env et remplis tes identifiants.');
    process.exit(1);
  }

  const client = new DexcomClient({
    username: DEXCOM_USERNAME,
    password: DEXCOM_PASSWORD,
    region: DEXCOM_REGION || 'ous',
  });

  console.log(`Région : ${DEXCOM_REGION || 'ous'}`);

  console.log('1/3 Authentification (accountId)...');
  const accountId = await client.authenticate();
  console.log(`    ✅ accountId : ${maskUuid(accountId)}`);

  console.log('2/3 Login (sessionId)...');
  await client.login();
  console.log('    ✅ session ouverte');

  console.log('3/3 Lecture des mesures (24 h)...');
  const readings = await client.getLatestGlucoseValues();
  if (readings.length === 0) {
    console.log('    ⚠️  Auth OK mais aucune mesure renvoyée.');
    console.log('    Vérifie que le partage est activé dans l\'app G7 avec au moins un follower,');
    console.log('    et que le capteur transmet (pas en warmup).');
    return;
  }

  const last = readings[0];
  console.log(`    ✅ ${readings.length} mesures reçues\n`);
  console.log(`    Dernière glycémie : ${last.mgdl} mg/dL (${mgdlToMmol(last.mgdl)} mmol/L) ${last.trendArrow} (${last.trend})`);
  console.log(`    Il y a ${ageMinutes(last.timestamp)} min (${last.timestamp.toLocaleString()})\n`);

  console.log('    Dernières mesures :');
  for (const r of readings.slice(0, 6)) {
    console.log(`      ${r.timestamp.toLocaleTimeString()}  ${String(r.mgdl).padStart(3)} mg/dL  ${r.trendArrow}`);
  }
  console.log('\n✅ Tout fonctionne — tu peux lancer le serveur avec : npm start');
}

main().catch((err) => {
  if (err instanceof DexcomCredentialsError) {
    console.error(`\n❌ ${err.message}`);
  } else {
    console.error(`\n❌ Erreur : ${err.message}`);
    console.error('   (problème réseau ou API Dexcom indisponible — réessaie dans quelques minutes)');
  }
  process.exit(1);
});
