// ============================================================
// Veilleuse Dexcom — ESP8266 (Wemos D1 mini) + ruban SK6812/WS2812B
// WiFi + API Dexcom Share directe + poll 60 s + couleurs.
//
// Câblage : DIN ruban -> D1 (GPIO5), 5V -> 5V, GND -> G.
//           (+ résistance ~330 ohms en série sur DIN,
//            condensateur 470-1000 uF entre 5V et GND)
//
// Configuration : tout se règle depuis le portail captif (WiFi + compte
// Dexcom + seuils), persisté en LittleFS. Aucun identifiant n'a besoin
// d'être compilé — secrets.h ne sert qu'au développement.
//
// Codes couleur :
//   vert doux fixe      = dans la plage
//   orange fixe         = hyper (> seuil hyper)
//   gyrophare orange    = hyper severe (> seuil hyper2) : rotation, pas plus
//                         de lumiere, juste du mouvement
//   rouge "respiration" = hypo  (< seuil hypo)
//   blanc doux fixe     = pas de données fiables (périmées > STALE_MIN,
//                         WiFi coupé ou Dexcom injoignable)
//   violet respirant    = configuration WiFi : l'ESP émet le réseau
//                         "Veilleuse-XXXX" -> s'y connecter au téléphone.
//   violet/rouge        = identifiants Dexcom refusés (ou absents) : le
//                         portail est ouvert pour les corriger.
//   rouge/bleu alterné  = identifiants refusés, portail indisponible.
//                         JAMAIS de faux vert muet.
// ============================================================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiManager.h>
#include <ESP8266WiFiMulti.h>
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <ESP8266httpUpdate.h>
#include <LittleFS.h>
#include <time.h>
#include "secrets.h"

// secrets.h est optionnel en production : tout peut rester vide.
#ifndef OTA_PASSWORD
#define OTA_PASSWORD "veilleuse"
#endif
#ifndef DEXCOM_USERNAME
#define DEXCOM_USERNAME ""
#endif
#ifndef DEXCOM_PASSWORD
#define DEXCOM_PASSWORD ""
#endif

#define WIFI_LIST_MAX  10                   // nb max de reseaux memorises

// ------------------- Version / mise a jour -------------------
// FW_VERSION est la source de verite, lue par scripts/release.sh.
#define FW_VERSION     "1.1.0"

#ifndef UPDATE_BASE_URL                     // surchargeable par build_flags
#define UPDATE_BASE_URL "https://github.com/didux123/veilleuse-dexcom/releases/latest/download"
#endif
static const char* UPDATE_VERSION_URL = UPDATE_BASE_URL "/version.txt";
static const char* UPDATE_FW_URL      = UPDATE_BASE_URL "/firmware.bin";
#define UPDATE_TRY_PATH "/update.txt"
#define UPDATE_HOUR     4                   // check quotidien vers 4 h locale

// ------------------- Réglages materiel -------------------
#define LED_PIN        D1      // GPIO5
#define NUM_LEDS       12      // anneau quasi complet (200 mm à 60 LED/m)

// ----- MODE DEMO : cycle tous les etats (6 s chacun), sans requete Dexcom.
//       Remettre a 0 puis reflasher pour le fonctionnement normal. -----
#define DEMO_MODE      0

// Luminosite globale a fond : chaque etat porte sa propre intensite dans ses
// couleurs. Les etats normaux restent doux ; l'HYPO pulse a pleine puissance
// (rouge pur 12 LED = ~240 mA, sans danger pour l'alim USB).

#define STALE_MIN      15        // données plus vieilles -> "stale"
#define POLL_BASE_MS   60000UL   // 60 s
#define POLL_MAX_MS    300000UL  // backoff max 5 min

static const char* DEX_APP_ID = "d89443d2-327c-4a6f-89e5-496bbb0317db"; // public
static const char* DEX_UA = "Dexcom Share/3.0.2.11 CFNetwork/711.2.23 Darwin/14.0.0";

// ------------------- État -------------------
enum DisplayState { ST_BOOT, ST_CONFIG, ST_OK, ST_HYPER, ST_HYPER2, ST_HYPO,
                    ST_STALE, ST_ERROR, ST_BADCREDS, ST_FATAL };

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
WiFiManager wm;
ESP8266WiFiMulti wifiMulti;
unsigned long wifiRetryAt = 0;

// ------------------- Identite (unique par puce) -------------------
static char gHostname[20];   // "veilleuse-a1b2c3" (mDNS / OTA)
static char gApName[16];     // "Veilleuse-B2C3"   (SSID du portail)

static void buildIdentity() {
  uint32_t id = ESP.getChipId() & 0xFFFFFF;
  snprintf(gHostname, sizeof(gHostname), "veilleuse-%06x", id);
  snprintf(gApName, sizeof(gApName), "Veilleuse-%04X", (unsigned)(id & 0xFFFF));
  Serial.printf("[id] chipid %06X | hostname %s | AP %s | fw %s\n",
                id, gHostname, gApName, FW_VERSION);
}

// ------------------- Configuration (LittleFS) -------------------
// /config.cfg : "cle=valeur" par ligne, decoupe sur le PREMIER '=' (le mot
// de passe peut en contenir).
#define CFG_PATH "/config.cfg"

struct Config {
  String user     = DEXCOM_USERNAME;   // fallback dev depuis secrets.h
  String pass     = DEXCOM_PASSWORD;
  bool   regionUS = false;             // false = "ous" (Europe)
  int    hypo     = 70;                // mg/dL
  int    hyper    = 180;
  int    hyper2   = 300;               // gyrophare
  int    nightStart = 22;              // heure locale
  int    nightEnd   = 7;
  int    nightDim   = 22;              // luminosite nuit, en %
};
static Config gCfg;

static int clampInt(int v, int lo, int hi, int def) {
  return (v < lo || v > hi) ? def : v;
}

static const char* dexBase() {
  return gCfg.regionUS
    ? "https://share2.dexcom.com/ShareWebServices/Services"
    : "https://shareous1.dexcom.com/ShareWebServices/Services";
}

// true si des identifiants utilisables ont ete charges.
static bool loadConfig() {
  File f = LittleFS.open(CFG_PATH, "r");
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String k = line.substring(0, eq);
    String v = line.substring(eq + 1);
    int n = v.toInt();
    if      (k == "user")   gCfg.user = v;
    else if (k == "pass")   gCfg.pass = v;
    else if (k == "region") gCfg.regionUS = (v == "us");
    else if (k == "hypo")   gCfg.hypo   = clampInt(n, 50, 90, 70);
    else if (k == "hyper")  gCfg.hyper  = clampInt(n, 120, 250, 180);
    else if (k == "hyper2") gCfg.hyper2 = clampInt(n, 200, 400, 300);
    else if (k == "nstart") gCfg.nightStart = clampInt(n, 0, 23, 22);
    else if (k == "nend")   gCfg.nightEnd   = clampInt(n, 0, 23, 7);
    else if (k == "ndim")   gCfg.nightDim   = clampInt(n, 5, 100, 22);
  }
  f.close();
  return gCfg.user.length() > 0 && gCfg.pass.length() > 0;
}

static bool saveConfig() {
  File w = LittleFS.open(CFG_PATH, "w");
  if (!w) { Serial.println("[cfg] ECHEC ecriture config"); return false; }
  w.print("user=" + gCfg.user + "\n");
  w.print("pass=" + gCfg.pass + "\n");
  w.print(String("region=") + (gCfg.regionUS ? "us" : "ous") + "\n");
  w.print("hypo="   + String(gCfg.hypo)   + "\n");
  w.print("hyper="  + String(gCfg.hyper)  + "\n");
  w.print("hyper2=" + String(gCfg.hyper2) + "\n");
  w.print("nstart=" + String(gCfg.nightStart) + "\n");
  w.print("nend="   + String(gCfg.nightEnd)   + "\n");
  w.print("ndim="   + String(gCfg.nightDim)   + "\n");
  w.close();
  return true;
}

// Petits fichiers d'une ligne (suivi des mises a jour)
static String readLineFile(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return "";
  String v = f.readStringUntil('\n');
  f.close();
  v.replace("\r", "");
  v.trim();
  return v;
}

static void writeLineFile(const char* path, const String& v) {
  File w = LittleFS.open(path, "w");
  if (!w) return;
  w.print(v + "\n");
  w.close();
}

// ------------------- Memoire multi-WiFi (LittleFS) -------------------
// /wifis.txt : une ligne par reseau, "ssid\tmotdepasse", plus recent en tete.

static int loadWifiList() {
  int n = 0;
  File f = LittleFS.open("/wifis.txt", "r");
  if (!f) return 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    int tab = line.indexOf('\t');
    if (tab <= 0) continue;
    String ssid = line.substring(0, tab);
    String pass = line.substring(tab + 1);
    wifiMulti.addAP(ssid.c_str(), pass.length() ? pass.c_str() : nullptr);
    n++;
  }
  f.close();
  return n;
}

static void rememberWifi(const String& ssid, const String& pass) {
  if (ssid.length() == 0) return;
  String keep = "";
  int count = 1;
  File f = LittleFS.open("/wifis.txt", "r");
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.replace("\r", "");
      int tab = line.indexOf('\t');
      if (tab <= 0) continue;
      if (line.substring(0, tab) == ssid) continue;   // deja connu : remplace
      if (count >= WIFI_LIST_MAX) break;              // borne la liste
      keep += line + "\n";
      count++;
    }
    f.close();
  }
  File w = LittleFS.open("/wifis.txt", "w");
  if (!w) { Serial.println("[wifi] ECHEC ecriture liste"); return; }
  w.print(ssid + "\t" + pass + "\n");                 // plus recent en tete
  w.print(keep);
  w.close();
  wifiMulti.addAP(ssid.c_str(), pass.length() ? pass.c_str() : nullptr);
  Serial.printf("[wifi] reseau memorise : %s (%d en liste)\n", ssid.c_str(), count);
}

String  accountId  = "";
String  sessionId  = "";
int     mgdl       = -1;
String  trendStr   = "";
time_t  wtEpoch    = 0;        // horodatage Dexcom de la mesure
bool    fatalCreds = false;
int     failCount  = 0;
unsigned long nextPollAt   = 0;
unsigned long pollInterval = POLL_BASE_MS;

static bool gCredsJustSaved = false;   // pose par le portail, consomme par loop
static bool gFatalPortal    = false;   // portail "correction identifiants"
static bool gBootCheckDone  = false;   // check de mise a jour au demarrage
static int  gLastCheckYday  = -1;      // dernier jour du check quotidien

// ------------------- Utilitaires HTTP/JSON -------------------
static String httpPost(const String& url, const String& payload, int& codeOut) {
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();                 // compromis assumé (pas de vérif cert)
  client->setBufferSizes(4096, 1024);    // RAM ESP8266 oblige
  client->setTimeout(10000);

  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(*client, url)) { codeOut = -1; return ""; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", DEX_UA);
  codeOut = http.POST(payload);
  String body = http.getString();
  http.end();
  return body;
}

// Première chaîne entre guillemets ("uuid") -> uuid
static String firstQuoted(const String& s) {
  int a = s.indexOf('"');
  if (a < 0) return "";
  int b = s.indexOf('"', a + 1);
  if (b < 0) return "";
  return s.substring(a + 1, b);
}

static bool isCredentialError(const String& body) {
  return body.indexOf("AccountPassword") >= 0 ||
         body.indexOf("AccountNotFound") >= 0 ||
         body.indexOf("InvalidPassword") >= 0 ||
         body.indexOf("AccountLoginSuspended") >= 0 ||
         body.indexOf("SSO_AuthenticateMaxAttemptsExceeded") >= 0;
}

static bool isSessionError(const String& body) {
  return body.indexOf("SessionIdNotFound") >= 0 ||
         body.indexOf("SessionNotValid") >= 0;
}

// ------------------- Dexcom Share (3 étapes) -------------------
static bool dexAuthenticate() {
  int code;
  String payload = String("{\"accountName\":\"") + gCfg.user +
                   "\",\"password\":\"" + gCfg.pass +
                   "\",\"applicationId\":\"" + DEX_APP_ID + "\"}";
  String body = httpPost(String(dexBase()) + "/General/AuthenticatePublisherAccount",
                         payload, code);
  Serial.printf("[dex] auth HTTP %d\n", code);
  if (code != 200) {
    if (isCredentialError(body)) { fatalCreds = true; Serial.println("[dex] IDENTIFIANTS REFUSES"); }
    return false;
  }
  accountId = firstQuoted(body);
  return accountId.length() == 36;
}

static bool dexLogin() {
  int code;
  String payload = String("{\"accountId\":\"") + accountId +
                   "\",\"password\":\"" + gCfg.pass +
                   "\",\"applicationId\":\"" + DEX_APP_ID + "\"}";
  String body = httpPost(String(dexBase()) + "/General/LoginPublisherAccountById",
                         payload, code);
  Serial.printf("[dex] login HTTP %d\n", code);
  if (code != 200) {
    if (isCredentialError(body)) { fatalCreds = true; Serial.println("[dex] IDENTIFIANTS REFUSES"); }
    return false;
  }
  sessionId = firstQuoted(body);
  if (sessionId == "00000000-0000-0000-0000-000000000000") { fatalCreds = true; return false; }
  return sessionId.length() == 36;
}

// retour : 0 = ok, 1 = erreur session (re-login possible), 2 = autre erreur
static int dexFetch() {
  int code;
  String url = String(dexBase()) +
               "/Publisher/ReadPublisherLatestGlucoseValues?sessionId=" + sessionId +
               "&minutes=30&maxCount=1";
  String body = httpPost(url, "", code);
  Serial.printf("[dex] fetch HTTP %d\n", code);
  if (code != 200) return isSessionError(body) ? 1 : 2;

  int vi = body.indexOf("\"Value\":");
  if (vi < 0) return 2;                  // tableau vide = pas de mesure recente
  mgdl = body.substring(vi + 8).toInt();

  int ti = body.indexOf("\"Trend\":");
  if (ti >= 0) {
    int end = body.indexOf(',', ti + 8);
    if (end < 0) end = body.indexOf('}', ti + 8);
    trendStr = body.substring(ti + 8, end);
    trendStr.replace("\"", "");
  }

  int wi = body.indexOf("Date(");
  if (wi >= 0) {
    // epoch ms -> on tronque aux 10 premiers chiffres (secondes)
    String num = body.substring(wi + 5, wi + 15);
    wtEpoch = (time_t) num.toInt();
  }
  Serial.printf("[dex] %d mg/dL, trend %s\n", mgdl, trendStr.c_str());
  return 0;
}

// Un cycle complet : session valide -> fetch ; sinon (re)login UNE fois.
// Jamais de boucle : protege le compte du verrouillage.
static bool dexUpdate() {
  if (fatalCreds) return false;
  if (gCfg.user.length() == 0 || gCfg.pass.length() == 0) {
    fatalCreds = true;                   // jamais configure -> portail
    Serial.println("[dex] aucun identifiant -> configuration requise");
    return false;
  }
  if (accountId.length() != 36 && !dexAuthenticate()) return false;
  if (sessionId.length() != 36 && !dexLogin()) return false;

  int r = dexFetch();
  if (r == 1) {                          // session expiree : UN seul retry
    sessionId = "";
    if (!dexLogin()) return false;
    r = dexFetch();
  }
  return r == 0;
}

// ------------------- Affichage LED -------------------
static float gDim = 1.0f;   // facteur applique par fillAll (mode nuit)

static void fillAll(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, (uint8_t)(r * gDim), (uint8_t)(g * gDim),
                        (uint8_t)(b * gDim));
  strip.show();
}

static void progressBar(int cur, int total) {   // barre bleue (OTA / update)
  int lit = total > 0 ? (int)(((long)cur * NUM_LEDS) / total) : 0;
  strip.clear();
  for (int i = 0; i < lit; i++) strip.setPixelColor(i, 0, 0, 60);
  strip.show();
}

static bool isNight() {
  time_t now = time(nullptr);
  if (now < 1600000000) return false;    // NTP pas encore synchro -> jour
  struct tm lt;
  localtime_r(&now, &lt);
  if (gCfg.nightStart == gCfg.nightEnd) return false;
  if (gCfg.nightStart > gCfg.nightEnd)   // plage a cheval sur minuit
    return lt.tm_hour >= gCfg.nightStart || lt.tm_hour < gCfg.nightEnd;
  return lt.tm_hour >= gCfg.nightStart && lt.tm_hour < gCfg.nightEnd;
}

static DisplayState currentState() {
  if (fatalCreds) return wm.getConfigPortalActive() ? ST_BADCREDS : ST_FATAL;
  if (wm.getConfigPortalActive()) return ST_CONFIG;
  if (WiFi.status() != WL_CONNECTED || failCount >= 2 || mgdl < 0) return ST_ERROR;
  time_t now = time(nullptr);
  bool timeOk = now > 1600000000;        // NTP synchronise
  if (timeOk && wtEpoch > 0 && (now - wtEpoch) > STALE_MIN * 60) return ST_STALE;
  if (mgdl < gCfg.hypo)   return ST_HYPO;
  if (mgdl > gCfg.hyper2) return ST_HYPER2;
  if (mgdl > gCfg.hyper)  return ST_HYPER;
  return ST_OK;
}

static void render(DisplayState st) {
  unsigned long t = millis();
  switch (st) {
    case ST_BOOT: {                      // point bleu tournant
      strip.clear();
      strip.setPixelColor((t / 120) % NUM_LEDS, 0, 0, 28);
      strip.show();
      break;
    }
    case ST_CONFIG: {                                   // violet respirant
      float phase = (t % 3000) / 3000.0f;
      float lvl = 0.5f * (1.0f - cosf(phase * 2.0f * PI));
      fillAll((uint8_t)(6 + 38 * lvl), 0, (uint8_t)(9 + 47 * lvl));
      break;
    }
    case ST_OK:    fillAll(0, 36, 5); break;            // vert doux
    case ST_HYPER: fillAll(66, 18, 0); break;           // orange
    case ST_HYPER2: {                    // gyrophare : bloc de 6 LED qui tourne
      int head = (int)((t / 125) % NUM_LEDS);           // 1 tour / 1,5 s
      strip.clear();
      for (int j = 0; j < 6; j++) {
        int i = (head + j) % NUM_LEDS;
        strip.setPixelColor(i, (uint8_t)(140 * gDim), (uint8_t)(38 * gDim), 0);
      }
      strip.show();
      break;
    }
    case ST_HYPO: {                       // ALARME : pulsation rouge pleine
      float phase = (t % 1200) / 1200.0f; // puissance, rapide, 0 -> 255
      float lvl = 0.5f * (1.0f - cosf(phase * 2.0f * PI));   // 0..1
      fillAll((uint8_t)(255 * lvl), 0, 0);
      break;
    }
    case ST_STALE:                       // blanc fixe = pas de donnees fiables
    case ST_ERROR:                       // (donnees perimees ou reseau coupe)
      fillAll(30, 30, 30);
      break;
    case ST_BADCREDS: {                  // violet/rouge : corriger au portail
      bool on = (t / 600) % 2;
      if (on) fillAll(40, 0, 50); else fillAll(60, 0, 0);
      break;
    }
    case ST_FATAL: {                     // rouge/bleu : identifiants, sans portail
      bool on = (t / 600) % 2;
      if (on) fillAll(47, 0, 0); else fillAll(0, 0, 47);
      break;
    }
  }
}

// ------------------- Portail de configuration -------------------
// Les WiFiManagerParameter doivent rester en scope : la lib ne garde que
// des pointeurs. Ils s'affichent sur la page WiFi -> un seul formulaire.
WiFiManagerParameter pSecDex("<hr><h3>Compte Dexcom</h3>");
WiFiManagerParameter pDexUser("dexuser", "Email Dexcom (compte publieur)", "", 64);
WiFiManagerParameter pDexPass("dexpass", "Mot de passe Dexcom", "", 64, "type='password'");
WiFiManagerParameter pDexUS("dexus", "Compte US (hors Europe)", "1", 2,
                            "type='checkbox'", WFM_LABEL_AFTER);
WiFiManagerParameter pSecSet("<hr><h3>Reglages</h3>");
WiFiManagerParameter pHypo("hypo", "Seuil hypo (mg/dL)", "70", 4,
                           "type='number' min='50' max='90'");
WiFiManagerParameter pHyper("hyper", "Seuil hyper (mg/dL)", "180", 4,
                            "type='number' min='120' max='250'");
WiFiManagerParameter pHyper2("hyper2", "Seuil gyrophare (mg/dL)", "300", 4,
                             "type='number' min='200' max='400'");
WiFiManagerParameter pNStart("nstart", "Nuit : debut (heure)", "22", 3,
                             "type='number' min='0' max='23'");
WiFiManagerParameter pNEnd("nend", "Nuit : fin (heure)", "7", 3,
                           "type='number' min='0' max='23'");
WiFiManagerParameter pNDim("ndim", "Nuit : luminosite (%)", "22", 4,
                           "type='number' min='5' max='100'");

// Reflete la config courante dans les champs (jamais le mot de passe).
static void syncPortalFields() {
  pDexUser.setValue(gCfg.user.c_str(), 64);
  pDexUS.setValue(gCfg.regionUS ? "1" : "", 2);
  pHypo.setValue(String(gCfg.hypo).c_str(), 4);
  pHyper.setValue(String(gCfg.hyper).c_str(), 4);
  pHyper2.setValue(String(gCfg.hyper2).c_str(), 4);
  pNStart.setValue(String(gCfg.nightStart).c_str(), 3);
  pNEnd.setValue(String(gCfg.nightEnd).c_str(), 3);
  pNDim.setValue(String(gCfg.nightDim).c_str(), 4);
}

// Appele de maniere synchrone par wm.process() au POST du formulaire.
static void onPortalSave() {
  // Reglages : toujours appliques (champs pre-remplis, donc jamais vides).
  gCfg.hypo       = clampInt(String(pHypo.getValue()).toInt(), 50, 90, gCfg.hypo);
  gCfg.hyper      = clampInt(String(pHyper.getValue()).toInt(), 120, 250, gCfg.hyper);
  gCfg.hyper2     = clampInt(String(pHyper2.getValue()).toInt(), 200, 400, gCfg.hyper2);
  gCfg.nightStart = clampInt(String(pNStart.getValue()).toInt(), 0, 23, gCfg.nightStart);
  gCfg.nightEnd   = clampInt(String(pNEnd.getValue()).toInt(), 0, 23, gCfg.nightEnd);
  gCfg.nightDim   = clampInt(String(pNDim.getValue()).toInt(), 5, 100, gCfg.nightDim);

  // Identifiants : seulement si les deux champs sont remplis, sinon on garde
  // ceux en place (cas "je ne change que le WiFi ou les seuils").
  String u = pDexUser.getValue(); u.trim();
  String p = pDexPass.getValue();
  bool credsChanged = false;
  if (u.length() > 0 && p.length() > 0) {
    // checkbox decochee = argument absent du POST
    bool us = wm.server && wm.server->hasArg("dexus");
    if (u != gCfg.user || p != gCfg.pass || us != gCfg.regionUS) credsChanged = true;
    gCfg.user = u;
    gCfg.pass = p;
    gCfg.regionUS = us;
  }

  saveConfig();
  Serial.printf("[cfg] sauvegarde (region %s, hypo %d, hyper %d/%d, nuit %dh-%dh %d%%)\n",
                gCfg.regionUS ? "us" : "ous", gCfg.hypo, gCfg.hyper, gCfg.hyper2,
                gCfg.nightStart, gCfg.nightEnd, gCfg.nightDim);

  if (credsChanged || (fatalCreds && gCfg.user.length() && gCfg.pass.length())) {
    // Nouveaux identifiants -> on autorise UNE nouvelle tentative.
    // C'est le SEUL chemin qui leve fatalCreds : une action humaine = un
    // essai, jamais de boucle sur des identifiants refuses (anti-lock).
    fatalCreds = false;
    accountId = "";
    sessionId = "";
    failCount = 0;
    pollInterval = POLL_BASE_MS;
    gCredsJustSaved = true;
    Serial.println("[cfg] nouveaux identifiants -> une tentative autorisee");
  }
  syncPortalFields();
}

// ------------------- Mise a jour automatique -------------------
// Tailles de buffer TLS mesurees sur objects.githubusercontent.com :
// il n'annonce pas de max_fragment_length et envoie des enregistrements
// pleine taille pour les gros transferts. Un buffer de reception de 4 Ko
// suffit pour une petite reponse (version.txt) mais renvoie ZERO octet sur
// le telechargement du binaire : celui-ci exige 16 Ko.
#define TLS_RX_SMALL   4096
#define TLS_RX_BULK    16384
#define TLS_TX         512
// Marge de heap exigee avant une mise a jour (buffer 16 Ko + HTTPClient).
#define UPDATE_MIN_HEAP 28000
#define UPDATE_MAX_TRIES 3     // essais par version avant abandon

// Recupere le contenu de version.txt sur GitHub. "" si echec.
static String fetchRemoteVersion() {
  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(TLS_RX_SMALL, TLS_TX);
  HTTPClient http;
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // github -> objects.*
  if (!http.begin(client, UPDATE_VERSION_URL)) return "";
  int code = http.GET();
  String v = (code == 200) ? http.getString() : "";
  http.end();
  v.trim();
  Serial.printf("[upd] distante '%s' (HTTP %d) | locale '%s'\n",
                v.c_str(), code, FW_VERSION);
  return v;
}

// Telechargement + flash (bloquant ~30-60 s, reboot automatique si OK).
static void doFirmwareUpdate(const String& remoteVer, int tries) {
  // Ecrit AVANT le flash : si la carte n'en revient pas, on saura au boot
  // suivant que cette version a deja ete tentee (anti-boucle de brique).
  writeLineFile(UPDATE_TRY_PATH, remoteVer + " " + String(tries + 1));

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(TLS_RX_BULK, TLS_TX);   // 16 Ko : indispensable ici
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  ESPhttpUpdate.onStart([]() {
    Serial.println("[upd] telechargement et flash...");
    strip.clear();
    strip.show();
  });
  ESPhttpUpdate.onProgress([](int cur, int total) { progressBar(cur, total); });
  ESPhttpUpdate.onError([](int e) { Serial.printf("[upd] erreur %d\n", e); });

  t_httpUpdate_return r = ESPhttpUpdate.update(client, UPDATE_FW_URL);
  // Attention : HTTP_UPDATE_FAILED vaut 0, comparer aux enums.
  if (r == HTTP_UPDATE_FAILED)
    Serial.printf("[upd] ECHEC (%d) : %s\n", ESPhttpUpdate.getLastError(),
                  ESPhttpUpdate.getLastErrorString().c_str());
  else if (r == HTTP_UPDATE_NO_UPDATES)
    Serial.println("[upd] rien a mettre a jour");
  // HTTP_UPDATE_OK : la carte a redemarre, on ne repasse pas ici.
}

// Retourne true si la verification a pu aller au bout (peu importe qu'une
// mise a jour ait ete trouvee), false si elle a ete reportee.
static bool checkAutoUpdate() {
  if (WiFi.status() != WL_CONNECTED) return false;
  // Portail ouvert AVEC quelqu'un dessus : ne pas couper la configuration.
  // (Une lampe seule en mode correctif doit pouvoir se reparer la nuit.)
  if (wm.getConfigPortalActive() && WiFi.softAPgetStationNum() > 0) return false;
  if (mgdl >= 0 && mgdl < gCfg.hypo) return false;   // jamais pendant une alarme
  if (ESP.getFreeHeap() < UPDATE_MIN_HEAP) {
    Serial.printf("[upd] heap %u trop bas, report\n", ESP.getFreeHeap());
    return false;
  }
  String remote = fetchRemoteVersion();
  if (remote.length() == 0) return false;
  // Inegalite stricte (et non ">") : permet de revenir en arriere en
  // republiant une version anterieure. Pas de ping-pong : apres flash les
  // versions sont egales.
  if (remote == FW_VERSION) return true;

  // Suivi des tentatives : "<version> <n>". Un echec reseau passager doit
  // pouvoir etre retente, mais un binaire durablement mauvais ne doit pas
  // boucler indefiniment.
  String tried = readLineFile(UPDATE_TRY_PATH);
  int tries = 0;
  int sp = tried.indexOf(' ');
  if (sp > 0 && tried.substring(0, sp) == remote) tries = tried.substring(sp + 1).toInt();
  else if (tried == remote) tries = 1;              // ancien format
  if (tries >= UPDATE_MAX_TRIES) {
    Serial.printf("[upd] v%s echouee %d fois, abandonnee\n", remote.c_str(), tries);
    return true;
  }
  Serial.printf("[upd] %s -> %s (essai %d)\n", FW_VERSION, remote.c_str(), tries + 1);
  doFirmwareUpdate(remote, tries);
  return true;
}

// ------------------- Setup / Loop -------------------
bool booting = true;

void setup() {
  Serial.begin(115200);
  Serial.println("\n[veilleuse] boot");
  buildIdentity();

  strip.begin();
  strip.setBrightness(255);   // l'intensite est portee par chaque etat
  strip.clear();
  strip.show();

  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  WiFi.hostname(gHostname);
  LittleFS.begin();

  // Mise a jour precedente : succes confirme si la version tentee est celle
  // qui tourne maintenant (fichier "<version> <essais>").
  String tried = readLineFile(UPDATE_TRY_PATH);
  int sp = tried.indexOf(' ');
  if (tried.length() && (tried == FW_VERSION ||
                         (sp > 0 && tried.substring(0, sp) == FW_VERSION))) {
    LittleFS.remove(UPDATE_TRY_PATH);
    Serial.printf("[upd] mise a jour vers %s confirmee\n", FW_VERSION);
  }

  if (loadConfig()) {
    Serial.printf("[cfg] config.cfg charge (region %s)\n", gCfg.regionUS ? "us" : "ous");
  } else if (gCfg.user.length() && gCfg.pass.length()) {
    Serial.println("[cfg] pas de config -> identifiants de secrets.h (dev)");
  } else {
    Serial.println("[cfg] AUCUN identifiant -> configuration par le portail");
  }
  syncPortalFields();

  int known = loadWifiList();
#ifdef WIFI_SSID
  if (strlen(WIFI_SSID) > 0) {           // seed optionnel depuis secrets.h
    wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
    known++;
  }
#endif
  Serial.printf("[wifi] %d reseau(x) connu(s)\n", known);

  wm.setConfigPortalBlocking(false);     // portail non bloquant (animations)
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(180);
  wm.setHostname(gHostname);
  wm.addParameter(&pSecDex);
  wm.addParameter(&pDexUser);
  wm.addParameter(&pDexPass);
  wm.addParameter(&pDexUS);
  wm.addParameter(&pSecSet);
  wm.addParameter(&pHypo);
  wm.addParameter(&pHyper);
  wm.addParameter(&pHyper2);
  wm.addParameter(&pNStart);
  wm.addParameter(&pNEnd);
  wm.addParameter(&pNDim);
  wm.setSaveParamsCallback(onPortalSave);

  if (known > 0) {
    Serial.println("[wifi] connexion au meilleur reseau connu...");
    wifiMulti.run(15000);                // scanne et prend le plus fort
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] portail de configuration actif");
    wm.startConfigPortal(gApName);
  }

  // NTP avec fuseau Europe/Paris (ete/hiver automatique)
  configTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
  wifiRetryAt = millis() + 240000UL;

  // OTA local : mise a jour par WiFi depuis le poste de dev
  // (pio run -e d1_mini_ota -t upload --upload-port <hostname>.local)
  ArduinoOTA.setHostname(gHostname);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println("[ota] mise a jour...");
    strip.clear();
    strip.show();
  });
  ArduinoOTA.onProgress([](unsigned int prog, unsigned int total) {
    progressBar((int)prog, (int)total);
  });
  ArduinoOTA.onEnd([]() { Serial.println("[ota] termine, reboot"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] erreur %u\n", e); });
  ArduinoOTA.begin();
  // Publie la version en mDNS : permet de savoir ce que tourne chaque lampe
  // du reseau sans la debrancher (dns-sd -L <hostname> _arduino._tcp local).
  MDNS.addServiceTxt("arduino", "tcp", "fw", FW_VERSION);
}

void loop() {
  ArduinoOTA.handle();

  // Animation (30 fps environ)
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame >= 33) {
    lastFrame = millis();
#if DEMO_MODE
    static const DisplayState demoSeq[] = {ST_OK, ST_HYPER, ST_HYPER2, ST_HYPO,
                                           ST_STALE, ST_ERROR, ST_CONFIG,
                                           ST_BADCREDS, ST_FATAL};
    static const char* demoNames[] = {"VERT (dans la plage)", "ORANGE (hyper)",
        "GYROPHARE ORANGE (hyper severe)", "ROUGE PULSE (hypo)",
        "BLANC (donnees perimees)", "BLANC (erreur reseau)",
        "VIOLET (config WiFi)", "VIOLET/ROUGE (identifiants a corriger)",
        "ROUGE/BLEU (identifiants, sans portail)"};
    int idx = (int)((millis() / 6000) % 9);
    static int lastIdx = -1;
    if (idx != lastIdx) { lastIdx = idx; Serial.printf("[demo] %s\n", demoNames[idx]); }
    render(demoSeq[idx]);
#else
    if (booting && !wm.getConfigPortalActive()) {
      render(ST_BOOT);                   // point bleu tournant : demarrage
    } else {
      DisplayState st = currentState();
      gDim = (isNight() && st != ST_HYPO) ? (gCfg.nightDim / 100.0f) : 1.0f;
      render(st);
    }
#endif
  }

  // Identifiants refuses ou absents : ouvrir le portail pour les corriger
  // (sans timeout) au lieu de rester bloque jusqu'a un reflash.
  if (fatalCreds && !wm.getConfigPortalActive()) {
    Serial.println("[dex] identifiants a corriger -> portail ouvert");
    wm.setConfigPortalTimeout(0);
    wm.startConfigPortal(gApName);
    gFatalPortal = true;
  }

  // Gestion WiFi : memoire multi-reseaux + portail captif
  static bool wasConnected = false;
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !wasConnected) {
    Serial.printf("[wifi] connecte a %s : %s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    rememberWifi(WiFi.SSID(), WiFi.psk());   // tout reseau rejoint est memorise
    // On ne ferme pas le portail correctif : il attend de nouveaux identifiants.
    if (wm.getConfigPortalActive() && !gFatalPortal) wm.stopConfigPortal();
  }
  wasConnected = connected;

  // Le portail doit tourner meme WiFi connecte (mode correctif en AP_STA).
  if (wm.getConfigPortalActive()) {
    wm.process();
  } else if (!connected && (long)(millis() - wifiRetryAt) >= 0) {
    Serial.println("[wifi] reconnexion aux reseaux connus...");
    if (wifiMulti.run(12000) != WL_CONNECTED) {
      Serial.println("[wifi] echec -> portail 3 min");
      wm.startConfigPortal(gApName);
    }
    wifiRetryAt = millis() + 240000UL;
  }

  // Nouveaux identifiants sauves : fermer le portail AVANT le retry TLS
  // (le softAP + serveur web mangent la RAM necessaire au handshake).
  if (gCredsJustSaved) {
    gCredsJustSaved = false;
    if (gFatalPortal) {
      wm.stopConfigPortal();
      wm.setConfigPortalTimeout(180);
      gFatalPortal = false;
    }
    nextPollAt = millis() + 3000;        // laisser le STA se (re)connecter
    booting = false;
  }

  // Attente du WiFi au demarrage
  if (booting) {
    if (connected) {
      booting = false;
      nextPollAt = millis();             // premier poll immediat
    }
    return;
  }

  // Poll Dexcom
#if DEMO_MODE
  return;                                // pas de requetes pendant la demo
#endif
  if ((long)(millis() - nextPollAt) >= 0 && !fatalCreds) {
    if (WiFi.status() != WL_CONNECTED) {
      failCount++;
      pollInterval = min(pollInterval * 2, POLL_MAX_MS);
    } else if (dexUpdate()) {
      failCount = 0;
      pollInterval = POLL_BASE_MS;       // retour a la normale
      // Check MAJ apres le 1er poll reussi. S'il a ete reporte (heap,
      // portail...), on retentera au poll suivant plutot que d'attendre 4 h.
      if (!gBootCheckDone) gBootCheckDone = checkAutoUpdate();
    } else {
      failCount++;
      pollInterval = min(pollInterval * 2, POLL_MAX_MS);  // backoff 60s -> 5min
      Serial.printf("[dex] echec #%d, prochain essai dans %lus\n",
                    failCount, pollInterval / 1000);
    }
    nextPollAt = millis() + pollInterval;
  }

  // Check quotidien des mises a jour, vers 4 h locale. La minute depend du
  // chipid : etale les telechargements du parc sur une heure.
  static unsigned long lastClockCheck = 0;
  if (millis() - lastClockCheck >= 1000) {
    lastClockCheck = millis();
    time_t now = time(nullptr);
    if (now > 1600000000) {
      struct tm lt;
      localtime_r(&now, &lt);
      int targetMin = (int)(ESP.getChipId() % 60);
      if (lt.tm_hour == UPDATE_HOUR && lt.tm_min == targetMin &&
          lt.tm_yday != gLastCheckYday) {
        gLastCheckYday = lt.tm_yday;     // une seule fois par jour
        checkAutoUpdate();
      }
    }
  }
}
