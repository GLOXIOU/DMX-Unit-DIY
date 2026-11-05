#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// DMX 1
#define DMX1_TX_PIN    17
#define DMX1_RTS_PIN   4
#define DMX1_RX_PIN    16

// DMX 2
#define DMX2_TX_PIN    18
#define DMX2_RTS_PIN   5
#define DMX2_RX_PIN    15

#define PORTAL_ONLY_TEST 0
#define FORCE_AP_MODE    1
#define DEBUG_ON_USB     1

#if PORTAL_ONLY_TEST || DEBUG_ON_USB
  #define DBG Serial
#else
  HardwareSerial DebugSerial(0);
  #define DBG DebugSerial
#endif

// WiFi acces point
#define WIFI_AP_SSID        "DMX-Unit-Setup"
#define WIFI_AP_PASSWORD    "dmxsetup1"
#define WIFI_STA_TIMEOUT_MS 10000UL

// Art-Net
#define ARTNET_PORT           6454
#define ARTNET_HEADER_SZ      18
#define ARTNET_MAX_SIZE       530
#define ARTNET_OP_DMX         0x5000

const uint8_t ARTNET_ID[8] = { 'A', 'r', 't', '-', 'N', 'e', 't', 0 };

WiFiUDP wifiUdp;
uint8_t artnetBuffer[ARTNET_MAX_SIZE];

HardwareSerial dmxPort1(1);
HardwareSerial dmxPort2(2);

uint8_t dmxData1[513];
uint8_t dmxData2[513];
uint8_t dmxUsbData[513];

unsigned long lastDmxSendMs = 0;
const unsigned long DMX_REFRESH_MS = 25;

unsigned long lastUsbDmxMs = 0;
const unsigned long USB_DMX_TIMEOUT_MS = 2000;

#define CFG_NAMESPACE "dmxcfg"

struct DmxUnitConfig {
  String wifiSsid;
  String wifiPass;
  uint16_t universe1;
  uint16_t universe2;
  bool usbInputEnabled;
  bool dmx1Enabled;
  bool dmx2Enabled;
  bool artnetEnabled;
};

DmxUnitConfig cfg;
Preferences prefs;

WebServer webServer(80);
DNSServer dnsServer;
bool apMode = false;
IPAddress apIP(192, 168, 4, 1);
unsigned long webReqCount = 0;

unsigned long dmxFrames1 = 0;
unsigned long dmxFrames2 = 0;
unsigned long artnetDmxCount1 = 0;
unsigned long artnetDmxCount2 = 0;
unsigned long artnetIgnored = 0;
unsigned long usbPacketCount = 0;
unsigned long wifiPktCount = 0;
uint16_t lastUniverseSeen = 0xFFFF;
volatile uint8_t testMode = 0;
bool usbWasLive = false;
bool artnetSeen1 = false;
bool artnetSeen2 = false;

const char *onOff(bool b) {
  return b ? "ON" : "OFF";
}

bool isUsbLive() {
  return cfg.usbInputEnabled && lastUsbDmxMs != 0 && (millis() - lastUsbDmxMs < USB_DMX_TIMEOUT_MS);
}

const char *dmxSourceName(int port) {
  if (port == 1 && isUsbLive()) return "USB";
  return "ARTNET";
}

// DMX
#define ENTTEC_START 0x7E
#define ENTTEC_END   0xE7
#define ENTTEC_LABEL_GET_WIDGET_PARAMS 3
#define ENTTEC_LABEL_SEND_DMX          6

enum UsbDmxState {
  USB_WAIT_START,
  USB_WAIT_LABEL,
  USB_WAIT_LEN_LO,
  USB_WAIT_LEN_HI,
  USB_WAIT_DATA,
  USB_WAIT_END
};

UsbDmxState usbState = USB_WAIT_START;
uint8_t  usbLabel = 0;
uint16_t usbLen = 0;
uint16_t usbIndex = 0;
uint8_t  usbPacket[600];

void loadConfig();
void saveConfig();
void setupDmxOutputs();
void sendDmx();
void setupWifiArtnet();
void pollWifiArtnet();
void handleArtnetPacket(uint8_t *buf, int len);
void setupUsbDmx();
void pollUsbDmx();
void handleUsbDmxPacket(uint8_t label, uint8_t *data, uint16_t len);
void sendEnttecReply(uint8_t label, const uint8_t *data, uint16_t len);
void setupWifiPortal();
void handlePortal();
void handleRoot();
void handleSave();
void handleScanRdm();
void handleReboot();
void handleTestToggle(uint8_t mode);
void handleTestDmx();
void handleTestArtnet();
void injectTestArtnet(uint16_t universe, uint8_t value);
void handleNotFound();
void logReq(const char *tag);
void setupWifiEvents();
void heartbeat();
void webTask(void *arg);
String rdmUidsToHtml(int portIndex);
void applyOutputState();
void logDmxStats();

void setup() {
#if PORTAL_ONLY_TEST || DEBUG_ON_USB
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  delay(500);
#else
  DebugSerial.begin(115200);
  Serial.begin(115200);
  delay(200);
#endif
  DBG.println();
  DBG.println(F("=== DMX-Unit-DIY firmware V3 (debug) ==="));
  DBG.printf("Raison du reset : %d\n", (int)esp_reset_reason());
  DBG.printf("Heap libre : %u octets\n", ESP.getFreeHeap());
  loadConfig();
  DBG.printf("Config : SSID sauvegarde = '%s'\n", cfg.wifiSsid.c_str());
  DBG.printf("Config : DMX1=%s DMX2=%s ARTNET=%s USB=%s\n",
             onOff(cfg.dmx1Enabled), onOff(cfg.dmx2Enabled),
             onOff(cfg.artnetEnabled), onOff(cfg.usbInputEnabled));
  DBG.printf("Config : univers1=%u univers2=%u\n",
             cfg.universe1, cfg.universe2);
  DBG.printf("Options : DEBUG_ON_USB=%d FORCE_AP=%d\n",
             DEBUG_ON_USB, FORCE_AP_MODE);

#if PORTAL_ONLY_TEST
  DBG.println(F("MODE TEST : DMX / Art-Net / USB desactives"));
#else
  setupDmxOutputs();
#endif

  setupWifiPortal();

#if !PORTAL_ONLY_TEST
  setupUsbDmx();
  setupWifiArtnet();
  xTaskCreatePinnedToCore(webTask, "web", 8192, NULL, 1, NULL, 0);
#endif
  DBG.println(F("setup() termine"));
}

void webTask(void *arg) {
  for (;;) {
    handlePortal();
    vTaskDelay(2);
  }
}

void heartbeat() {
  static unsigned long last = 0;
  if (millis() - last < 5000) return;
  last = millis();
  DBG.printf("[%lus] heap=%u clients_wifi=%d requetes_http=%lu mode=%s\n",
             millis() / 1000, ESP.getFreeHeap(), WiFi.softAPgetStationNum(),
             webReqCount, apMode ? "AP" : "STA");
  if (apMode) {
    DBG.printf("   AP '%s' / IP %s / canal %d\n", WIFI_AP_SSID,
               WiFi.softAPIP().toString().c_str(), WiFi.channel());
  }
  DBG.printf("   DMX1=%s DMX2=%s ARTNET=%s\n",
             onOff(cfg.dmx1Enabled), onOff(cfg.dmx2Enabled),
             onOff(cfg.artnetEnabled));
  DBG.printf("   ArtDMX u1=%lu u2=%lu ignores=%lu | USB paquets=%lu\n",
             artnetDmxCount1, artnetDmxCount2,
             artnetIgnored, usbPacketCount);
}

void loop() {
#if PORTAL_ONLY_TEST
  handlePortal();
#else
  pollWifiArtnet();
  pollUsbDmx();

  bool usbLive = isUsbLive();
  if (usbLive != usbWasLive) {
    usbWasLive = usbLive;
    DBG.printf("[USB] source USB %s\n", usbLive ? "ACTIVE (sortie 1 pilotee par USB)" : "PERDUE (retour Art-Net)");
  }
  if (usbLive) {
    memcpy(dmxData1, dmxUsbData, 513);
  }

  uint8_t tm = testMode;
  static uint8_t lastTm = 0;
  if (tm != lastTm) {
    for (int i = 1; i <= 4; i++) {
      dmxData1[i] = 0;
      dmxData2[i] = 0;
    }
    lastTm = tm;
  }

  if (tm != 0) {
    uint16_t t = (millis() / 10) % 512;
    uint8_t v = t < 256 ? t : 511 - t;
    if (tm == 1) {
      for (int i = 1; i <= 4; i++) {
        dmxData1[i] = v;
        dmxData2[i] = v;
      }
    } else {
      static unsigned long lastTestArtnet = 0;
      if (millis() - lastTestArtnet >= 25) {
        lastTestArtnet = millis();
        injectTestArtnet(cfg.universe1, v);
        injectTestArtnet(cfg.universe2, v);
      }
    }
  }

  // Emission DMX periodique
  unsigned long now = millis();
  if (now - lastDmxSendMs >= DMX_REFRESH_MS) {
    lastDmxSendMs = now;
    sendDmx();
  }
#endif
  heartbeat();
}

void loadConfig() {
  prefs.begin(CFG_NAMESPACE, false);
  cfg.wifiSsid = prefs.getString("ssid", "");
  cfg.wifiPass = prefs.getString("pass", "");
  cfg.universe1 = prefs.getUShort("uni1", 0);
  cfg.universe2 = prefs.getUShort("uni2", 1);
  cfg.usbInputEnabled = prefs.getBool("usbin", true);
  cfg.dmx1Enabled = prefs.getBool("dmx1en", true);
  cfg.dmx2Enabled = prefs.getBool("dmx2en", true);
  cfg.artnetEnabled = prefs.getBool("anen", true);
  prefs.end();
}

void saveConfig() {
  prefs.begin(CFG_NAMESPACE, false);
  prefs.putString("ssid", cfg.wifiSsid);
  prefs.putString("pass", cfg.wifiPass);
  prefs.putUShort("uni1", cfg.universe1);
  prefs.putUShort("uni2", cfg.universe2);
  prefs.putBool("usbin", cfg.usbInputEnabled);
  prefs.putBool("dmx1en", cfg.dmx1Enabled);
  prefs.putBool("dmx2en", cfg.dmx2Enabled);
  prefs.putBool("anen", cfg.artnetEnabled);
  prefs.end();
}

// Series port for DMX
void setupDmxOutputs() {
  memset(dmxData1, 0, 513);
  memset(dmxData2, 0, 513);
  memset(dmxUsbData, 0, 513);
  pinMode(DMX1_RTS_PIN, OUTPUT);
  pinMode(DMX2_RTS_PIN, OUTPUT);
  applyOutputState();
  dmxPort1.begin(250000, SERIAL_8N2, DMX1_RX_PIN, DMX1_TX_PIN);
  dmxPort2.begin(250000, SERIAL_8N2, DMX2_RX_PIN, DMX2_TX_PIN);
  DBG.printf("[DMX] S1 TX=%d RTS=%d RX=%d | S2 TX=%d RTS=%d RX=%d | 250000 bauds 8N2\n",
             DMX1_TX_PIN, DMX1_RTS_PIN, DMX1_RX_PIN,
             DMX2_TX_PIN, DMX2_RTS_PIN, DMX2_RX_PIN);
}

void applyOutputState() {
  digitalWrite(DMX1_RTS_PIN, cfg.dmx1Enabled ? HIGH : LOW);
  digitalWrite(DMX2_RTS_PIN, cfg.dmx2Enabled ? HIGH : LOW);
  DBG.printf("[DMX] sortie 1 = %s (RTS=%s) | sortie 2 = %s (RTS=%s)\n",
             onOff(cfg.dmx1Enabled), cfg.dmx1Enabled ? "HIGH" : "LOW",
             onOff(cfg.dmx2Enabled), cfg.dmx2Enabled ? "HIGH" : "LOW");
}

void sendDmx() {
  if (cfg.dmx1Enabled) {
    dmxPort1.flush();
    dmxPort1.updateBaudRate(57600);
    dmxPort1.write(0);
    dmxPort1.flush();
    dmxPort1.updateBaudRate(250000);
    dmxPort1.write(dmxData1, 513);
    dmxFrames1++;
  }

  if (cfg.dmx2Enabled) {
    dmxPort2.flush();
    dmxPort2.updateBaudRate(57600);
    dmxPort2.write(0);
    dmxPort2.flush();
    dmxPort2.updateBaudRate(250000);
    dmxPort2.write(dmxData2, 513);
    dmxFrames2++;
  }

  logDmxStats();
}

void logDmxStats() {
  static unsigned long last = 0;
  static unsigned long prev1 = 0;
  static unsigned long prev2 = 0;
  unsigned long now = millis();
  if (now - last < 2000) return;
  float dt = (now - last) / 1000.0f;
  last = now;
  DBG.printf("[DMX] S1 %s src=%s %.1f tr/s ch1-4=%u,%u,%u,%u\n",
             onOff(cfg.dmx1Enabled), dmxSourceName(1), (dmxFrames1 - prev1) / dt,
             dmxData1[1], dmxData1[2], dmxData1[3], dmxData1[4]);
  DBG.printf("[DMX] S2 %s src=%s %.1f tr/s ch1-4=%u,%u,%u,%u\n",
             onOff(cfg.dmx2Enabled), dmxSourceName(2), (dmxFrames2 - prev2) / dt,
             dmxData2[1], dmxData2[2], dmxData2[3], dmxData2[4]);
  prev1 = dmxFrames1;
  prev2 = dmxFrames2;
}

void setupWifiArtnet() {
  WiFi.setSleep(false);
  wifiUdp.begin(ARTNET_PORT);
  DBG.printf("[ARTNET] ecoute Wi-Fi UDP %d\n", ARTNET_PORT);
}

void pollWifiArtnet() {
  for (int i = 0; i < 8; i++) {
    int packetSize = wifiUdp.parsePacket();
    if (packetSize <= 0) return;
    wifiPktCount++;
    if (!cfg.artnetEnabled) {
      artnetIgnored++;
      wifiUdp.flush();
      continue;
    }
    if (packetSize > ARTNET_MAX_SIZE) packetSize = ARTNET_MAX_SIZE;
    int n = wifiUdp.read(artnetBuffer, packetSize);
    if (n < 10 || memcmp(artnetBuffer, ARTNET_ID, 8) != 0) continue;
    uint16_t opcode = artnetBuffer[8] | (artnetBuffer[9] << 8);
    if (opcode == ARTNET_OP_DMX) handleArtnetPacket(artnetBuffer, n);
  }
}

void handleArtnetPacket(uint8_t *buf, int len) {
  if (len < 10) return;
  if (memcmp(buf, ARTNET_ID, 8) != 0) return;
  uint16_t opcode = buf[8] | (buf[9] << 8);

  // ArtDMX
  if (opcode != ARTNET_OP_DMX) return;
  if (len < ARTNET_HEADER_SZ) return;
  uint8_t subUni = buf[14];
  uint8_t net    = buf[15] & 0x7F;
  uint16_t universe = (net << 8) | subUni;
  lastUniverseSeen = universe;
  uint16_t dmxLen = (buf[16] << 8) | buf[17];
  if (dmxLen > 512) dmxLen = 512;
  if (len < ARTNET_HEADER_SZ + dmxLen) return;
  uint8_t *dmxPayload = buf + ARTNET_HEADER_SZ;

  if (universe == cfg.universe1) {
    memcpy(&dmxData1[1], dmxPayload, dmxLen);
    artnetDmxCount1++;
    if (!artnetSeen1) {
      artnetSeen1 = true;
      DBG.printf("[ARTNET] premier ArtDMX univers %u -> sortie 1 (%u canaux)\n", universe, dmxLen);
    }
  } else if (universe == cfg.universe2) {
    memcpy(&dmxData2[1], dmxPayload, dmxLen);
    artnetDmxCount2++;
    if (!artnetSeen2) {
      artnetSeen2 = true;
      DBG.printf("[ARTNET] premier ArtDMX univers %u -> sortie 2 (%u canaux)\n", universe, dmxLen);
    }
  } else {
    static uint16_t lastIgnored = 0xFFFF;
    if (universe != lastIgnored) {
      lastIgnored = universe;
      DBG.printf("[ARTNET] ArtDMX univers %u ignore (attendu %u ou %u)\n",
                 universe, cfg.universe1, cfg.universe2);
    }
  }
}

void setupUsbDmx() {
  usbState = USB_WAIT_START;
}

void sendEnttecReply(uint8_t label, const uint8_t *data, uint16_t len) {
  Serial.write(ENTTEC_START);
  Serial.write(label);
  Serial.write((uint8_t)(len & 0xFF));
  Serial.write((uint8_t)((len >> 8) & 0xFF));
  if (len) Serial.write(data, len);
  Serial.write(ENTTEC_END);
}

void handleUsbDmxPacket(uint8_t label, uint8_t *data, uint16_t len) {
  switch (label) {
    case ENTTEC_LABEL_SEND_DMX:
      if (len >= 1) {
        uint16_t n = len - 1;
        if (n > 512) n = 512;
        memcpy(&dmxUsbData[1], &data[1], n);
        lastUsbDmxMs = millis();
        usbPacketCount++;
        if (usbPacketCount == 1) {
          DBG.printf("[USB] premier paquet DMX recu (%u octets)\n", len);
        }
      }
      break;
    case ENTTEC_LABEL_GET_WIDGET_PARAMS: {
      uint8_t reply[5] = { 1, 0, 9, 9, 40 };
      DBG.println(F("[USB] GET_WIDGET_PARAMS recu -> reponse envoyee"));
      sendEnttecReply(ENTTEC_LABEL_GET_WIDGET_PARAMS, reply, sizeof(reply));
      break;
    }
    default:
      DBG.printf("[USB] label %u non gere (%u octets)\n", label, len);
      break;
  }
}

void pollUsbDmx() {
  while (Serial.available()) {
    uint8_t b = (uint8_t)Serial.read();
    switch (usbState) {
      case USB_WAIT_START:
        if (b == ENTTEC_START) usbState = USB_WAIT_LABEL;
        break;
      case USB_WAIT_LABEL:
        usbLabel = b;
        usbState = USB_WAIT_LEN_LO;
        break;
      case USB_WAIT_LEN_LO:
        usbLen = b;
        usbState = USB_WAIT_LEN_HI;
        break;
      case USB_WAIT_LEN_HI:
        usbLen |= ((uint16_t)b << 8);
        if (usbLen > sizeof(usbPacket)) {
          usbState = USB_WAIT_START;
        } else if (usbLen == 0) {
          usbState = USB_WAIT_END;
        } else {
          usbIndex = 0;
          usbState = USB_WAIT_DATA;
        }
        break;
      case USB_WAIT_DATA:
        usbPacket[usbIndex++] = b;
        if (usbIndex >= usbLen) usbState = USB_WAIT_END;
        break;
      case USB_WAIT_END:
        if (b == ENTTEC_END) {
          handleUsbDmxPacket(usbLabel, usbPacket, usbLen);
        }
        usbState = USB_WAIT_START;
        break;
    }
  }
}

// Wi-Fi and web portal
void setupWifiEvents() {
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_AP_START:
        DBG.println(F("[WiFi] Point d'acces demarre"));
        break;
      case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
        const uint8_t *m = info.wifi_ap_staconnected.mac;
        DBG.printf("[WiFi] Client connecte : %02X:%02X:%02X:%02X:%02X:%02X\n",
                   m[0], m[1], m[2], m[3], m[4], m[5]);
        break;
      }
      case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
        DBG.println(F("[WiFi] Client deconnecte"));
        break;
      case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
        DBG.printf("[WiFi] DHCP : IP donnee au client = %s\n",
                   IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
        break;
      default:
        break;
    }
  });
}

void setupWifiPortal() {
  bool connected = false;
  setupWifiEvents();

  if (!FORCE_AP_MODE && cfg.wifiSsid.length() > 0) {
    DBG.printf("Connexion Wi-Fi a '%s'...\n", cfg.wifiSsid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_STA_TIMEOUT_MS) {
      delay(250);
    }
    connected = (WiFi.status() == WL_CONNECTED);
  }

  if (connected) {
    apMode = false;
    DBG.print(F("Wi-Fi connecte, IP : "));
    DBG.println(WiFi.localIP());
  } else {
    apMode = true;
    WiFi.mode(WIFI_AP);
    delay(100);
    bool cfgOk = WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    bool apOk  = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    delay(500);
    DBG.printf("softAPConfig = %s / softAP = %s\n", cfgOk ? "OK" : "ECHEC", apOk ? "OK" : "ECHEC");
    DBG.print(F("IP reelle du point d'acces : "));
    DBG.println(WiFi.softAPIP());
    dnsServer.start(53, "*", apIP);
    DBG.print(F("Point d'acces actif : "));
    DBG.println(WIFI_AP_SSID);
    DBG.print(F("Portail web disponible sur : http://"));
    DBG.println(apIP);
  }

  // URL
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.on("/scan", HTTP_GET, handleScanRdm);
  webServer.on("/reboot", HTTP_POST, handleReboot);
  webServer.on("/test_dmx", HTTP_POST, handleTestDmx);
  webServer.on("/test_artnet", HTTP_POST, handleTestArtnet);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  DBG.println(F("Serveur Web demarre !"));
}

void logReq(const char *tag) {
  webReqCount++;
  DBG.printf("[HTTP] %s %s depuis %s (Host: %s)\n", tag, webServer.uri().c_str(),
             webServer.client().remoteIP().toString().c_str(),
             webServer.hostHeader().c_str());
}

void handlePortal() {
  if (apMode) dnsServer.processNextRequest();
  webServer.handleClient();
}

void handleNotFound() {
  logReq("404->redirect");
  if (apMode) {
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
  } else {
    webServer.send(404, "text/plain", "Not found");
  }
}

String rdmUidsToHtml(int portIndex) {
  return "<p>RDM non pris en charge en mode Serial natif.</p>";
}

// HTML page for configuration
void handleRoot() {
  logReq("GET");
  String html;
  html.reserve(6000);
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>DMX-Unit-DIY</title><style>body{font-family:sans-serif;max-width:640px;margin:20px auto;padding:0 12px;}fieldset{margin-bottom:16px;}label{display:block;margin-top:8px;}input[type=text],input[type=number],input[type=password]{width:100%;box-sizing:border-box;padding:6px;}button{padding:8px 16px;margin-top:8px;}</style></head><body>");
  html += F("<h1>DMX-Unit-DIY</h1>");
  html += "<h2>Etat</h2><ul>";
  if (apMode) {
    html += "<li>Wi-Fi : point d'acces " WIFI_AP_SSID "</li>";
  } else {
    html += "<li>Wi-Fi : connecte, IP " + WiFi.localIP().toString() + "</li>";
  }
  html += "<li>Sortie DMX 1 : " + String(onOff(cfg.dmx1Enabled)) + " - source " + dmxSourceName(1) + "</li>";
  html += "<li>Sortie DMX 2 : " + String(onOff(cfg.dmx2Enabled)) + " - source " + dmxSourceName(2) + "</li>";
  html += "<li>Art-Net : " + String(onOff(cfg.artnetEnabled)) + "</li>";
  html += "<li>Paquets Wi-Fi recus : " + String(wifiPktCount) + " | ArtDMX sortie 1 : " + String(artnetDmxCount1) + " | sortie 2 : " + String(artnetDmxCount2) + " | ignores : " + String(artnetIgnored) + "</li>";
  html += "<li>Dernier univers Art-Net recu : " + (lastUniverseSeen == 0xFFFF ? String("aucun") : String(lastUniverseSeen)) + " (attendu " + String(cfg.universe1) + " ou " + String(cfg.universe2) + ")</li>";
  html += "<li>Trames DMX envoyees : sortie 1 = " + String(dmxFrames1) + ", sortie 2 = " + String(dmxFrames2) + " | canaux 1-4 sortie 1 = " + String(dmxData1[1]) + "," + String(dmxData1[2]) + "," + String(dmxData1[3]) + "," + String(dmxData1[4]) + "</li>";
  html += "</ul>";
  html += "<form method='POST' action='/save'>";
  html += F("<fieldset><legend>Activation</legend>");
  html += "<label><input type='checkbox' name='dmx1en' " + String(cfg.dmx1Enabled ? "checked" : "") + "> Sortie DMX 1</label>";
  html += "<label><input type='checkbox' name='dmx2en' " + String(cfg.dmx2Enabled ? "checked" : "") + "> Sortie DMX 2</label>";
  html += "<label><input type='checkbox' name='anen' " + String(cfg.artnetEnabled ? "checked" : "") + "> Art-Net</label>";
  html += F("</fieldset>");
  html += F("<fieldset><legend>Wi-Fi (portail de configuration)</legend>");
  html += "<label>SSID<input type='text' name='ssid' value='" + cfg.wifiSsid + "'></label>";
  html += "<label>Mot de passe<input type='password' name='pass' value='" + cfg.wifiPass + "'></label>";
  html += F("</fieldset>");
  html += F("<fieldset><legend>Art-Net</legend>");
  html += "<label>Univers sortie 1<input type='number' name='uni1' min='0' max='32767' value='" + String(cfg.universe1) + "'></label>";
  html += "<label>Univers sortie 2<input type='number' name='uni2' min='0' max='32767' value='" + String(cfg.universe2) + "'></label>";
  html += F("</fieldset>");
  html += F("<fieldset><legend>Entree DMX/USB</legend>");
  html += "<label><input type='checkbox' name='usbin' " + String(cfg.usbInputEnabled ? "checked" : "") + "> Activer l'entree USB (mode Enttec-like) sur la sortie 1</label>";
  html += F("</fieldset>");
  html += F("<button type='submit'>Enregistrer</button></form>");
  html += F("<h2>Tests</h2>");
  html += "<p>Test en cours : " + String(testMode == 1 ? "DMX direct" : (testMode == 2 ? "Art-Net interne" : "aucun")) + " (rampe 0-255-0 sur les canaux 1 a 4)</p>";
  html += "<form method='POST' action='/test_dmx'><button type='submit'>" + String(testMode == 1 ? "Arreter le test DMX" : "Lancer le test DMX") + "</button></form>";
  html += "<form method='POST' action='/test_artnet'><button type='submit'>" + String(testMode == 2 ? "Arreter le test Art-Net" : "Lancer le test Art-Net") + "</button></form>";
  html += F("<h2>RDM</h2>");
  html += "<h3>Sortie 1 (univers " + String(cfg.universe1) + ")</h3>";
  html += "<form method='GET' action='/scan'><input type='hidden' name='port' value='1'><button type='submit'>Scanner</button></form>";
  html += rdmUidsToHtml(0);
  html += "<h3>Sortie 2 (univers " + String(cfg.universe2) + ")</h3>";
  html += "<form method='GET' action='/scan'><input type='hidden' name='port' value='2'><button type='submit'>Scanner</button></form>";
  html += rdmUidsToHtml(1);
  html += F("<h2>Divers</h2><form method='POST' action='/reboot'><button type='submit'>Redemarrer</button></form>");
  html += F("</body></html>");
  webServer.send(200, "text/html", html);
}

void handleSave() {
  logReq("POST");
  if (webServer.hasArg("ssid"))  cfg.wifiSsid = webServer.arg("ssid");
  if (webServer.hasArg("pass"))  cfg.wifiPass = webServer.arg("pass");
  if (webServer.hasArg("uni1"))  cfg.universe1 = (uint16_t)webServer.arg("uni1").toInt();
  if (webServer.hasArg("uni2"))  cfg.universe2 = (uint16_t)webServer.arg("uni2").toInt();
  cfg.usbInputEnabled = webServer.hasArg("usbin");
  cfg.dmx1Enabled = webServer.hasArg("dmx1en");
  cfg.dmx2Enabled = webServer.hasArg("dmx2en");
  cfg.artnetEnabled = webServer.hasArg("anen");
  saveConfig();
  DBG.printf("[WEB] config enregistree : DMX1=%s DMX2=%s ARTNET=%s USB=%s univ1=%u univ2=%u\n",
             onOff(cfg.dmx1Enabled), onOff(cfg.dmx2Enabled), onOff(cfg.artnetEnabled),
             onOff(cfg.usbInputEnabled), cfg.universe1, cfg.universe2);
  applyOutputState();
  String page = F("<html><body><h2>Reglages enregistres.</h2><form method='POST' action='/reboot'><button type='submit'>Redemarrer maintenant</button></form><p><a href='/'>Retour</a></p></body></html>");
  webServer.send(200, "text/html", page);
}

void injectTestArtnet(uint16_t universe, uint8_t value) {
  if (!cfg.artnetEnabled) {
    artnetIgnored++;
    return;
  }
  static uint8_t pkt[ARTNET_HEADER_SZ + 512];
  memset(pkt, 0, sizeof(pkt));
  memcpy(pkt, ARTNET_ID, 8);
  pkt[8] = ARTNET_OP_DMX & 0xFF;
  pkt[9] = ARTNET_OP_DMX >> 8;
  pkt[11] = 14;
  pkt[14] = universe & 0xFF;
  pkt[15] = (universe >> 8) & 0x7F;
  pkt[16] = 512 >> 8;
  pkt[17] = 512 & 0xFF;
  for (int i = 0; i < 4; i++) pkt[ARTNET_HEADER_SZ + i] = value;
  handleArtnetPacket(pkt, sizeof(pkt));
}

void handleTestToggle(uint8_t mode) {
  logReq("POST");
  testMode = (testMode == mode) ? 0 : mode;
  DBG.printf("[TEST] mode = %u (0=aucun 1=DMX 2=Art-Net)\n", (unsigned)testMode);
  webServer.sendHeader("Location", "/", true);
  webServer.send(303, "text/plain", "");
}

void handleTestDmx() {
  handleTestToggle(1);
}

void handleTestArtnet() {
  handleTestToggle(2);
}

void handleScanRdm() {
  webServer.sendHeader("Location", "/", true);
  webServer.send(302, "text/plain", "");
}

// Reboot
void handleReboot() {
  logReq("POST");
  DBG.println(F("[WEB] redemarrage demande"));
  webServer.send(200, "text/html", "<html><body>Redemarrage...</body></html>");
  delay(300);
  ESP.restart();
}
