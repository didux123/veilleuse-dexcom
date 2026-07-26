// ============================================================
// Veilleuse Dexcom — ESP8266 (Wemos D1 mini) + ruban SK6812/WS2812B
// Étape 1 : WiFi + API Dexcom Share directe + poll 60 s + couleurs.
//
// Câblage : DIN ruban -> D1 (GPIO5), 5V -> 5V, GND -> G.
//           (+ résistance ~330 ohms en série sur DIN,
//            condensateur 470-1000 uF entre 5V et GND)
//
// Codes couleur :
//   vert doux fixe      = dans la plage
//   orange fixe         = hyper (> HYPER_MGDL)
//   rouge "respiration" = hypo  (< HYPO_MGDL)
//   bleu/blanc lent     = données périmées (> STALE_MIN)
//   bleu/blanc rapide   = erreur réseau / WiFi / Dexcom injoignable
//   violet respirant    = mode configuration WiFi : l'ESP émet le réseau
//                         "Veilleuse-Dexcom" -> s'y connecter au téléphone,
//                         le portail s'ouvre, choisir son WiFi.
//   rouge/bleu alterné  = identifiants refusés (reflasher) — JAMAIS de
//                         faux vert muet.
// ============================================================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiManager.h>
#include <ESP8266WiFiMulti.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <time.h>
#include "secrets.h"

#ifndef OTA_PASSWORD
#define OTA_PASSWORD "veilleuse"   // mot de passe OTA par defaut
#endif

#define WIFI_AP_NAME   "Veilleuse-Dexcom"   // reseau emis en mode config
#define WIFI_LIST_MAX  10                   // nb max de reseaux memorises

// ------------------- Réglages -------------------
#define LED_PIN        D1      // GPIO5
#define NUM_LEDS       12      // anneau quasi complet (200 mm à 60 LED/m)

// ----- MODE DEMO : cycle tous les etats (6 s chacun), sans requete Dexcom.
//       Remettre a 0 puis reflasher pour le fonctionnement normal. -----
#define DEMO_MODE      0

// Luminosite globale a fond : chaque etat porte sa propre intensite dans ses
// couleurs. Les etats normaux restent doux ; l'HYPO pulse a pleine puissance
// (rouge pur 12 LED = ~240 mA, sans danger pour l'alim USB).

// ----- Mode nuit : couleur juste lisible, sans eclairer la chambre.
//       L'HYPO ignore le mode nuit (alarme toujours pleine puissance). -----
#define NIGHT_START_H  22      // debut (heure locale, TZ Europe/Paris)
#define NIGHT_END_H    7       // fin
#define NIGHT_DIM      0.22f   // facteur de luminosite la nuit

#define HYPO_MGDL      70
#define HYPER_MGDL     180
#define STALE_MIN      15      // données plus vieilles -> "stale"

#define POLL_BASE_MS   60000UL   // 60 s
#define POLL_MAX_MS    300000UL  // backoff max 5 min

// Région Europe ("ous")
static const char* DEX_BASE = "https://shareous1.dexcom.com/ShareWebServices/Services";
static const char* DEX_APP_ID = "d89443d2-327c-4a6f-89e5-496bbb0317db"; // public
static const char* DEX_UA = "Dexcom Share/3.0.2.11 CFNetwork/711.2.23 Darwin/14.0.0";

// ------------------- État -------------------
enum DisplayState { ST_BOOT, ST_CONFIG, ST_OK, ST_HYPER, ST_HYPO, ST_STALE, ST_ERROR, ST_FATAL };

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
WiFiManager wm;
ESP8266WiFiMulti wifiMulti;
unsigned long wifiRetryAt = 0;

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
  String payload = String("{\"accountName\":\"") + DEXCOM_USERNAME +
                   "\",\"password\":\"" + DEXCOM_PASSWORD +
                   "\",\"applicationId\":\"" + DEX_APP_ID + "\"}";
  String body = httpPost(String(DEX_BASE) + "/General/AuthenticatePublisherAccount",
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
                   "\",\"password\":\"" + DEXCOM_PASSWORD +
                   "\",\"applicationId\":\"" + DEX_APP_ID + "\"}";
  String body = httpPost(String(DEX_BASE) + "/General/LoginPublisherAccountById",
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
  String url = String(DEX_BASE) +
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

static bool isNight() {
  time_t now = time(nullptr);
  if (now < 1600000000) return false;    // NTP pas encore synchro -> jour
  struct tm lt;
  localtime_r(&now, &lt);
  return lt.tm_hour >= NIGHT_START_H || lt.tm_hour < NIGHT_END_H;
}

static DisplayState currentState() {
  if (fatalCreds) return ST_FATAL;
  if (wm.getConfigPortalActive()) return ST_CONFIG;
  if (WiFi.status() != WL_CONNECTED || failCount >= 2 || mgdl < 0) return ST_ERROR;
  time_t now = time(nullptr);
  bool timeOk = now > 1600000000;        // NTP synchronise
  if (timeOk && wtEpoch > 0 && (now - wtEpoch) > STALE_MIN * 60) return ST_STALE;
  if (mgdl < HYPO_MGDL)  return ST_HYPO;
  if (mgdl > HYPER_MGDL) return ST_HYPER;
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
    case ST_HYPO: {                       // ALARME : pulsation rouge pleine
      float phase = (t % 1200) / 1200.0f; // puissance, rapide, 0 -> 255
      float lvl = 0.5f * (1.0f - cosf(phase * 2.0f * PI));   // 0..1
      fillAll((uint8_t)(255 * lvl), 0, 0);
      break;
    }
    case ST_STALE: {                                    // bleu/blanc lent
      bool on = (t / 1200) % 2;
      if (on) fillAll(0, 0, 38); else fillAll(22, 22, 22);
      break;
    }
    case ST_ERROR: {                                    // bleu/blanc rapide
      bool on = (t / 400) % 2;
      if (on) fillAll(0, 0, 38); else fillAll(22, 22, 22);
      break;
    }
    case ST_FATAL: {                                    // rouge/bleu : reflasher
      bool on = (t / 600) % 2;
      if (on) fillAll(47, 0, 0); else fillAll(0, 0, 47);
      break;
    }
  }
}

// ------------------- Setup / Loop -------------------
bool booting = true;

void setup() {
  Serial.begin(115200);
  Serial.println("\n[veilleuse] boot");

  strip.begin();
  strip.setBrightness(255);   // l'intensite est portee par chaque etat
  strip.clear();
  strip.show();

  WiFi.mode(WIFI_STA);
  WiFi.persistent(true);
  LittleFS.begin();

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

  if (known > 0) {
    Serial.println("[wifi] connexion au meilleur reseau connu...");
    wifiMulti.run(15000);                // scanne et prend le plus fort
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] portail de configuration actif");
    wm.startConfigPortal(WIFI_AP_NAME);
  }

  // NTP avec fuseau Europe/Paris (ete/hiver automatique)
  configTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
  wifiRetryAt = millis() + 240000UL;

  // OTA : mise a jour par WiFi (pio run -e d1_mini_ota -t upload)
  ArduinoOTA.setHostname("veilleuse");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println("[ota] mise a jour...");
    strip.clear();
    strip.show();
  });
  ArduinoOTA.onProgress([](unsigned int prog, unsigned int total) {
    int lit = (int)((prog * (unsigned)NUM_LEDS) / total);
    strip.clear();                       // barre de progression bleue
    for (int i = 0; i < lit; i++) strip.setPixelColor(i, 0, 0, 60);
    strip.show();
  });
  ArduinoOTA.onEnd([]() { Serial.println("[ota] termine, reboot"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] erreur %u\n", e); });
  ArduinoOTA.begin();
}

void loop() {
  ArduinoOTA.handle();

  // Animation (30 fps environ)
  static unsigned long lastFrame = 0;
  if (millis() - lastFrame >= 33) {
    lastFrame = millis();
#if DEMO_MODE
    static const DisplayState demoSeq[] = {ST_OK, ST_HYPER, ST_HYPO,
                                           ST_STALE, ST_ERROR, ST_CONFIG, ST_FATAL};
    static const char* demoNames[] = {"VERT (dans la plage)", "ORANGE (hyper)",
        "ROUGE PULSE (hypo)", "BLEU/BLANC lent (perime)",
        "BLEU/BLANC rapide (erreur)", "VIOLET (config WiFi)",
        "ROUGE/BLEU (identifiants)"};
    int idx = (int)((millis() / 6000) % 7);
    static int lastIdx = -1;
    if (idx != lastIdx) { lastIdx = idx; Serial.printf("[demo] %s\n", demoNames[idx]); }
    render(demoSeq[idx]);
#else
    if (booting && !wm.getConfigPortalActive()) {
      strip.clear();                     // point bleu tournant : demarrage
      strip.setPixelColor((millis() / 120) % NUM_LEDS, 0, 0, 28);
      strip.show();
    } else {
      DisplayState st = currentState();
      gDim = (isNight() && st != ST_HYPO) ? NIGHT_DIM : 1.0f;
      render(st);
    }
#endif
  }

  // Gestion WiFi : memoire multi-reseaux + portail captif
  static bool wasConnected = false;
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !wasConnected) {
    Serial.printf("[wifi] connecte a %s : %s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    rememberWifi(WiFi.SSID(), WiFi.psk());   // tout reseau rejoint est memorise
    if (wm.getConfigPortalActive()) wm.stopConfigPortal();
  }
  wasConnected = connected;

  if (!connected) {
    if (wm.getConfigPortalActive()) {
      wm.process();
    } else if ((long)(millis() - wifiRetryAt) >= 0) {
      Serial.println("[wifi] reconnexion aux reseaux connus...");
      if (wifiMulti.run(12000) != WL_CONNECTED) {
        Serial.println("[wifi] echec -> portail 3 min");
        wm.startConfigPortal(WIFI_AP_NAME);
      }
      wifiRetryAt = millis() + 240000UL;
    }
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
    } else {
      failCount++;
      pollInterval = min(pollInterval * 2, POLL_MAX_MS);  // backoff 60s -> 5min
      Serial.printf("[dex] echec #%d, prochain essai dans %lus\n",
                    failCount, pollInterval / 1000);
    }
    nextPollAt = millis() + pollInterval;
  }
}
