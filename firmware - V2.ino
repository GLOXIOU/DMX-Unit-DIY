#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// W5500
#define ETH_SCLK_PIN   12
#define ETH_MISO_PIN   13
#define ETH_MOSI_PIN   11
#define ETH_CS_PIN     10
#define ETH_RST_PIN    9

// DMX 1
#define DMX1_TX_PIN    17
#define DMX1_RTS_PIN   4
#define DMX1_RX_PIN    16

// DMX 2
#define DMX2_TX_PIN    18
#define DMX2_RTS_PIN   5
#define DMX2_RX_PIN    15

// Fan PWM control
#define FAN_PIN         6
#define FAN_PWM_FREQ    25000
#define FAN_PWM_RES     8

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
#define ARTNET_OP_POLL        0x2000
#define ARTPOLL_REPLY_SIZE    239

const uint8_t ARTNET_ID[8] = { 'A', 'r', 't', '-', 'N', 'e', 't', 0 };

byte macAddress[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01 };

IPAddress DEFAULT_IP(192, 168, 1, 77);
IPAddress DEFAULT_GW(192, 168, 1, 1);
IPAddress DEFAULT_SN(255, 255, 255, 0);

EthernetUDP artnetUdp;
uint8_t artnetBuffer[ARTNET_MAX_SIZE];
uint8_t pollReplyBuf[ARTPOLL_REPLY_SIZE];

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
  IPAddress ip;
  IPAddress gw;
  IPAddress sn;
  String wifiSsid;
  String wifiPass;
  uint16_t universe1;
  uint16_t universe2;
  String shortName;
  String longName;
  uint8_t fanDuty;
  bool fanEnabled;
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
bool ethReady = false;
unsigned long webReqCount = 0;

unsigned long dmxFrames1 = 0;
unsigned long dmxFrames2 = 0;
unsigned long artnetPollCount = 0;
unsigned long artnetDmxCount1 = 0;
unsigned long artnetDmxCount2 = 0;
unsigned long artnetIgnored = 0;
unsigned long usbPacketCount = 0;
bool usbWasLive = false;
bool artnetSeen1 = false;
bool artnetSeen2 = false;

const char *onOff(bool b) {
  return b ? "ON" : "OFF";
}

const char *ethLinkName() {
  if (!ethReady) return "non initialise";
  switch (Ethernet.linkStatus()) {
    case LinkON:  return "ON";
    case LinkOFF: return "OFF (cable debranche ?)";
    default:      return "inconnu";
  }
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
IPAddress strToIp(const String &s, IPAddress fallback);
void setupFan();
void applyFanDuty(uint8_t duty);
void setupDmxOutputs();
void sendDmx();
void setupEthernet();
void setupArtnet();
void pollArtnet();
void handleArtnetPacket(uint8_t *buf, int len);
IPAddress calcBroadcastAddress(IPAddress ip, IPAddress sn);
void sendArtPollReply(IPAddress dest);
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
void handleFanToggle();
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
  DBG.printf("Config : univers1=%u univers2=%u IP=%s\n",
             cfg.universe1, cfg.universe2, cfg.ip.toString().c_str());
  DBG.printf("Options : DEBUG_ON_USB=%d FORCE_AP=%d\n",
             DEBUG_ON_USB, FORCE_AP_MODE);

#if PORTAL_ONLY_TEST
  DBG.println(F("MODE TEST : DMX / Ethernet / Art-Net / USB desactives"));
#else
  setupFan();
  setupDmxOutputs();
  setupEthernet();
  if (ethReady) {
    setupArtnet();
  } else {
    DBG.println(F("[ARTNET] inactif : Ethernet non pret"));
  }
#endif

  setupWifiPortal();

#if !PORTAL_ONLY_TEST
  setupUsbDmx();
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
  DBG.printf("   DMX1=%s DMX2=%s ARTNET=%s | ETH lien=%s\n",
             onOff(cfg.dmx1Enabled), onOff(cfg.dmx2Enabled),
             onOff(cfg.artnetEnabled), ethLinkName());
  DBG.printf("   ArtDMX u1=%lu u2=%lu polls=%lu ignores=%lu | USB paquets=%lu\n",
             artnetDmxCount1, artnetDmxCount2, artnetPollCount,
             artnetIgnored, usbPacketCount);
}

void loop() {
#if PORTAL_ONLY_TEST
  handlePortal();
#else
  if (ethReady) {
    Ethernet.maintain();
    pollArtnet();
  }
  pollUsbDmx();

  bool usbLive = isUsbLive();
  if (usbLive != usbWasLive) {
    usbWasLive = usbLive;
    DBG.printf("[USB] source USB %s\n", usbLive ? "ACTIVE (sortie 1 pilotee par USB)" : "PERDUE (retour Art-Net)");
  }
  if (usbLive) {
    memcpy(dmxData1, dmxUsbData, 513);
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
  cfg.ip = IPAddress(prefs.getUInt("ip", (uint32_t)DEFAULT_IP));
  cfg.gw = IPAddress(prefs.getUInt("gw", (uint32_t)DEFAULT_GW));
  cfg.sn = IPAddress(prefs.getUInt("sn", (uint32_t)DEFAULT_SN));
  cfg.wifiSsid = prefs.getString("ssid", "");
  cfg.wifiPass = prefs.getString("pass", "");
  cfg.universe1 = prefs.getUShort("uni1", 0);
  cfg.universe2 = prefs.getUShort("uni2", 1);
  cfg.shortName = prefs.getString("sname", "DMX-Unit-DIY");
  cfg.longName  = prefs.getString("lname", "DMX-Unit-DIY - Art-Net Node");
  cfg.fanDuty = prefs.getUChar("fanduty", 180);
  cfg.fanEnabled = prefs.getBool("fanen", true);
  cfg.usbInputEnabled = prefs.getBool("usbin", true);
  cfg.dmx1Enabled = prefs.getBool("dmx1en", true);
  cfg.dmx2Enabled = prefs.getBool("dmx2en", true);
  cfg.artnetEnabled = prefs.getBool("anen", true);
  prefs.end();
}

void saveConfig() {
  prefs.begin(CFG_NAMESPACE, false);
  prefs.putUInt("ip", (uint32_t)cfg.ip);
  prefs.putUInt("gw", (uint32_t)cfg.gw);
  prefs.putUInt("sn", (uint32_t)cfg.sn);
  prefs.putString("ssid", cfg.wifiSsid);
  prefs.putString("pass", cfg.wifiPass);
  prefs.putUShort("uni1", cfg.universe1);
  prefs.putUShort("uni2", cfg.universe2);
  prefs.putString("sname", cfg.shortName);
  prefs.putString("lname", cfg.longName);
  prefs.putUChar("fanduty", cfg.fanDuty);
  prefs.putBool("fanen", cfg.fanEnabled);
  prefs.putBool("usbin", cfg.usbInputEnabled);
  prefs.putBool("dmx1en", cfg.dmx1Enabled);
  prefs.putBool("dmx2en", cfg.dmx2Enabled);
  prefs.putBool("anen", cfg.artnetEnabled);
  prefs.end();
}

IPAddress strToIp(const String &s, IPAddress fallback) {
  IPAddress result;
  if (result.fromString(s)) return result;
  return fallback;
}

void setupFan() {
  bool ok = ledcAttach(FAN_PIN, FAN_PWM_FREQ, FAN_PWM_RES);
  DBG.printf("[FAN] ledcAttach pin %d = %s\n", FAN_PIN, ok ? "OK" : "ECHEC (PWM non configure)");
  applyFanDuty(cfg.fanDuty);
}

void applyFanDuty(uint8_t duty) {
  uint8_t out = cfg.fanEnabled ? duty : 0;
  ledcWrite(FAN_PIN, out);
  DBG.printf("[FAN] %s, vitesse appliquee = %u/255 (reglage %u, pin %d)\n",
             onOff(cfg.fanEnabled), out, duty, FAN_PIN);
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

// W5500
void setupEthernet() {
  pinMode(ETH_RST_PIN, OUTPUT);
  digitalWrite(ETH_RST_PIN, LOW);
  delay(50);
  digitalWrite(ETH_RST_PIN, HIGH);
  delay(200);

  SPI.begin(ETH_SCLK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN, ETH_CS_PIN);
  Ethernet.init(ETH_CS_PIN);

  DBG.println(F("Initialisation Ethernet..."));

  Ethernet.begin(macAddress, cfg.ip, cfg.gw, cfg.gw, cfg.sn);
  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    DBG.println(F("ERREUR: W5500 non detecte. Poursuite en Wi-Fi seul."));
    ethReady = false;
    return;
  }
  ethReady = true;
  DBG.print(F("IP Ethernet (Art-Net): "));
  DBG.println(Ethernet.localIP());
  DBG.printf("[ETH] W5500 detecte, lien = %s\n", ethLinkName());
}

// UDP Art-Net
void setupArtnet() {
  artnetUdp.begin(ARTNET_PORT);
  DBG.printf("[ARTNET] ecoute UDP %d (Art-Net %s)\n", ARTNET_PORT, onOff(cfg.artnetEnabled));
}

// UDP packet reception
void pollArtnet() {
  int packetSize = artnetUdp.parsePacket();
  if (packetSize <= 0) return;
  if (!cfg.artnetEnabled) {
    artnetIgnored++;
    artnetUdp.flush();
    return;
  }
  if (packetSize > ARTNET_MAX_SIZE) packetSize = ARTNET_MAX_SIZE;
  artnetUdp.read(artnetBuffer, packetSize);
  handleArtnetPacket(artnetBuffer, packetSize);
}

void handleArtnetPacket(uint8_t *buf, int len) {
  if (len < 10) return;
  if (memcmp(buf, ARTNET_ID, 8) != 0) return;
  uint16_t opcode = buf[8] | (buf[9] << 8);
  
  if (opcode == ARTNET_OP_POLL) {
    IPAddress bcast = calcBroadcastAddress(Ethernet.localIP(), cfg.sn);
    artnetPollCount++;
    DBG.printf("[ARTNET] ArtPoll recu -> reponse envoyee a %s\n", bcast.toString().c_str());
    sendArtPollReply(bcast);
    return;
  }
  
  // ArtDMX
  if (opcode != ARTNET_OP_DMX) return;
  if (len < ARTNET_HEADER_SZ) return;
  uint8_t subUni = buf[14];
  uint8_t net    = buf[15] & 0x7F;
  uint16_t universe = (net << 8) | subUni;
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

IPAddress calcBroadcastAddress(IPAddress ip, IPAddress sn) {
  uint32_t ipN = (uint32_t)ip;
  uint32_t snN = (uint32_t)sn;
  uint32_t bcastN = ipN | (~snN);
  return IPAddress(bcastN);
}

// ArtPollReply
void sendArtPollReply(IPAddress dest) {
  memset(pollReplyBuf, 0, sizeof(pollReplyBuf));
  memcpy(pollReplyBuf, ARTNET_ID, 8);
  pollReplyBuf[8] = 0x00;
  pollReplyBuf[9] = 0x21; // Opcode ArtPollReply
  IPAddress ip = Ethernet.localIP();
  pollReplyBuf[10] = ip[0];
  pollReplyBuf[11] = ip[1];
  pollReplyBuf[12] = ip[2];
  pollReplyBuf[13] = ip[3];
  pollReplyBuf[14] = ARTNET_PORT & 0xFF;
  pollReplyBuf[15] = (ARTNET_PORT >> 8) & 0xFF;
  pollReplyBuf[16] = 0;
  pollReplyBuf[17] = 2;
  pollReplyBuf[18] = (cfg.universe1 >> 8) & 0x7F;
  pollReplyBuf[19] = (cfg.universe1 >> 4) & 0x0F;
  pollReplyBuf[20] = 0x00;
  pollReplyBuf[21] = 0x00;
  pollReplyBuf[22] = 0x00;
  pollReplyBuf[23] = 0xD0;
  pollReplyBuf[24] = 0x00;
  pollReplyBuf[25] = 0x00;
  {
    String sName = cfg.shortName;
    if (sName.length() > 17) sName = sName.substring(0, 17);
    memcpy(&pollReplyBuf[26], sName.c_str(), sName.length());
  }
  {
    String lName = cfg.longName;
    if (lName.length() > 63) lName = lName.substring(0, 63);
    memcpy(&pollReplyBuf[44], lName.c_str(), lName.length());
  }
  {
    const char *report = "#0001 [1] DMX-Unit-DIY pret";
    size_t rlen = strlen(report);
    if (rlen > 63) rlen = 63;
    memcpy(&pollReplyBuf[108], report, rlen);
  }
  pollReplyBuf[172] = 0x00;
  pollReplyBuf[173] = 0x02;
  pollReplyBuf[174] = 0x80;
  pollReplyBuf[175] = 0x80;
  pollReplyBuf[178] = 0x00;
  pollReplyBuf[179] = 0x00;
  pollReplyBuf[182] = 0x80;
  pollReplyBuf[183] = 0x80;
  pollReplyBuf[190] = cfg.universe1 & 0x0F;
  pollReplyBuf[191] = cfg.universe2 & 0x0F;
  pollReplyBuf[200] = 0x00;
  pollReplyBuf[201] = macAddress[0];
  pollReplyBuf[202] = macAddress[1];
  pollReplyBuf[203] = macAddress[2];
  pollReplyBuf[204] = macAddress[3];
  pollReplyBuf[205] = macAddress[4];
  pollReplyBuf[206] = macAddress[5];
  pollReplyBuf[207] = ip[0];
  pollReplyBuf[208] = ip[1];
  pollReplyBuf[209] = ip[2];
  pollReplyBuf[210] = ip[3];
  pollReplyBuf[211] = 1;
  pollReplyBuf[212] = 0;
  artnetUdp.beginPacket(dest, ARTNET_PORT);
  artnetUdp.write(pollReplyBuf, ARTPOLL_REPLY_SIZE);
  artnetUdp.endPacket();
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
  webServer.on("/fan", HTTP_POST, handleFanToggle);
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
  html += "<li>IP Ethernet (Art-Net) : " + (ethReady ? Ethernet.localIP().toString() : String("non initialise")) + "</li>";
  if (apMode) {
    html += "<li>Wi-Fi : point d'acces " WIFI_AP_SSID "</li>";
  } else {
    html += "<li>Wi-Fi : connecte, IP " + WiFi.localIP().toString() + "</li>";
  }
  html += "<li>Sortie DMX 1 : " + String(onOff(cfg.dmx1Enabled)) + " - source " + dmxSourceName(1) + "</li>";
  html += "<li>Sortie DMX 2 : " + String(onOff(cfg.dmx2Enabled)) + " - source " + dmxSourceName(2) + "</li>";
  html += "<li>Art-Net : " + String(onOff(cfg.artnetEnabled)) + "</li>";
  html += "<li>Ventilateur : " + String(onOff(cfg.fanEnabled)) + " (reglage " + String(cfg.fanDuty) + "/255)</li>";
  html += "</ul>";
  html += "<form method='POST' action='/save'>";
  html += F("<fieldset><legend>Activation</legend>");
  html += "<label><input type='checkbox' name='dmx1en' " + String(cfg.dmx1Enabled ? "checked" : "") + "> Sortie DMX 1</label>";
  html += "<label><input type='checkbox' name='dmx2en' " + String(cfg.dmx2Enabled ? "checked" : "") + "> Sortie DMX 2</label>";
  html += "<label><input type='checkbox' name='anen' " + String(cfg.artnetEnabled ? "checked" : "") + "> Art-Net (reception Ethernet)</label>";
  html += F("</fieldset>");
  html += F("<fieldset><legend>Reseau Ethernet (Art-Net)</legend>");
  html += "<label>Adresse IP<input type='text' name='ip' value='" + cfg.ip.toString() + "'></label>";
  html += "<label>Passerelle<input type='text' name='gw' value='" + cfg.gw.toString() + "'></label>";
  html += "<label>Masque de sous-reseau<input type='text' name='sn' value='" + cfg.sn.toString() + "'></label>";
  html += F("</fieldset>");
  html += F("<fieldset><legend>Wi-Fi (portail de configuration)</legend>");
  html += "<label>SSID<input type='text' name='ssid' value='" + cfg.wifiSsid + "'></label>";
  html += "<label>Mot de passe<input type='password' name='pass' value='" + cfg.wifiPass + "'></label>";
  html += F("</fieldset>");
  html += F("<fieldset><legend>Art-Net</legend>");
  html += "<label>Univers sortie 1<input type='number' name='uni1' min='0' max='32767' value='" + String(cfg.universe1) + "'></label>";
  html += "<label>Univers sortie 2<input type='number' name='uni2' min='0' max='32767' value='" + String(cfg.universe2) + "'></label>";
  html += "<label>Nom court (ArtPollReply)<input type='text' name='sname' maxlength='17' value='" + cfg.shortName + "'></label>";
  html += "<label>Nom long (ArtPollReply)<input type='text' name='lname' maxlength='63' value='" + cfg.longName + "'></label>";
  html += F("</fieldset>");
  html += F("<fieldset><legend>Ventilateur</legend>");
  html += "<label>Vitesse (0-255)<input type='number' name='fanduty' min='0' max='255' value='" + String(cfg.fanDuty) + "'></label>";
  html += "<button type='submit' formaction='/fan'>" + String(cfg.fanEnabled ? "Desactiver le ventilateur" : "Activer le ventilateur") + "</button>";
  html += F("</fieldset>");
  html += F("<fieldset><legend>Entree DMX/USB</legend>");
  html += "<label><input type='checkbox' name='usbin' " + String(cfg.usbInputEnabled ? "checked" : "") + "> Activer l'entree USB (mode Enttec-like) sur la sortie 1</label>";
  html += F("</fieldset>");
  html += F("<button type='submit'>Enregistrer</button></form>");
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
  if (webServer.hasArg("ip"))    cfg.ip = strToIp(webServer.arg("ip"), cfg.ip);
  if (webServer.hasArg("gw"))    cfg.gw = strToIp(webServer.arg("gw"), cfg.gw);
  if (webServer.hasArg("sn"))    cfg.sn = strToIp(webServer.arg("sn"), cfg.sn);
  if (webServer.hasArg("ssid"))  cfg.wifiSsid = webServer.arg("ssid");
  if (webServer.hasArg("pass"))  cfg.wifiPass = webServer.arg("pass");
  if (webServer.hasArg("uni1"))  cfg.universe1 = (uint16_t)webServer.arg("uni1").toInt();
  if (webServer.hasArg("uni2"))  cfg.universe2 = (uint16_t)webServer.arg("uni2").toInt();
  if (webServer.hasArg("sname")) cfg.shortName = webServer.arg("sname");
  if (webServer.hasArg("lname")) cfg.longName  = webServer.arg("lname");
  if (webServer.hasArg("fanduty")) {
    int v = webServer.arg("fanduty").toInt();
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    cfg.fanDuty = (uint8_t)v;
  }
  cfg.usbInputEnabled = webServer.hasArg("usbin");
  cfg.dmx1Enabled = webServer.hasArg("dmx1en");
  cfg.dmx2Enabled = webServer.hasArg("dmx2en");
  cfg.artnetEnabled = webServer.hasArg("anen");
  saveConfig();
  DBG.printf("[WEB] config enregistree : DMX1=%s DMX2=%s ARTNET=%s USB=%s univ1=%u univ2=%u fan=%u\n",
             onOff(cfg.dmx1Enabled), onOff(cfg.dmx2Enabled), onOff(cfg.artnetEnabled),
             onOff(cfg.usbInputEnabled), cfg.universe1, cfg.universe2, cfg.fanDuty);
  applyOutputState();
  applyFanDuty(cfg.fanDuty);
  String page = F("<html><body><h2>Reglages enregistres.</h2><form method='POST' action='/reboot'><button type='submit'>Redemarrer maintenant</button></form><p><a href='/'>Retour</a></p></body></html>");
  webServer.send(200, "text/html", page);
}

void handleFanToggle() {
  logReq("POST");
  cfg.fanEnabled = !cfg.fanEnabled;
  saveConfig();
  applyFanDuty(cfg.fanDuty);
  webServer.sendHeader("Location", "/", true);
  webServer.send(303, "text/plain", "");
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