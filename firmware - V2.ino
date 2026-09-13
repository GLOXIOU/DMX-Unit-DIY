#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

#define ETH_SCLK_PIN   12
#define ETH_MISO_PIN   13
#define ETH_MOSI_PIN   11
#define ETH_CS_PIN     10
#define ETH_RST_PIN    9

#define DMX1_TX_PIN    17
#define DMX1_RTS_PIN   4
#define DMX1_RX_PIN    16

#define DMX2_TX_PIN    18
#define DMX2_RTS_PIN   5
#define DMX2_RX_PIN    15

#define FAN_PIN         6
#define FAN_PWM_FREQ    25000
#define FAN_PWM_RES     8

HardwareSerial DebugSerial(0);
#define DBG DebugSerial

#define WIFI_AP_SSID        "DMX-Unit-Setup"
#define WIFI_AP_PASSWORD    "dmxsetup1"
#define WIFI_STA_TIMEOUT_MS 10000UL

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
  bool usbInputEnabled;
};

DmxUnitConfig cfg;
Preferences prefs;

WebServer webServer(80);
DNSServer dnsServer;
bool apMode = false;
IPAddress apIP(192, 168, 4, 1);

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
void handleNotFound();
String rdmUidsToHtml(int portIndex);

void setup() {
  DebugSerial.begin(115200);
  Serial.begin(115200);
  delay(200);
  DBG.println();
  DBG.println(F("DMX-Unit-DIY - firmware V2"));
  loadConfig();
  setupFan();
  setupDmxOutputs();
  setupEthernet();
  setupArtnet();
  setupWifiPortal();
  setupUsbDmx();
}

void loop() {
  Ethernet.maintain();
  pollArtnet();
  pollUsbDmx();
  handlePortal();

  bool usbLive = cfg.usbInputEnabled && (millis() - lastUsbDmxMs < USB_DMX_TIMEOUT_MS);
  if (usbLive) {
    memcpy(dmxData1, dmxUsbData, 513);
  }

  unsigned long now = millis();
  if (now - lastDmxSendMs >= DMX_REFRESH_MS) {
    lastDmxSendMs = now;
    sendDmx();
  }
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
  cfg.usbInputEnabled = prefs.getBool("usbin", true);
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
  prefs.putBool("usbin", cfg.usbInputEnabled);
  prefs.end();
}

IPAddress strToIp(const String &s, IPAddress fallback) {
  IPAddress result;
  if (result.fromString(s)) return result;
  return fallback;
}

void setupFan() {
  ledcAttach(FAN_PIN, FAN_PWM_FREQ, FAN_PWM_RES);
  applyFanDuty(cfg.fanDuty);
}

void applyFanDuty(uint8_t duty) {
  ledcWrite(FAN_PIN, duty);
}

void setupDmxOutputs() {
  memset(dmxData1, 0, 513);
  memset(dmxData2, 0, 513);
  memset(dmxUsbData, 0, 513);
  pinMode(DMX1_RTS_PIN, OUTPUT);
  pinMode(DMX2_RTS_PIN, OUTPUT);
  digitalWrite(DMX1_RTS_PIN, HIGH);
  digitalWrite(DMX2_RTS_PIN, HIGH);
  dmxPort1.begin(250000, SERIAL_8N2, DMX1_RX_PIN, DMX1_TX_PIN);
  dmxPort2.begin(250000, SERIAL_8N2, DMX2_RX_PIN, DMX2_TX_PIN);
}

void sendDmx() {
  dmxPort1.flush();
  dmxPort1.updateBaudRate(57600);
  dmxPort1.write(0);
  dmxPort1.flush();
  dmxPort1.updateBaudRate(250000);
  dmxPort1.write(dmxData1, 513);
  
  dmxPort2.flush();
  dmxPort2.updateBaudRate(57600);
  dmxPort2.write(0);
  dmxPort2.flush();
  dmxPort2.updateBaudRate(250000);
  dmxPort2.write(dmxData2, 513);
}

void setupEthernet() {
  pinMode(ETH_RST_PIN, OUTPUT);
  digitalWrite(ETH_RST_PIN, LOW);
  delay(50);
  digitalWrite(ETH_RST_PIN, HIGH);
  delay(200);

  SPI.begin(ETH_SCLK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN, ETH_CS_PIN);
  Ethernet.init(ETH_CS_PIN);

  DBG.println(F("Initialisation Ethernet..."));
  
  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    DBG.println(F("ERREUR: W5500 non detecte. Poursuite en Wi-Fi seul."));
    return;
  }

  Ethernet.begin(macAddress, cfg.ip, cfg.gw, cfg.gw, cfg.sn);
  DBG.print(F("IP Ethernet (Art-Net): "));
  DBG.println(Ethernet.localIP());
}

void setupArtnet() {
  artnetUdp.begin(ARTNET_PORT);
}

void pollArtnet() {
  int packetSize = artnetUdp.parsePacket();
  if (packetSize <= 0) return;
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
    sendArtPollReply(bcast);
    return;
  }
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
  } else if (universe == cfg.universe2) {
    memcpy(&dmxData2[1], dmxPayload, dmxLen);
  }
}

IPAddress calcBroadcastAddress(IPAddress ip, IPAddress sn) {
  uint32_t ipN = (uint32_t)ip;
  uint32_t snN = (uint32_t)sn;
  uint32_t bcastN = ipN | (~snN);
  return IPAddress(bcastN);
}

void sendArtPollReply(IPAddress dest) {
  memset(pollReplyBuf, 0, sizeof(pollReplyBuf));
  memcpy(pollReplyBuf, ARTNET_ID, 8);
  pollReplyBuf[8] = 0x00;
  pollReplyBuf[9] = 0x21;
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
      }
      break;
    case ENTTEC_LABEL_GET_WIDGET_PARAMS: {
      uint8_t reply[5] = { 1, 0, 9, 9, 40 };
      sendEnttecReply(ENTTEC_LABEL_GET_WIDGET_PARAMS, reply, sizeof(reply));
      break;
    }
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

void setupWifiPortal() {
  bool connected = false;

  if (cfg.wifiSsid.length() > 0) {
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
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    dnsServer.start(53, "*", apIP);
    DBG.print(F("Point d'acces actif : "));
    DBG.println(WIFI_AP_SSID);
    DBG.print(F("Portail web disponible sur : http://"));
    DBG.println(apIP);
  }

  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.on("/scan", HTTP_GET, handleScanRdm);
  webServer.on("/reboot", HTTP_POST, handleReboot);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  DBG.println(F("Serveur Web demarre !"));
}

void handlePortal() {
  if (apMode) dnsServer.processNextRequest();
  webServer.handleClient();
}

void handleNotFound() {
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

void handleRoot() {
  String html;
  html.reserve(6000);
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>DMX-Unit-DIY</title><style>body{font-family:sans-serif;max-width:640px;margin:20px auto;padding:0 12px;}fieldset{margin-bottom:16px;}label{display:block;margin-top:8px;}input[type=text],input[type=number],input[type=password]{width:100%;box-sizing:border-box;padding:6px;}button{padding:8px 16px;margin-top:8px;}</style></head><body>");
  html += F("<h1>DMX-Unit-DIY</h1>");
  html += "<h2>Etat</h2><ul>";
  html += "<li>IP Ethernet (Art-Net) : " + Ethernet.localIP().toString() + "</li>";
  if (apMode) {
    html += "<li>Wi-Fi : point d'acces " WIFI_AP_SSID "</li>";
  } else {
    html += "<li>Wi-Fi : connecte, IP " + WiFi.localIP().toString() + "</li>";
  }
  bool usbLive = (millis() - lastUsbDmxMs < USB_DMX_TIMEOUT_MS);
  html += "<li>Source sortie 1 : " + String(usbLive ? "USB (Enttec-like)" : "Art-Net") + "</li>";
  html += "<li>Source sortie 2 : Art-Net</li>";
  html += "</ul>";
  html += "<form method='POST' action='/save'>";
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
  saveConfig();
  applyFanDuty(cfg.fanDuty);
  String page = F("<html><body><h2>Reglages enregistres.</h2><form method='POST' action='/reboot'><button type='submit'>Redemarrer maintenant</button></form><p><a href='/'>Retour</a></p></body></html>");
  webServer.send(200, "text/html", page);
}

void handleScanRdm() {
  webServer.sendHeader("Location", "/", true);
  webServer.send(302, "text/plain", "");
}

void handleReboot() {
  webServer.send(200, "text/html", "<html><body>Redemarrage...</body></html>");
  delay(300);
  ESP.restart();
}