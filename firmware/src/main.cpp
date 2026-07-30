// ============================================================
// Veilleuse Dexcom — ESP8266 (Wemos D1 mini) + ruban SK6812/WS2812B
// WiFi + API Dexcom Share directe + poll 60 s + couleurs.
//
// Câblage : DIN ruban -> D1 (GPIO5), 5V -> 5V, GND -> G.
//           (+ résistance ~330 ohms en série sur DIN,
//            condensateur 470-1000 uF entre 5V et GND)
//
// Configuration : page web embarquée (web/veilleuse-config.html, gzippée en
// PROGMEM). Elle sert de portail captif au premier démarrage — le réseau
// "Veilleuse-XXXX" est ouvert et la page s'ouvre d'elle-même sur le téléphone —
// puis reste accessible en permanence sur le réseau local à
// http://veilleuse-<chipid>.local/. Tout est persisté en LittleFS ;
// secrets.h ne sert qu'au développement.
//
// Codes couleur :
//   vert doux fixe      = dans la plage
//   orange fixe         = hyper (> seuil hyper)
//   gyrophare orange    = hyper severe (> seuil hyper2) : rotation, pas plus
//                         de lumiere, juste du mouvement
//   rouge "respiration" = hypo  (< seuil hypo)
//   blanc doux fixe     = pas de données fiables (périmées > STALE_MIN,
//                         WiFi coupé ou Dexcom injoignable)
//   violet respirant    = portail de configuration ouvert
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
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <ESP8266mDNS.h>
#include <ESP8266httpUpdate.h>
#include <LittleFS.h>
#include <time.h>
#include "secrets.h"
#include "web_page.h"          // PAGE_GZ, PAGE_GZ_LEN, PAGE_ETAG (genere)

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
#define FW_VERSION     "1.2.3"

#ifndef UPDATE_BASE_URL                     // surchargeable par build_flags
#define UPDATE_BASE_URL "https://github.com/didux123/veilleuse-dexcom/releases/latest/download"
#endif
static const char* UPDATE_VERSION_URL = UPDATE_BASE_URL "/version.txt";
static const char* UPDATE_FW_URL      = UPDATE_BASE_URL "/firmware.bin";
#define UPDATE_TRY_PATH "/update.txt"
#define UPDATE_HOUR     4                   // check quotidien vers 4 h locale

// Tailles de buffer TLS mesurees sur objects.githubusercontent.com :
// il n'annonce pas de max_fragment_length et envoie des enregistrements
// pleine taille pour les gros transferts. Un buffer de reception de 4 Ko
// suffit pour une petite reponse (version.txt) mais renvoie ZERO octet sur
// le telechargement du binaire : celui-ci exige 16 Ko.
#define TLS_RX_SMALL   4096
#define TLS_RX_BULK    16384
#define TLS_TX         512
// Marge de heap exigee avant une mise a jour (buffer 16 Ko + serveur web).
#define UPDATE_MIN_HEAP 30000
#define UPDATE_MAX_TRIES 3     // essais par version avant abandon

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
ESP8266WiFiMulti  wifiMulti;
ESP8266WebServer  server(80);
DNSServer         dns;

static bool          gPortal       = false;  // softAP + DNS actifs
static bool          gPortalForced = false;  // /api/portal?on=1 (diagnostic)
static unsigned long gStaLostAt    = 0;      // STA tombee depuis (debounce)
static unsigned long gRebootAt     = 0;      // 0 = pas de reboot programme
static bool          gForgetWifi   = false;  // oublier le reseau au reboot
// Configuration appliquee a chaud depuis le portail : on attend de savoir si
// le WiFi et le compte Dexcom repondent avant de redemarrer (voir handleSave).
static bool          gPending      = false;
static unsigned long gPendingSince = 0;
static unsigned long gLastHttpAt   = 0;      // derniere requete servie
static unsigned long wifiRetryAt   = 0;
static bool          gConnecting   = false;  // wifiMulti.run() en cours
static int           gKnownWifi    = 0;
static String        gScanJson;              // dernier scan pre-serialise
static unsigned long gScanDoneAt   = 0;
static uint16_t      gLoopsPerSec  = 0;

// ------------------- Journal circulaire -------------------
// Remplace le port serie (absent) : lisible par GET /api/log.
// Ne JAMAIS y ecrire un mot de passe.
#define LOG_SIZE 1024
static char     gLog[LOG_SIZE];
static uint16_t gLogHead = 0;
static bool     gLogWrap = false;

static void logPut(const char* s) {
  while (*s) {
    gLog[gLogHead++] = *s++;
    if (gLogHead >= LOG_SIZE) { gLogHead = 0; gLogWrap = true; }
  }
}

static void logf(const char* fmt, ...) {
  char b[160];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  Serial.print(b);
  logPut(b);
}

// ------------------- Identite (unique par puce) -------------------
static char gHostname[20];   // "veilleuse-a1b2c3" (mDNS / OTA)
static char gApName[16];     // "Veilleuse-B2C3"   (SSID du portail)

static void buildIdentity() {
  uint32_t id = ESP.getChipId() & 0xFFFFFF;
  snprintf(gHostname, sizeof(gHostname), "veilleuse-%06x", id);
  snprintf(gApName, sizeof(gApName), "Veilleuse-%04X", (unsigned)(id & 0xFFFF));
  logf("[id] chipid %06X | hostname %s | AP %s | fw %s\n",
       id, gHostname, gApName, FW_VERSION);
}

// ------------------- Configuration (LittleFS) -------------------
// /config.cfg : "cle=valeur" par ligne, decoupe sur le PREMIER '=' (le mot
// de passe peut en contenir). ver=2 : les horaires de nuit sont en MINUTES
// depuis minuit (la page envoie du HH:MM).
#define CFG_PATH "/config.cfg"
#define CFG_VER  2

struct Config {
  String user     = DEXCOM_USERNAME;   // fallback dev depuis secrets.h
  String pass     = DEXCOM_PASSWORD;
  bool   regionUS = false;             // false = "ous" (Europe)
  int    hypo     = 70;                // mg/dL
  int    hyper    = 180;
  int    hyper2   = 300;               // gyrophare
  int    nightStart = 22 * 60;         // minutes depuis minuit
  int    nightEnd   =  7 * 60;
  int    nightDim   = 22;              // luminosite nuit, en %
  bool   otaAuto    = true;            // mises a jour automatiques
};
static Config gCfg;
static bool   gCfgMigrated = false;

static int clampInt(int v, int lo, int hi, int def) {
  return (v < lo || v > hi) ? def : v;
}

static String hhmm(int m) {
  if (m < 0 || m > 1439) m = 0;
  char b[12];                       // large : evite un avertissement de troncature
  snprintf(b, sizeof(b), "%02d:%02d", m / 60, m % 60);
  return String(b);
}

static int parseHHMM(const String& s, int def) {
  int c = s.indexOf(':');
  if (c < 1) return def;
  int h = s.substring(0, c).toInt();
  int m = s.substring(c + 1, c + 3).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return def;
  return h * 60 + m;
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
  int ver = 1;                    // absence de "ver" = ancien format (heures)
  int nStart = -1, nEnd = -1;     // valeurs brutes, interpretees apres la boucle
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String k = line.substring(0, eq);
    String v = line.substring(eq + 1);
    int n = v.toInt();
    if      (k == "ver")    ver = n;
    else if (k == "user")   gCfg.user = v;
    else if (k == "pass")   gCfg.pass = v;
    else if (k == "region") gCfg.regionUS = (v == "us");
    else if (k == "hypo")   gCfg.hypo   = clampInt(n, 40, 400, 70);
    else if (k == "hyper")  gCfg.hyper  = clampInt(n, 40, 400, 180);
    else if (k == "hyper2") gCfg.hyper2 = clampInt(n, 40, 400, 300);
    else if (k == "nstart") nStart = n;
    else if (k == "nend")   nEnd   = n;
    else if (k == "ndim")   gCfg.nightDim = clampInt(n, 5, 100, 22);
    else if (k == "ota")    gCfg.otaAuto  = (n != 0);
  }
  f.close();

  // v1 -> v2 : nstart/nend etaient des HEURES (0-23) et deviennent des
  // MINUTES. Impossible de deviner d'apres la valeur (22 est valide dans les
  // deux) : c'est le marqueur "ver" qui tranche.
  if (ver < CFG_VER) {
    if (nStart >= 0) gCfg.nightStart = clampInt(nStart, 0, 23, 22) * 60;
    if (nEnd   >= 0) gCfg.nightEnd   = clampInt(nEnd,   0, 23,  7) * 60;
    gCfgMigrated = true;
  } else {
    if (nStart >= 0) gCfg.nightStart = clampInt(nStart, 0, 1439, 22 * 60);
    if (nEnd   >= 0) gCfg.nightEnd   = clampInt(nEnd,   0, 1439,  7 * 60);
  }
  // Une config abimee ne doit pas produire hypo > hyper (l'etat HYPO
  // l'emporterait en permanence).
  if (!(gCfg.hypo < gCfg.hyper && gCfg.hyper < gCfg.hyper2)) {
    gCfg.hypo = 70; gCfg.hyper = 180; gCfg.hyper2 = 300;
  }
  return gCfg.user.length() > 0 && gCfg.pass.length() > 0;
}

static bool saveConfig() {
  File w = LittleFS.open(CFG_PATH, "w");
  if (!w) { logf("[cfg] ECHEC ecriture config\n"); return false; }
  w.print("ver=" + String(CFG_VER) + "\n");
  w.print("user=" + gCfg.user + "\n");
  w.print("pass=" + gCfg.pass + "\n");
  w.print(String("region=") + (gCfg.regionUS ? "us" : "ous") + "\n");
  w.print("hypo="   + String(gCfg.hypo)   + "\n");
  w.print("hyper="  + String(gCfg.hyper)  + "\n");
  w.print("hyper2=" + String(gCfg.hyper2) + "\n");
  w.print("nstart=" + String(gCfg.nightStart) + "\n");
  w.print("nend="   + String(gCfg.nightEnd)   + "\n");
  w.print("ndim="   + String(gCfg.nightDim)   + "\n");
  w.print(String("ota=") + (gCfg.otaAuto ? "1" : "0") + "\n");
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
  if (!w) { logf("[wifi] ECHEC ecriture liste\n"); return; }
  w.print(ssid + "\t" + pass + "\n");                 // plus recent en tete
  w.print(keep);
  w.close();
  wifiMulti.addAP(ssid.c_str(), pass.length() ? pass.c_str() : nullptr);
  logf("[wifi] reseau memorise : %s (%d en liste)\n", ssid.c_str(), count);
}

// Mot de passe deja connu pour ce SSID ("" si inconnu). Sert au cas
// "l'utilisateur enregistre sans retaper le mot de passe du WiFi".
static String lookupWifiPass(const String& ssid) {
  File f = LittleFS.open("/wifis.txt", "r");
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.replace("\r", "");
      int tab = line.indexOf('\t');
      if (tab <= 0) continue;
      if (line.substring(0, tab) == ssid) { f.close(); return line.substring(tab + 1); }
    }
    f.close();
  }
  if (WiFi.isConnected() && WiFi.SSID() == ssid) return WiFi.psk();
  return "";
}

static String firstKnownSsid() {
  File f = LittleFS.open("/wifis.txt", "r");
  if (!f) return "";
  String line = f.readStringUntil('\n');
  f.close();
  line.replace("\r", "");
  int tab = line.indexOf('\t');
  return tab > 0 ? line.substring(0, tab) : "";
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

static bool gBootCheckDone  = false;   // check de mise a jour au demarrage
static int  gLastCheckYday  = -1;      // dernier jour du check quotidien

// ------------------- Utilitaires HTTP/JSON -------------------
static String httpPost(const String& url, const String& payload, int& codeOut) {
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();                 // compromis assumé (pas de vérif cert)
  client->setBufferSizes(TLS_RX_SMALL, 1024);
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
  logf("[dex] auth HTTP %d\n", code);
  if (code != 200) {
    if (isCredentialError(body)) { fatalCreds = true; logf("[dex] IDENTIFIANTS REFUSES\n"); }
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
  logf("[dex] login HTTP %d\n", code);
  if (code != 200) {
    if (isCredentialError(body)) { fatalCreds = true; logf("[dex] IDENTIFIANTS REFUSES\n"); }
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
  logf("[dex] fetch HTTP %d\n", code);
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
  logf("[dex] %d mg/dL, trend %s\n", mgdl, trendStr.c_str());
  return 0;
}

// Verifie un couple email/mot de passe sans toucher a l'etat courant, pour
// pouvoir repondre a l'utilisateur pendant qu'il est encore sur la page.
// La premiere etape de l'API suffit : elle rejette explicitement un mauvais
// mot de passe. UNE seule tentative par appel (regle anti-verrouillage).
// Retour : 1 = valides, 0 = refuses, -1 = indetermine (reseau ou Dexcom KO).
static int dexProbe(const String& user, const String& pass, bool us) {
  const char* base = us ? "https://share2.dexcom.com/ShareWebServices/Services"
                        : "https://shareous1.dexcom.com/ShareWebServices/Services";
  int code;
  String payload = String("{\"accountName\":\"") + user +
                   "\",\"password\":\"" + pass +
                   "\",\"applicationId\":\"" + DEX_APP_ID + "\"}";
  String body = httpPost(String(base) + "/General/AuthenticatePublisherAccount",
                         payload, code);
  logf("[dex] verification des identifiants : HTTP %d\n", code);
  if (code == 200) return firstQuoted(body).length() == 36 ? 1 : -1;
  return isCredentialError(body) ? 0 : -1;
}

// Un cycle complet : session valide -> fetch ; sinon (re)login UNE fois.
// Jamais de boucle : protege le compte du verrouillage.
static bool dexUpdate() {
  if (fatalCreds) return false;
  if (gCfg.user.length() == 0 || gCfg.pass.length() == 0) {
    fatalCreds = true;                   // jamais configure -> portail
    logf("[dex] aucun identifiant -> configuration requise\n");
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
  int cur = lt.tm_hour * 60 + lt.tm_min;
  if (gCfg.nightStart == gCfg.nightEnd) return false;
  if (gCfg.nightStart > gCfg.nightEnd)   // plage a cheval sur minuit
    return cur >= gCfg.nightStart || cur < gCfg.nightEnd;
  return cur >= gCfg.nightStart && cur < gCfg.nightEnd;
}

static DisplayState currentState() {
  if (fatalCreds) return gPortal ? ST_BADCREDS : ST_FATAL;
  if (gPortal) return ST_CONFIG;
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

// Codes attendus par la page (memes noms que les etats LED).
static const char* stateCode(DisplayState st) {
  switch (st) {
    case ST_OK:      return "ok";
    case ST_HYPER:   return "hi";
    case ST_HYPER2:  return "hs";
    case ST_HYPO:    return "lo";
    case ST_STALE:
    case ST_ERROR:   return "un";
    case ST_CONFIG:  return "su";
    case ST_BOOT:    return "bo";
    default:         return "er";        // ST_BADCREDS, ST_FATAL
  }
}

static const char* stateLabel(DisplayState st) {
  switch (st) {
    case ST_OK:      return "Dans la plage";
    case ST_HYPER:   return "Au-dessus du seuil";
    case ST_HYPER2:  return "Hyper sévère";
    case ST_HYPO:    return "Hypo";
    case ST_STALE:   return "Données périmées";
    case ST_ERROR:   return "Réseau ou capteur injoignable";
    case ST_CONFIG:  return "En attente de configuration";
    case ST_BOOT:    return "Démarrage";
    default:         return "Identifiants Dexcom refusés";
  }
}

// Dexcom renvoie tantot le nom de la tendance, tantot son index.
static const char* trendArrow() {
  static const char* NAMES[] = {"None","DoubleUp","SingleUp","FortyFiveUp","Flat",
                                "FortyFiveDown","SingleDown","DoubleDown",
                                "NotComputable","RateOutOfRange"};
  static const char* ARROWS[] = {"", "↑↑","↑","↗","→","↘","↓","↓↓","",""};
  if (trendStr.length() == 0) return "";
  int idx = -1;
  if (isDigit(trendStr[0])) idx = trendStr.toInt();
  else for (int i = 0; i < 10; i++) if (trendStr == NAMES[i]) { idx = i; break; }
  return (idx >= 0 && idx < 10) ? ARROWS[idx] : "";
}

// ------------------- Serialisation JSON -------------------
// Petits objets (< 500 o) : String avec reserve() pour ne pas fragmenter.
static void jsonEsc(String& out, const String& v) {
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if (c == '"' || c == '\\')  { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
    else                        { out += c; }        // UTF-8 passe tel quel
  }
}
static void jsonS(String& out, const char* k, const String& v) {
  out += '"'; out += k; out += F("\":\""); jsonEsc(out, v); out += F("\",");
}
static void jsonI(String& out, const char* k, long v) {
  out += '"'; out += k; out += F("\":"); out += v; out += ',';
}
static void jsonB(String& out, const char* k, bool v) {
  out += '"'; out += k; out += F("\":"); out += (v ? F("true") : F("false")); out += ',';
}
static void jsonEnd(String& out) {
  if (out.endsWith(",")) out.remove(out.length() - 1);
  out += '}';
}
static void sendJson(const String& body) {
  gLastHttpAt = millis();
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), body);
}

// ------------------- Scan WiFi asynchrone -------------------
// Un scan synchrone appelle esp_suspend() et figerait loop() (serveur web et
// LED compris) : on ne fait donc que de l'asynchrone + polling.
static bool canScanNow() {
  if (gConnecting) return false;
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return false;
  // Le scan appelle wifi_station_disconnect() si la STA n'est ni GOT_IP ni
  // IDLE : ne jamais scanner pendant une tentative de connexion.
  station_status_t s = wifi_station_get_connect_status();
  return (s == STATION_GOT_IP || s == STATION_IDLE);
}

// Tri par RSSI decroissant + dedoublonnage, 8 max, sans allocation.
static void buildScanJson(int n) {
  if (n > 32) n = 32;
  uint32_t used = 0;
  String o; o.reserve(512); o = F("{\"networks\":[");
  int emitted = 0;
  for (int k = 0; k < 8; k++) {
    int best = -1;
    for (int i = 0; i < n; i++) {
      if (used & (1UL << i)) continue;
      if (WiFi.SSID(i).length() == 0) { used |= 1UL << i; continue; }  // masque
      if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    }
    if (best < 0) break;
    String s = WiFi.SSID(best);
    used |= 1UL << best;
    for (int i = 0; i < n; i++)                    // repeteurs / doubles bandes
      if (!(used & (1UL << i)) && WiFi.SSID(i) == s) used |= 1UL << i;
    if (emitted++) o += ',';
    o += F("{\"ssid\":\""); jsonEsc(o, s);
    o += F("\",\"rssi\":"); o += WiFi.RSSI(best); o += '}';
  }
  o += F("]}");
  gScanJson = o;
  WiFi.scanDelete();      // ~110 o par reseau : liberer tout de suite
}

static void pollScan() {
  int n = WiFi.scanComplete();
  if (n < 0) return;                    // -1 en cours, -2 non declenche
  buildScanJson(n);
  gScanDoneAt = millis();
  logf("[scan] %d reseaux, json %u o\n", n, (unsigned)gScanJson.length());
}

// ------------------- Handlers HTTP -------------------
static void handleRoot() {
  gLastHttpAt = millis();
  // sendHeader AVANT send_P : les en-tetes sont vides apres chaque envoi.
  server.sendHeader(F("Content-Encoding"), F("gzip"));
  server.sendHeader(F("Cache-Control"), F("no-cache"));
  // send_P gere PROGMEM (StreamConstPtr detecte l'adresse flash) et pose
  // lui-meme Content-Length : ne jamais le faire a la main.
  server.send_P(200, PSTR("text/html"), (PGM_P)PAGE_GZ, PAGE_GZ_LEN);
}

static void handleStatus() {
  DisplayState st = currentState();
  time_t now = time(nullptr);
  long age = 0;
  if (now > 1600000000 && wtEpoch > 0) {
    age = (long)(now - wtEpoch);
    if (age < 0) age = 0;
  }
  String o; o.reserve(224); o = '{';
  jsonS(o, "state", stateCode(st));
  jsonS(o, "label", stateLabel(st));
  jsonI(o, "mgdl", mgdl < 0 ? 0 : mgdl);   // la page teste d.mgdl : 0 = rien
  jsonS(o, "trend", trendArrow());
  jsonI(o, "age",  age);
  jsonS(o, "fw",   FW_VERSION);
  // Permettent a la page de distinguer, apres une premiere configuration,
  // "ca connecte encore", "WiFi refuse" et "Dexcom refuse".
  jsonB(o, "wifi", WiFi.isConnected());
  jsonB(o, "pending", gPending);
  jsonS(o, "host", String(gHostname) + ".local");
  jsonEnd(o);
  sendJson(o);
}

static void handleConfigGet() {
  String o; o.reserve(320); o = '{';
  bool credsOk = gCfg.user.length() && gCfg.pass.length();
  // configured=false fait demarrer la page a l'accueil (WiFi puis Dexcom) :
  // c'est ce qu'il faut aussi bien pour une lampe neuve que pour une lampe
  // qui a perdu son reseau ou dont les identifiants sont refuses.
  jsonB(o, "configured", credsOk && WiFi.isConnected() && !fatalCreds);
  jsonS(o, "ssid",   WiFi.isConnected() ? WiFi.SSID() : firstKnownSsid());
  jsonS(o, "region", gCfg.regionUS ? "us" : "eu");   // firmware "ous" = page "eu"
  jsonS(o, "email",  gCfg.user);                     // JAMAIS de mot de passe
  jsonI(o, "lo",  gCfg.hypo);
  jsonI(o, "hi",  gCfg.hyper);
  jsonI(o, "sev", gCfg.hyper2);
  jsonS(o, "nightOn",  hhmm(gCfg.nightStart));       // <input type=time> exige
  jsonS(o, "nightOff", hhmm(gCfg.nightEnd));         // le format HH:MM
  jsonI(o, "dim", gCfg.nightDim);
  jsonB(o, "ota", gCfg.otaAuto);
  jsonS(o, "host", String(gHostname) + ".local");
  jsonEnd(o);
  sendJson(o);
}

static void handleScan() {
  gLastHttpAt = millis();
  // Reponse immediate depuis le cache, puis relance en tache de fond.
  sendJson(gScanJson.length() ? gScanJson : String(F("{\"networks\":[]}")));
  if (canScanNow() && (gScanDoneAt == 0 || millis() - gScanDoneAt > 10000))
    WiFi.scanNetworks(true, false);
}

static void handleSave() {
  gLastHttpAt = millis();
  String ssid = server.arg("ssid");  ssid.trim();
  String wpw  = server.arg("wpw");
  String mail = server.arg("email"); mail.trim();
  String dpw  = server.arg("dpw");
  bool   us   = (server.arg("region") == "us");
  int lo  = server.arg("lo").toInt();
  int hi  = server.arg("hi").toInt();
  int sev = server.arg("sev").toInt();
  int dim = server.arg("dim").toInt();
  bool ota = (server.arg("ota") == "1");
  int nOn  = parseHHMM(server.arg("nightOn"),  gCfg.nightStart);
  int nOff = parseHHMM(server.arg("nightOff"), gCfg.nightEnd);

  lo  = constrain(lo,  40, 400);
  hi  = constrain(hi,  40, 400);
  sev = constrain(sev, 40, 400);
  dim = constrain(dim, 5, 100);
  // Un ordre incoherent est une vraie erreur : la page laisse l'utilisateur
  // pousser l'hypo au-dessus de l'hyper.
  if (!(lo < hi && hi < sev)) {
    logf("[cfg] refus : seuils %d/%d/%d\n", lo, hi, sev);
    server.send(400, F("application/json"), F("{\"ok\":false,\"err\":\"seuils\"}"));
    return;
  }

  // Champs de mot de passe vides = "garder l'existant". On calcule les
  // nouvelles valeurs AVANT de toucher a gCfg, pour pouvoir les verifier.
  String newUser = mail.length() ? mail : gCfg.user;
  String newPass = dpw.length()  ? dpw  : gCfg.pass;
  bool credsChanged = (newUser != gCfg.user) || (newPass != gCfg.pass) ||
                      (us != gCfg.regionUS);

  // Verification immediate, tant que l'utilisateur est devant sa page : sans
  // elle, une faute de frappe ne se manifeste qu'apres le redemarrage, par un
  // discret motif violet/rouge — et en portail captif le telephone a souvent
  // deja quitte le reseau de la lampe, donc le message n'arrive jamais.
  // Possible seulement si la lampe a du reseau : au premier branchement elle
  // n'a que son point d'acces et ne peut joindre personne.
  // Une seule tentative par clic : la regle anti-verrouillage du compte
  // Dexcom reste intacte (une action humaine = un essai).
  if (credsChanged && WiFi.isConnected() && newUser.length() && newPass.length()) {
    int r = dexProbe(newUser, newPass, us);
    if (r == 0) {
      logf("[cfg] refus : identifiants Dexcom rejetes par Dexcom\n");
      // Rien n'est enregistre, pas de redemarrage : l'utilisateur corrige.
      server.send(400, F("application/json"), F("{\"ok\":false,\"err\":\"dexcom\"}"));
      return;
    }
    if (r < 0) logf("[cfg] verification impossible (reseau) : on enregistre quand meme\n");
  }

  gCfg.hypo = lo; gCfg.hyper = hi; gCfg.hyper2 = sev;
  gCfg.nightStart = nOn; gCfg.nightEnd = nOff; gCfg.nightDim = dim;
  gCfg.otaAuto = ota;
  gCfg.user = newUser;
  gCfg.pass = newPass;
  gCfg.regionUS = us;

  if (!saveConfig()) {
    server.send(500, F("application/json"), F("{\"ok\":false,\"err\":\"flash\"}"));
    return;
  }

  if (ssid.length()) {
    String pw = wpw.length() ? wpw : lookupWifiPass(ssid);
    rememberWifi(ssid, pw);
    gKnownWifi = 1;
  }

  if (credsChanged || fatalCreds) {
    // SEUL chemin qui leve fatalCreds : une action humaine = un essai.
    // Anti-lock du compte Dexcom, ne pas assouplir.
    fatalCreds = false;
    accountId = ""; sessionId = "";
    failCount = 0; pollInterval = POLL_BASE_MS;
  }
  logf("[cfg] sauvegarde (region %s, %d/%d/%d, nuit %s->%s %d%%, ota %d)\n",
       gCfg.regionUS ? "us" : "ous", lo, hi, sev,
       hhmm(nOn).c_str(), hhmm(nOff).c_str(), dim, (int)ota);

  // Deux facons d'appliquer, selon qu'on avait du reseau ou non.
  if (!WiFi.isConnected() && ssid.length()) {
    // PREMIERE CONFIGURATION (portail captif, aucun acces Internet).
    // On ne redemarre PAS : un reboot fait disparaitre le point d'acces
    // pendant 15 s, le telephone part voir ailleurs et l'utilisateur
    // n'apprend jamais qu'un mot de passe etait faux. On applique donc a
    // chaud, point d'acces maintenu, et la page suit le verdict en direct.
    String pw = wpw.length() ? wpw : lookupWifiPass(ssid);
    logf("[cfg] application a chaud : connexion a %s\n", ssid.c_str());
    server.send(200, F("application/json"), F("{\"ok\":true,\"pending\":true}"));
    WiFi.begin(ssid.c_str(), pw.length() ? pw.c_str() : nullptr);
    gPending = true;
    gPendingSince = millis();
    nextPollAt = millis() + 4000;   // laisse la connexion s'etablir
    return;
  }

  // LAMPE DEJA EN LIGNE : les identifiants ont ete verifies ci-dessus, le
  // redemarrage est donc sans surprise. Le 200 part AVANT : ces 1,2 s sont le
  // seul moment ou l'utilisateur voit l'ecran final et l'adresse a noter.
  server.send(200, F("application/json"), F("{\"ok\":true}"));
  gRebootAt = millis() + 1200;
}

// Reinitialisation : la lampe redevient vierge (utile avant de la donner).
static void handleForget() {
  gLastHttpAt = millis();
  logf("[cfg] REINITIALISATION demandee\n");
  LittleFS.remove("/wifis.txt");
  LittleFS.remove(UPDATE_TRY_PATH);
  // On ecrit une config VIDE plutot que de supprimer le fichier : son absence
  // ferait retomber sur les identifiants de secrets.h (pratique en dev, mais
  // ici on veut vraiment une lampe vierge).
  gCfg = Config();
  gCfg.user = "";
  gCfg.pass = "";
  saveConfig();
  // On repond AVANT de toucher au WiFi : couper la STA ici empecherait la
  // reponse d'arriver, et la page annoncerait un echec alors que tout a
  // fonctionne. L'oubli du reseau memorise par le SDK se fait juste avant
  // le redemarrage (voir la fin de loop()).
  server.send(200, F("application/json"), F("{\"ok\":true}"));
  gForgetWifi = true;
  gRebootAt = millis() + 1200;
}

// Hors spec designer, mais indispensables : il n'y a pas de port serie.
static void handleDiag() {
  String o; o.reserve(448); o = '{';
  jsonS(o, "fw", FW_VERSION);
  jsonI(o, "up", millis() / 1000);
  jsonI(o, "lps", gLoopsPerSec);        // iterations de loop()/s : detecteur de gel
  jsonI(o, "heap", ESP.getFreeHeap());
  jsonI(o, "maxblk", ESP.getMaxFreeBlockSize());
  jsonI(o, "frag", ESP.getHeapFragmentation());
  jsonS(o, "rst", ESP.getResetReason());
  jsonS(o, "host", String(gHostname) + ".local");
  jsonS(o, "ap", gApName);
  jsonS(o, "ssid", WiFi.SSID());
  jsonI(o, "rssi", WiFi.RSSI());
  jsonS(o, "ip", WiFi.localIP().toString());
  jsonB(o, "portal", gPortal);
  jsonI(o, "apclients", WiFi.softAPgetStationNum());
  jsonI(o, "wifis", gKnownWifi);
  jsonI(o, "cfgver", CFG_VER);
  jsonI(o, "scan", WiFi.scanComplete());
  jsonB(o, "fatal", fatalCreds);
  jsonI(o, "fail", failCount);
  jsonI(o, "poll", pollInterval / 1000);
  jsonB(o, "otaauto", gCfg.otaAuto);
  jsonS(o, "tried", readLineFile(UPDATE_TRY_PATH));
  jsonS(o, "state", stateCode(currentState()));
  jsonEnd(o);
  sendJson(o);
}

static void handleLog() {
  gLastHttpAt = millis();
  String o; o.reserve(LOG_SIZE + 16);
  if (gLogWrap) for (uint16_t i = gLogHead; i < LOG_SIZE; i++) o += gLog[i];
  for (uint16_t i = 0; i < gLogHead; i++) o += gLog[i];
  server.send(200, F("text/plain"), o);
}

// Permet de tester tout le portail captif sans couper la STA (mode AP_STA).
static void handlePortal() {
  gPortalForced = (server.arg("on") == "1");
  logf("[ap] portail force : %d\n", (int)gPortalForced);
  String o; o.reserve(64); o = '{';
  jsonB(o, "forced", gPortalForced);
  jsonB(o, "portal", gPortal);
  jsonEnd(o);
  sendJson(o);
}

// Redirection captive : 302 + Location, corps vide, puis fermeture du socket
// (un corps vide n'a pas de Content-Length, le client resterait pendu).
static bool captiveRedirect() {
  if (!gPortal) return false;
  String ip = WiFi.softAPIP().toString();
  String h  = server.hostHeader();
  if (h == ip || h.startsWith(gHostname)) return false;   // deja chez nous
  server.sendHeader(F("Location"), String(F("http://")) + ip + "/", true);
  server.send(302, F("text/plain"), F(""));
  server.client().stop();
  return true;
}

static void handleCaptive() {
  gLastHttpAt = millis();
  if (!captiveRedirect()) handleRoot();
}

static void handleNotFound() {
  gLastHttpAt = millis();
  if (!captiveRedirect()) server.send(404, F("text/plain"), F("404"));
}

static void registerRoutes() {
  server.on(F("/"),           HTTP_GET,  handleRoot);
  server.on(F("/api/status"), HTTP_GET,  handleStatus);
  server.on(F("/api/config"), HTTP_GET,  handleConfigGet);
  server.on(F("/api/scan"),   HTTP_GET,  handleScan);
  server.on(F("/api/save"),   HTTP_POST, handleSave);
  server.on(F("/api/forget"), HTTP_POST, handleForget);
  server.on(F("/api/diag"),   HTTP_GET,  handleDiag);
  server.on(F("/api/log"),    HTTP_GET,  handleLog);
  server.on(F("/api/portal"), HTTP_GET,  handlePortal);
  // Detection de portail captif : ces URL sont appelees par l'OS des qu'il
  // rejoint un WiFi. Une reponse inattendue lui fait ouvrir la page.
  server.on(F("/hotspot-detect.html"),        HTTP_GET, handleCaptive); // iOS
  server.on(F("/library/test/success.html"),  HTTP_GET, handleCaptive); // macOS
  server.on(F("/generate_204"),               HTTP_GET, handleCaptive); // Android
  server.on(F("/gen_204"),                    HTTP_GET, handleCaptive);
  server.on(F("/ncsi.txt"),                   HTTP_GET, handleCaptive); // Windows
  server.on(F("/connecttest.txt"),            HTTP_GET, handleCaptive);
  server.on(F("/redirect"),                   HTTP_GET, handleCaptive);
  server.on(F("/fwlink"),                     HTTP_GET, handleCaptive);
  server.onNotFound(handleNotFound);
}

// ------------------- Portail captif -------------------
static bool portalNeeded() {
  if (fatalCreds) return true;
  if (gCfg.user.length() == 0 || gCfg.pass.length() == 0) return true;
  if (WiFi.status() != WL_CONNECTED && (long)(millis() - gStaLostAt) > 30000)
    return true;
  return false;
}

static void portalStart() {
  if (gPortal) return;
  WiFi.mode(WIFI_AP_STA);
  // 3e argument = canal : en AP_STA le SDK aligne l'AP sur le canal de la STA
  // des qu'elle se connecte ; le passer d'emblee evite d'ejecter les clients
  // du portail. Mot de passe nullptr = reseau ouvert (obligatoire pour que la
  // detection captive s'ouvre sans friction ; un psk de 1 a 7 caracteres
  // ferait echouer softAP() en silence).
  int ch = WiFi.channel();
  if (ch < 1 || ch > 13) ch = 1;
  if (!WiFi.softAP(gApName, (const char*)nullptr, ch, false, 4)) {
    logf("[ap] ECHEC softAP\n");
    return;
  }
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());     // "*" = wildcard
  gPortal = true;
  gScanJson = "";                          // resultats perimes
  if (canScanNow()) WiFi.scanNetworks(true, false);   // pre-chauffe la liste
  logf("[ap] portail ouvert : %s canal %d ip %s\n",
       gApName, ch, WiFi.softAPIP().toString().c_str());
}

static void portalStop() {
  if (!gPortal) return;
  dns.stop();
  WiFi.softAPdisconnect(true);   // n'affecte pas la STA
  WiFi.mode(WIFI_STA);
  gPortal = false;
  logf("[ap] portail ferme\n");
}

// Seul endroit qui leve ou baisse l'AP.
static void portalTick() {
  bool need = portalNeeded() || gPortalForced;
  if (need && !gPortal) portalStart();
  else if (!need && gPortal && WiFi.softAPgetStationNum() == 0) portalStop();
}

static void wifiTick() {
  static bool wasConnected = false;
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !wasConnected) {
    logf("[wifi] connecte a %s : %s\n", WiFi.SSID().c_str(),
         WiFi.localIP().toString().c_str());
    rememberWifi(WiFi.SSID(), WiFi.psk());
  } else if (!connected && wasConnected) {
    gStaLostAt = millis();
    logf("[wifi] connexion perdue\n");
  }
  wasConnected = connected;

  if (connected || (long)(millis() - wifiRetryAt) < 0) return;
  wifiRetryAt = millis() + 240000UL;
  // wifiMulti.run() fait un scan synchrone : il fige loop() et couperait
  // l'AP en pleine configuration. On s'abstient si quelqu'un est dessus.
  if (WiFi.softAPgetStationNum() > 0) return;
  if (gKnownWifi == 0) return;
  gConnecting = true;
  logf("[wifi] reconnexion aux reseaux connus...\n");
  wifiMulti.run(8000);
  gConnecting = false;
}

// ------------------- Mise a jour automatique -------------------
// Extrait X, Y, Z de "X.Y.Z" (tout suffixe est ignore : "9.9.9-test" -> 9,9,9).
static void parseSemver(const String& s, int* out) {
  out[0] = out[1] = out[2] = 0;
  int idx = 0, start = 0;
  for (int i = 0; i <= (int)s.length() && idx < 3; i++) {
    char c = (i < (int)s.length()) ? s[i] : '.';
    if (c == '.' || c == '-' || c == '+') {
      out[idx++] = s.substring(start, i).toInt();
      if (c != '.') break;
      start = i + 1;
    }
  }
}

// N'installe QUE des versions strictement plus recentes.
// Une simple inegalite serait tentante (elle permettrait de revenir en arriere
// en republiant une version anterieure), mais elle retrograde aussi toute
// lampe portant un firmware plus recent que la release publiee : elle casse
// l'etape canari du script de release et le developpement (mesure en vrai :
// une lampe en 1.2.0 s'est reinstallee la 1.1.0 publiee).
// Pour revenir en arriere : republier l'ancien code sous un NUMERO PLUS HAUT.
static bool versionGreater(const String& a, const String& b) {
  int av[3], bv[3];
  parseSemver(a, av);
  parseSemver(b, bv);
  for (int i = 0; i < 3; i++) if (av[i] != bv[i]) return av[i] > bv[i];
  return false;
}

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
  logf("[upd] distante '%s' (HTTP %d) | locale '%s'\n", v.c_str(), code, FW_VERSION);
  return v;
}

// Telechargement + flash (bloquant ~30-60 s, reboot automatique si OK).
static void doFirmwareUpdate(const String& remoteVer, int tries) {
  // Ecrit AVANT le flash : si la carte n'en revient pas, on saura au boot
  // suivant que cette version a deja ete tentee (anti-boucle de brique).
  writeLineFile(UPDATE_TRY_PATH, remoteVer + " " + String(tries + 1));

  // Le buffer TLS de 16 Ko ne tient pas avec un softAP et des sockets ouverts.
  bool hadPortal = gPortal;
  server.stop();
  if (gPortal) { dns.stop(); WiFi.softAPdisconnect(true); gPortal = false; }
  delay(50);

  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(TLS_RX_BULK, TLS_TX);   // 16 Ko : indispensable ici
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  ESPhttpUpdate.onStart([]() {
    logf("[upd] telechargement et flash...\n");
    strip.clear();
    strip.show();
  });
  ESPhttpUpdate.onProgress([](int cur, int total) { progressBar(cur, total); });
  ESPhttpUpdate.onError([](int e) { logf("[upd] erreur %d\n", e); });

  t_httpUpdate_return r = ESPhttpUpdate.update(client, UPDATE_FW_URL);
  // Attention : HTTP_UPDATE_FAILED vaut 0, comparer aux enums.
  if (r == HTTP_UPDATE_FAILED)
    logf("[upd] ECHEC (%d) : %s\n", ESPhttpUpdate.getLastError(),
         ESPhttpUpdate.getLastErrorString().c_str());
  else if (r == HTTP_UPDATE_NO_UPDATES)
    logf("[upd] rien a mettre a jour\n");
  // HTTP_UPDATE_OK : la carte a redemarre, on ne repasse pas ici.

  server.begin();                 // echec : on remet tout en service
  if (hadPortal) portalStart();
}

// Retourne true si la verification est allee au bout (qu'une mise a jour ait
// ete trouvee ou non), false si elle a ete reportee.
static bool checkAutoUpdate() {
  if (!gCfg.otaAuto) return true;                    // desactive par l'utilisateur
  if (WiFi.status() != WL_CONNECTED) return false;
  if (gPortal && WiFi.softAPgetStationNum() > 0) return false;  // config en cours
  if (millis() - gLastHttpAt < 60000) return false;   // quelqu'un consulte la page
  DisplayState st = currentState();
  if (st == ST_HYPO || st == ST_HYPER2) return false; // jamais pendant une alerte
  if (ESP.getFreeHeap() < UPDATE_MIN_HEAP) {
    logf("[upd] heap %u trop bas, report\n", ESP.getFreeHeap());
    return false;
  }
  String remote = fetchRemoteVersion();
  if (remote.length() == 0) return false;
  if (!versionGreater(remote, FW_VERSION)) return true;   // rien de plus recent

  // Suivi des tentatives : "<version> <n>". Un echec reseau passager doit
  // pouvoir etre retente, mais un binaire durablement mauvais ne doit pas
  // boucler indefiniment.
  String tried = readLineFile(UPDATE_TRY_PATH);
  int tries = 0;
  int sp = tried.indexOf(' ');
  if (sp > 0 && tried.substring(0, sp) == remote) tries = tried.substring(sp + 1).toInt();
  else if (tried == remote) tries = 1;              // ancien format
  if (tries >= UPDATE_MAX_TRIES) {
    logf("[upd] v%s echouee %d fois, abandonnee\n", remote.c_str(), tries);
    return true;
  }
  logf("[upd] %s -> %s (essai %d)\n", FW_VERSION, remote.c_str(), tries + 1);
  doFirmwareUpdate(remote, tries);
  return true;
}

// ------------------- Setup / Loop -------------------
bool booting = true;

void setup() {
  Serial.begin(115200);
  logf("\n[veilleuse] boot\n");
  buildIdentity();

  strip.begin();
  strip.setBrightness(255);   // l'intensite est portee par chaque etat
  strip.clear();
  strip.show();

  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  WiFi.hostname(gHostname);
  WiFi.setAutoReconnect(true);
  LittleFS.begin();

  // Mise a jour precedente : succes confirme si la version tentee est celle
  // qui tourne maintenant (fichier "<version> <essais>").
  String tried = readLineFile(UPDATE_TRY_PATH);
  int sp = tried.indexOf(' ');
  if (tried.length() && (tried == FW_VERSION ||
                         (sp > 0 && tried.substring(0, sp) == FW_VERSION))) {
    LittleFS.remove(UPDATE_TRY_PATH);
    logf("[upd] mise a jour vers %s confirmee\n", FW_VERSION);
  }

  if (loadConfig()) {
    logf("[cfg] config chargee (region %s)\n", gCfg.regionUS ? "us" : "ous");
    if (gCfgMigrated) { saveConfig(); logf("[cfg] migre v1 -> v2 (horaires en minutes)\n"); }
  } else if (gCfg.user.length() && gCfg.pass.length()) {
    logf("[cfg] pas de config -> identifiants de secrets.h (dev)\n");
  } else {
    logf("[cfg] AUCUN identifiant -> configuration par le portail\n");
  }

  gKnownWifi = loadWifiList();
#ifdef WIFI_SSID
  if (strlen(WIFI_SSID) > 0) {           // seed optionnel depuis secrets.h
    wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
    gKnownWifi++;
  }
#endif
  logf("[wifi] %d reseau(x) connu(s)\n", gKnownWifi);

  gStaLostAt = millis() - 60000;         // pas de grace au boot : AP immediat
  if (gKnownWifi > 0) {
    logf("[wifi] connexion au meilleur reseau connu...\n");
    gConnecting = true;
    wifiMulti.run(15000);
    gConnecting = false;
  }

  // NTP avec fuseau Europe/Paris (ete/hiver automatique)
  configTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
  wifiRetryAt = millis() + 240000UL;

  // OTA local : mise a jour par WiFi depuis le poste de dev
  // (pio run -e d1_mini_ota -t upload --upload-port <hostname>.local)
  ArduinoOTA.setHostname(gHostname);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    logf("[ota] mise a jour...\n");
    strip.clear();
    strip.show();
  });
  ArduinoOTA.onProgress([](unsigned int prog, unsigned int total) {
    progressBar((int)prog, (int)total);
  });
  ArduinoOTA.onEnd([]() { logf("[ota] termine, reboot\n"); });
  ArduinoOTA.onError([](ota_error_t e) { logf("[ota] erreur %u\n", e); });
  ArduinoOTA.begin();
  // Publie la version en mDNS : permet de savoir ce que tourne chaque lampe
  // du reseau (dns-sd -L <hostname> _arduino._tcp local).
  MDNS.addServiceTxt("arduino", "tcp", "fw", FW_VERSION);

  // Fenetre OTA garantie AVANT de demarrer le serveur web : si le code qui
  // suit plantait au demarrage, il resterait 3 s a chaque cycle de reboot
  // pour reflasher la carte. Sans port serie, c'est le seul filet.
  unsigned long t0 = millis();
  while (millis() - t0 < 3000) {
    ArduinoOTA.handle();
    render(ST_BOOT);
    delay(10);
  }

  registerRoutes();
  server.begin();
  MDNS.addService("http", "tcp", 80);
  logf("[web] serveur pret sur http://%s.local/\n", gHostname);

  portalTick();               // leve l'AP tout de suite si necessaire
}

void loop() {
  ArduinoOTA.handle();        // appelle aussi MDNS.update()
  server.handleClient();
  if (gPortal) dns.processNextRequest();

  // Compteur d'iterations : detecteur de gel, expose par /api/diag.
  static unsigned long lastSec = 0;
  static uint16_t loops = 0;
  loops++;
  if (millis() - lastSec >= 1000) { lastSec = millis(); gLoopsPerSec = loops; loops = 0; }

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
    if (idx != lastIdx) { lastIdx = idx; logf("[demo] %s\n", demoNames[idx]); }
    render(demoSeq[idx]);
#else
    if (booting && !gPortal) {
      render(ST_BOOT);                   // point bleu tournant : demarrage
    } else {
      DisplayState st = currentState();
      gDim = (isNight() && st != ST_HYPO) ? (gCfg.nightDim / 100.0f) : 1.0f;
      render(st);
    }
#endif
  }

  pollScan();
  wifiTick();
  portalTick();

  // Verdict d'une configuration appliquee a chaud. On ne redemarre qu'une fois
  // la reussite constatee (WiFi + une vraie mesure), et avec du retard pour
  // laisser la page afficher sa confirmation et l'adresse a noter. En cas
  // d'echec : aucun redemarrage, le portail reste ouvert pour corriger.
  if (gPending) {
    if (fatalCreds) {
      gPending = false;
      logf("[cfg] verdict : identifiants Dexcom refuses\n");
    } else if (WiFi.isConnected() && mgdl >= 0) {
      gPending = false;
      gRebootAt = millis() + 10000;
      logf("[cfg] verdict : tout fonctionne, redemarrage dans 10 s\n");
    } else if ((long)(millis() - gPendingSince) > 120000) {
      gPending = false;      // on arrete d'attendre, la page montrera l'etat
      logf("[cfg] verdict : pas de reponse en 120 s\n");
    }
  }

  if (gRebootAt && (long)(millis() - gRebootAt) >= 0) {
    logf("[sys] redemarrage\n");
    server.stop();
    if (gPortal) { dns.stop(); WiFi.softAPdisconnect(true); }
    // Reinitialisation : effacer les identifiants WiFi memorises par le SDK
    // (sinon la carte se reconnecterait toute seule au dernier reseau).
    if (gForgetWifi) { WiFi.disconnect(true); delay(100); }
    delay(100);
    ESP.restart();             // jamais ESP.reset() : la reponse serait tronquee
  }

  // Attente du WiFi au demarrage
  if (booting) {
    if (WiFi.status() == WL_CONNECTED) {
      booting = false;
      nextPollAt = millis();             // premier poll immediat
    } else if (gPortal) {
      booting = false;                   // pas de WiFi : on vit sur le portail
    }
    return;
  }

  // Poll Dexcom
#if DEMO_MODE
  return;                                // pas de requetes pendant la demo
#endif
  if ((long)(millis() - nextPollAt) >= 0 && !fatalCreds) {
    // Un handshake TLS fige loop() 2 a 6 s : on laisse la priorite a
    // quelqu'un qui consulte la page. Pas de famine, la page n'interroge le
    // firmware que toutes les 15 s.
    if (millis() - gLastHttpAt < 3000 || WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
      nextPollAt = millis() + 3000;
    } else if (WiFi.status() != WL_CONNECTED) {
      failCount++;
      pollInterval = min(pollInterval * 2, POLL_MAX_MS);
      nextPollAt = millis() + pollInterval;
    } else if (dexUpdate()) {
      failCount = 0;
      pollInterval = POLL_BASE_MS;       // retour a la normale
      nextPollAt = millis() + pollInterval;
      // Check MAJ apres le 1er poll reussi. S'il a ete reporte (heap,
      // portail...), on retentera au poll suivant plutot qu'attendre 4 h.
      if (!gBootCheckDone) gBootCheckDone = checkAutoUpdate();
    } else {
      failCount++;
      pollInterval = min(pollInterval * 2, POLL_MAX_MS);  // backoff 60s -> 5min
      nextPollAt = millis() + pollInterval;
      logf("[dex] echec #%d, prochain essai dans %lus\n",
           failCount, pollInterval / 1000);
    }
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
