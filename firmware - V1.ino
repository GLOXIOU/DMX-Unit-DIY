#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <esp_dmx.h>

// W5500
#define ETH_SCLK_PIN   12
#define ETH_MISO_PIN   13
#define ETH_MOSI_PIN   11
#define ETH_CS_PIN     10
#define ETH_RST_PIN    9

// MAX485
#define DMX1_TX_PIN    17
#define DMX1_RTS_PIN   4
#define DMX1_RX_PIN    16

#define DMX2_TX_PIN    18
#define DMX2_RTS_PIN   5
#define DMX2_RX_PIN    15

// Fan
#define FAN_PIN        6
#define FAN_PWM_CHANNEL 0
#define FAN_PWM_FREQ    25000
#define FAN_PWM_RES     8
#define FAN_DUTY_FIXED  180

// Net
byte macAddress[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01 };
IPAddress staticIp(192, 168, 1, 77);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

// Art-Net
#define ARTNET_PORT      6454
#define ARTNET_HEADER_SZ 18
#define ARTNET_MAX_SIZE  530

#define ARTNET_UNIVERSE_OUT1  0
#define ARTNET_UNIVERSE_OUT2  1

EthernetUDP artnetUdp;
uint8_t artnetBuffer[ARTNET_MAX_SIZE];

const uint8_t ARTNET_ID[8] = { 'A', 'r', 't', '-', 'N', 'e', 't', 0 };

// DMX
const dmx_port_t dmxPort1 = DMX_NUM_1;
const dmx_port_t dmxPort2 = DMX_NUM_2;

uint8_t dmxData1[DMX_PACKET_SIZE];
uint8_t dmxData2[DMX_PACKET_SIZE];

unsigned long lastDmxSendMs = 0;
const unsigned long DMX_REFRESH_MS = 25;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== DMX-Unit-DIY - firmware partiel ==="));

  setupFan();
  setupDmxOutputs();
  setupEthernet();
  setupArtnet();

  Serial.println(F("Pret. En attente de paquets Art-Net..."));
}

void loop() {
  Ethernet.maintain();

  pollArtnet();

  unsigned long now = millis();
  if (now - lastDmxSendMs >= DMX_REFRESH_MS) {
    lastDmxSendMs = now;
    sendDmx();
  }
}

//  Ethernet
void setupEthernet() {
  pinMode(ETH_RST_PIN, OUTPUT);
  digitalWrite(ETH_RST_PIN, LOW);
  delay(50);
  digitalWrite(ETH_RST_PIN, HIGH);
  delay(200);

  SPI.begin(ETH_SCLK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN, ETH_CS_PIN);
  Ethernet.init(ETH_CS_PIN);

  Ethernet.begin(macAddress, staticIp, gateway, gateway, subnet);

  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    Serial.println(F("ERREUR: W5500 non detecte, verifiez le cablage SPI."));
  }

  Serial.print(F("IP: "));
  Serial.println(Ethernet.localIP());
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
  if (len < ARTNET_HEADER_SZ) return;
  if (memcmp(buf, ARTNET_ID, 8) != 0) return;

  uint16_t opcode = buf[8] | (buf[9] << 8);
  if (opcode != 0x5000) return;

  uint8_t subUni = buf[14];
  uint8_t net    = buf[15] & 0x7F;
  uint16_t universe = (net << 8) | subUni;

  uint16_t dmxLen = (buf[16] << 8) | buf[17];
  if (dmxLen > 512) dmxLen = 512;
  if (len < ARTNET_HEADER_SZ + dmxLen) return;

  uint8_t *dmxPayload = buf + ARTNET_HEADER_SZ;

  if (universe == ARTNET_UNIVERSE_OUT1) {
    memcpy(&dmxData1[1], dmxPayload, dmxLen);
  } else if (universe == ARTNET_UNIVERSE_OUT2) {
    memcpy(&dmxData2[1], dmxPayload, dmxLen);
  }
}

//  DMX512
void setupDmxOutputs() {
  memset(dmxData1, 0, sizeof(dmxData1));
  memset(dmxData2, 0, sizeof(dmxData2));

  dmx_config_t config = DMX_CONFIG_DEFAULT;

  dmx_driver_install(dmxPort1, &config, DMX_INTR_FLAGS_DEFAULT);
  dmx_set_pin(dmxPort1, DMX1_TX_PIN, DMX1_RX_PIN, DMX1_RTS_PIN);

  dmx_driver_install(dmxPort2, &config, DMX_INTR_FLAGS_DEFAULT);
  dmx_set_pin(dmxPort2, DMX2_TX_PIN, DMX2_RX_PIN, DMX2_RTS_PIN);
}

void sendDmx() {
  dmx_write(dmxPort1, dmxData1, DMX_PACKET_SIZE);
  dmx_send(dmxPort1);

  dmx_write(dmxPort2, dmxData2, DMX_PACKET_SIZE);
  dmx_send(dmxPort2);

  dmx_wait_sent(dmxPort1, DMX_TIMEOUT_TICK);
  dmx_wait_sent(dmxPort2, DMX_TIMEOUT_TICK);
}

//  Fan
void setupFan() {
  ledcSetup(FAN_PWM_CHANNEL, FAN_PWM_FREQ, FAN_PWM_RES);
  ledcAttachPin(FAN_PIN, FAN_PWM_CHANNEL);
  ledcWrite(FAN_PWM_CHANNEL, FAN_DUTY_FIXED);
}
