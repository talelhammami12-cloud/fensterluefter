/*
 * Lueftungssystem - ESP32 WiFi + Bluetooth bridge
 * Joy-IT NodeMCU ESP32  <->  FLY-Super8 Pro ("Serial Screen" header)
 *
 *   ESP32 RX2 (GPIO16)  <-  FLY PA9 (TX)
 *   ESP32 TX2 (GPIO17)  ->  FLY PA10 (RX)
 *   ESP32 VIN / GND     <-  FLY 5V / GND
 *
 * What it does:
 *   - opens an open WiFi "Lueftungssystem" (no password)
 *   - every phone/laptop that joins gets the fan app (captive portal pop-up,
 *     or open http://192.168.4.1)
 *   - Bluetooth LE "Lueftungssystem" (Nordic UART service) for the same app
 *   - passes every command line to the FLY board and its answers back
 */
#include <WiFi.h>
#include <driver/uart.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <HTTPUpdateServer.h>      // update over WiFi: http://lueftung.local/update
#include "secrets.h"             // private keys, not in the repo
#include "app_html.h"            // the app, gzip-compressed (made by build_esp.bat)

#include <WiFiClientSecure.h>
#include <PubSubClient.h>

Preferences prefs;               // stores the home WiFi name + password

// ---------------------------------------------------------------- Internet (MQTT relay)
// App and panel meet on a public MQTT broker. The secret key in the topic keeps others out.
static const char *NET_KEY = SECRET_NET_KEY;   // same key as in the app (secrets.h)
static const char *MQ_HOST = "broker.hivemq.com";
static const uint16_t MQ_PORT = 8883;                      // TLS
static WiFiClientSecure mqNet;
static PubSubClient mq(mqNet);
static String mqTopic(const char *k) { return String("lueftung/") + NET_KEY + "/" + k; }

static const char *NAME = "Lueftungssystem";
static const uint32_t FLY_BAUD = 460800;   // must match the FLY firmware
static const int PIN_RX2 = 16, PIN_TX2 = 17;

WebServer web(80);
HTTPUpdateServer updater(true);
static const char *OTA_USER = "admin";
static const char *OTA_PASS = SECRET_OTA_PASS;   // needed to upload new firmware (secrets.h)
DNSServer dns;

// ---------------------------------------------------------------- FLY link
static String lastState;              // newest {"t":"state",...} line
static uint32_t lastStateMs = 0;
static const int NMSG = 16;           // other answers (ok / err) for the web clients
static String msgs[NMSG];
static uint32_t msgSeq = 0;           // number of messages so far

static void toFly(const String &line)
{
    Serial2.print(line);
    Serial2.print('\n');
}

// ---------------------------------------------------------------- Bluetooth
#define NUS_SVC "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   // phone writes here
#define NUS_TX  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   // we notify here
static BLECharacteristic *bleTx = nullptr;
static volatile bool bleOn = false;
static uint16_t bleMtu = 23;
static String bleIn;                  // collects a command line from Bluetooth
static String bleQueue;               // complete lines waiting for the main loop
static portMUX_TYPE bleMux = portMUX_INITIALIZER_UNLOCKED;

class SrvCb : public BLEServerCallbacks
{
    void onConnect(BLEServer *s) override { bleOn = true; }
    void onDisconnect(BLEServer *s) override
    {
        bleOn = false;
        bleMtu = 23;
        BLEDevice::startAdvertising();   // let the next phone connect
    }
    void onMtuChanged(BLEServer *s, esp_ble_gatts_cb_param_t *p) override { bleMtu = p->mtu.mtu; }
};

class RxCb : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *c) override
    {
        String v = String(c->getValue().c_str());
        portENTER_CRITICAL(&bleMux);
        for (size_t i = 0; i < v.length(); i++)
        {
            char ch = v[i];
            if (ch == '\n' || ch == '\r')
            {
                if (bleIn.length()) { bleQueue += bleIn; bleQueue += '\n'; bleIn = ""; }
            }
            else if (bleIn.length() < 160) bleIn += ch;
        }
        portEXIT_CRITICAL(&bleMux);
    }
};

static void bleSend(const String &line)
{
    if (!bleOn || !bleTx) return;
    String s = line + "\n";
    size_t chunk = bleMtu > 23 ? bleMtu - 3 : 20;
    for (size_t i = 0; i < s.length(); i += chunk)
    {
        String part = s.substring(i, i + chunk);
        bleTx->setValue((uint8_t *)part.c_str(), part.length());
        bleTx->notify();
        delay(4);
    }
}

static void bleInit()
{
    BLEDevice::init(NAME);
    BLEDevice::setMTU(247);
    BLEServer *srv = BLEDevice::createServer();
    srv->setCallbacks(new SrvCb());
    BLEService *svc = srv->createService(NUS_SVC);
    bleTx = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
    bleTx->addDescriptor(new BLE2902());
    BLECharacteristic *rx = svc->createCharacteristic(NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    rx->setCallbacks(new RxCb());
    svc->start();
    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SVC);
    adv->setScanResponse(true);
    BLEDevice::startAdvertising();
}

// ---------------------------------------------------------------- lines from the FLY board
static String flyIn;
static uint32_t lastBleState = 0;

static uint32_t lastNetState = 0;

static void fromFly(const String &l)
{
    if (l.startsWith("{\"t\":\"state\""))
    {
        lastState = l;
        lastStateMs = millis();
        if (mq.connected() && millis() - lastNetState >= 1000)   // Internet: 1 update per second
        {
            lastNetState = millis();
            mq.publish(mqTopic("state").c_str(), l.c_str(), true);
        }
        if (millis() - lastBleState >= 500)   // Bluetooth is slow: 2 updates per second
        {
            lastBleState = millis();
            bleSend(l);
        }
    }
    else
    {
        msgs[msgSeq % NMSG] = l;
        msgSeq++;
        bleSend(l);
        if (mq.connected()) mq.publish(mqTopic("msg").c_str(), l.c_str());
    }
}

// ---------------------------------------------------------------- web pages
static void sendApp()
{
    web.sendHeader("Content-Encoding", "gzip");
    web.sendHeader("Cache-Control", "no-cache");
    web.send_P(200, "text/html", (const char *)APP_HTML_GZ, APP_HTML_GZ_LEN);
}

static void handlePoll()
{
    uint32_t since = web.hasArg("since") ? web.arg("since").toInt() : 0;
    if (since > msgSeq || msgSeq - since > NMSG) since = msgSeq > NMSG ? msgSeq - NMSG : 0;
    bool fresh = lastState.length() && millis() - lastStateMs < 3000;   // FLY board answered lately
    String out;
    out.reserve(lastState.length() + 600);
    out = "{\"n\":";
    out += msgSeq;
    out += ",\"state\":";
    if (fresh) { out += '"'; for (char c : lastState) { if (c == '"' || c == '\\') out += '\\'; out += c; } out += '"'; }
    else out += "null";
    out += ",\"msgs\":[";
    for (uint32_t i = since; i < msgSeq; i++)
    {
        if (i > since) out += ',';
        out += '"';
        for (char c : msgs[i % NMSG]) { if (c == '"' || c == '\\') out += '\\'; out += c; }
        out += '"';
    }
    out += "]}";
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "application/json", out);
}

static void handleCmd()
{
    String c = web.hasArg("plain") ? web.arg("plain") : web.arg("c");
    c.trim();
    if (c.length() && c.length() <= 160 && c.indexOf('\n') < 0) toFly(c);
    web.send(200, "text/plain", "ok");
}

static void captive()
{
    // any other address (phone checks like /generate_204) -> the app, so the pop-up opens
    web.sendHeader("Location", "http://" + web.client().localIP().toString() + "/", true);
    web.send(302, "text/plain", "");
}

// ---------------------------------------------------------------- FLY board update (/flyupdate)
// The FLY board gets the command "bootloader" and restarts into ST's built-in ROM bootloader.
// That bootloader listens on the same 2 wires (USART1), 115200 baud, 8 data bits, EVEN parity.
// Protocol: ST application note AN3155.
// The file is passed straight through to the FLY in 256-byte blocks while it arrives,
// so the ESP never has to hold the whole file in its (small) free memory.
static uint8_t flyBlk[256];           // one block on its way to the FLY
static size_t flyFill = 0;            // bytes in flyBlk
static size_t flyOff = 0;             // bytes already written to the FLY
static uint16_t flyErased = 0;        // 128 KB sectors erased so far
static bool flyOk = false;            // still going well?
static bool flyStarted = false;
static String flyLog;

static bool blAck(uint32_t ms)
{
    uint32_t t = millis();
    while (millis() - t < ms)
    {
        if (Serial2.available())
        {
            int c = Serial2.read();
            if (c == 0x79) return true;      // ACK
            if (c == 0x1F) return false;     // NACK
        }
        delay(1);
    }
    return false;
}

static bool blCmd(uint8_t c)
{
    uint8_t b[2] = {c, (uint8_t)(c ^ 0xFF)};
    Serial2.write(b, 2);
    return blAck(1000);
}

static bool blAddr(uint32_t a)
{
    uint8_t b[5] = {(uint8_t)(a >> 24), (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a, 0};
    b[4] = b[0] ^ b[1] ^ b[2] ^ b[3];
    Serial2.write(b, 5);
    return blAck(1000);
}

static bool flyEnterBootloader()
{
    // ask the FLY firmware to restart into the ROM bootloader
    // switch our side to the bootloader settings right away, BEFORE the FLY restarts,
    // so the bootloader never sees noise on the line (it would lock onto a wrong speed)
    toFly("bootloader");
    Serial2.flush();
    Serial2.updateBaudRate(115200);
    uart_set_parity(UART_NUM_2, UART_PARITY_EVEN);
    delay(1000);                              // FLY restarts and starts its ROM bootloader
    while (Serial2.available()) Serial2.read();
    // sync: send 0x7F until the bootloader answers
    for (int i = 0; i < 10; i++)
    {
        Serial2.write(0x7F);
        uint32_t t = millis();
        while (millis() - t < 300)
        {
            if (Serial2.available())
            {
                int c = Serial2.read();
                if (c == 0x79 || c == 0x1F) return true;   // NACK = already synced, also fine
            }
        }
    }
    return false;
}

static bool flyEraseSector(uint16_t sec)
{
    // extended erase of exactly one 128 KB sector (sector 7 = saved settings is never touched)
    if (sec >= 7 || !blCmd(0x44)) return false;
    uint8_t b[5] = {0x00, 0x00, (uint8_t)(sec >> 8), (uint8_t)sec, 0};
    b[4] = b[0] ^ b[1] ^ b[2] ^ b[3];
    Serial2.write(b, 5);
    return blAck(30000);
}

static bool flyWriteBlock()
{
    // erase the next sector before the first block that lands in it
    while (flyOff + 256 > (size_t)flyErased * 131072u)
    {
        if (!flyEraseSector(flyErased)) { flyLog += "erase failed, sector " + String(flyErased) + "\n"; return false; }
        flyErased++;
    }
    for (size_t i = flyFill; i < 256; i++) flyBlk[i] = 0xFF;   // pad the last block
    if (!blCmd(0x31) || !blAddr(0x08000000u + flyOff)) { flyLog += "write refused at " + String(flyOff) + "\n"; return false; }
    uint8_t c = 0xFF;                                          // N-1 = 255
    for (int i = 0; i < 256; i++) c ^= flyBlk[i];
    Serial2.write((uint8_t)0xFF);
    Serial2.write(flyBlk, 256);
    Serial2.write(c);
    if (!blAck(2000)) { flyLog += "write failed at " + String(flyOff) + "\n"; return false; }
    flyOff += 256;
    flyFill = 0;
    return true;
}

static void flyRestoreLink()
{
    Serial2.updateBaudRate(FLY_BAUD);
    uart_set_parity(UART_NUM_2, UART_PARITY_DISABLE);
    delay(1500);                              // FLY boots
    toFly("get");
}

static void handleFlyPage()
{
    if (!web.authenticate(OTA_USER, OTA_PASS)) return web.requestAuthentication();
    web.send(200, "text/html",
             "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
             "<body style='font:16px sans-serif;max-width:420px;margin:24px auto;padding:0 16px'>"
             "<h2>FLY board update</h2><p>Choose <b>lueftungssystem_usb.bin</b>.</p>"
             "<form method=post enctype=multipart/form-data><input type=file name=fw accept=.bin>"
             "<p><button>Upload &amp; flash</button></p></form><p><a href=/>Back to the app</a></p>");
}

static void handleFlyUpload()
{
    HTTPUpload &u = web.upload();
    if (u.status == UPLOAD_FILE_START)
    {
        flyLog = "";
        flyFill = flyOff = 0;
        flyErased = 0;
        flyStarted = web.authenticate(OTA_USER, OTA_PASS);
        if (!flyStarted) { flyOk = false; flyLog = "wrong password\n"; return; }
        flyOk = flyEnterBootloader();
        flyLog += flyOk ? "bootloader OK\n" : "FLY bootloader does not answer\n";
    }
    else if (u.status == UPLOAD_FILE_WRITE && flyOk)
    {
        for (size_t i = 0; i < u.currentSize && flyOk; i++)
        {
            flyBlk[flyFill++] = u.buf[i];
            if (flyFill == 256) flyOk = flyWriteBlock();
        }
    }
    else if (u.status == UPLOAD_FILE_END && flyOk)
    {
        if (flyFill) flyOk = flyWriteBlock();
        if (flyOk)
        {
            flyLog += "written " + String(u.totalSize) + " bytes\n";
            if (blCmd(0x21)) blAddr(0x08000000u);   // start the new program
            flyLog += "started\n";
        }
    }
}

static void handleFlyDone()
{
    if (!web.authenticate(OTA_USER, OTA_PASS)) return web.requestAuthentication();
    if (flyStarted) flyRestoreLink();
    flyStarted = false;
    web.send(200, "text/plain", String(flyOk ? "OK\n" : "ERROR\n") + flyLog);
}

// ---------------------------------------------------------------- home WiFi setup page (/wifi)
static String htmlEsc(const String &s)
{
    String o;
    for (char c : s)
    {
        if (c == '<') o += "&lt;";
        else if (c == '&') o += "&amp;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

static void handleWifiPage()
{
    String ssid = prefs.getString("ssid", "");
    String st = WiFi.status() == WL_CONNECTED
        ? "Verbunden mit <b>" + htmlEsc(ssid) + "</b><br>App im ganzen WLAN: <b>http://" + WiFi.localIP().toString() +
          "</b> oder <b>http://lueftung.local</b>"
        : (ssid.length() ? "Nicht verbunden mit <b>" + htmlEsc(ssid) + "</b> (Passwort falsch oder zu weit weg?)"
                         : "Noch kein Heim-WLAN eingestellt.");
    String opts;
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) opts += "<option value=\"" + htmlEsc(WiFi.SSID(i)) + "\">";
    String h = F("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                 "<title>Heim-WLAN</title><style>body{font:16px sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
                 "input,button{font:inherit;width:100%;padding:10px;margin:6px 0;box-sizing:border-box}"
                 "button{background:#1d7a46;color:#fff;border:0;border-radius:6px}</style>"
                 "<h2>L&uuml;ftung ins Heim-WLAN</h2><p>");
    h += st;
    h += F("</p><form method=post><label>WLAN-Name</label><input name=s list=nets required value=\"");
    h += htmlEsc(ssid);
    h += F("\"><datalist id=nets>");
    h += opts;
    h += F("</datalist><label>Passwort</label><input name=p type=password>"
           "<button>Speichern &amp; verbinden</button></form><p><a href=/>Zur&uuml;ck zur App</a></p>");
    web.send(200, "text/html", h);
}

static void handleWifiSave()
{
    String s = web.arg("s"), p = web.arg("p");
    s.trim();
    prefs.putString("ssid", s);
    prefs.putString("pass", p);
    web.send(200, "text/html",
             "<meta charset=utf-8><meta name=viewport content='width=device-width'>"
             "<p style='font:16px sans-serif;margin:24px'>Gespeichert. Die L&uuml;ftung startet neu und verbindet sich mit "
             "<b>" + htmlEsc(s) + "</b>.<br><br>Danach im Heim-WLAN <b>http://lueftung.local</b> &ouml;ffnen "
             "(oder die Adresse auf http://192.168.4.1/wifi nachsehen).</p>");
    delay(800);
    ESP.restart();
}

// ---------------------------------------------------------------- Internet link
static void onMqtt(char *topic, byte *data, unsigned int len)
{
    if (len == 0 || len > 160) return;
    String c;
    for (unsigned i = 0; i < len; i++) if (data[i] != '\n' && data[i] != '\r') c += (char)data[i];
    toFly(c);
}

static uint32_t lastMqTry = 0;
static void mqLoop()
{
    if (WiFi.status() != WL_CONNECTED) return;        // no home WiFi -> no Internet
    if (mq.connected()) { mq.loop(); return; }
    if (millis() - lastMqTry < 5000) return;          // retry every 5 s
    lastMqTry = millis();
    String id = "panel_" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mq.connect(id.c_str(), nullptr, nullptr, mqTopic("online").c_str(), 0, true, "0"))
    {
        mq.publish(mqTopic("online").c_str(), "1", true);
        mq.subscribe(mqTopic("cmd").c_str());
        toFly("get");
    }
}

// ---------------------------------------------------------------- start
void setup()
{
    Serial.begin(115200);
    Serial2.setRxBufferSize(4096);
    Serial2.begin(FLY_BAUD, SERIAL_8N1, PIN_RX2, PIN_TX2);

    prefs.begin("wifi", false);
    String homeSsid = prefs.getString("ssid", ""), homePass = prefs.getString("pass", "");

    WiFi.mode(WIFI_AP_STA);               // own WiFi + joins the home router
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(NAME);                    // open network, no password
    WiFi.setTxPower(WIFI_POWER_19_5dBm);  // full transmit power
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    if (homeSsid.length()) WiFi.begin(homeSsid.c_str(), homePass.c_str());
    MDNS.begin("lueftung");               // http://lueftung.local in the home WiFi
    MDNS.addService("http", "tcp", 80);

    mqNet.setInsecure();                  // encrypted, no certificate check
    mqNet.setTimeout(5000);
    mqNet.setHandshakeTimeout(5);
    mq.setSocketTimeout(5);
    mq.setServer(MQ_HOST, MQ_PORT);
    mq.setBufferSize(2048);               // state lines are long
    mq.setKeepAlive(30);
    mq.setCallback(onMqtt);
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", WiFi.softAPIP());

    web.on("/", sendApp);
    web.on("/index.html", sendApp);
    web.on("/poll", handlePoll);
    web.on("/cmd", handleCmd);
    web.on("/flyupdate", HTTP_GET, handleFlyPage);
    web.on("/flyupdate", HTTP_POST, handleFlyDone, handleFlyUpload);
    web.on("/wifi", HTTP_GET, handleWifiPage);
    web.on("/wifi", HTTP_POST, handleWifiSave);
    updater.setup(&web, "/update", OTA_USER, OTA_PASS);
    web.onNotFound(captive);
    web.begin();

    bleInit();
    Serial.println("Lueftungssystem bridge ready: WiFi 'Lueftungssystem', http://192.168.4.1, Bluetooth 'Lueftungssystem'");
    toFly("get");
}

void loop()
{
    dns.processNextRequest();
    web.handleClient();
    mqLoop();

    while (Serial2.available())
    {
        char c = (char)Serial2.read();
        if (c == '\n' || c == '\r')
        {
            if (flyIn.length()) { fromFly(flyIn); flyIn = ""; }
        }
        else if (flyIn.length() < 2000) flyIn += c;
    }

    if (bleQueue.length())
    {
        portENTER_CRITICAL(&bleMux);
        String q = bleQueue;
        bleQueue = "";
        portEXIT_CRITICAL(&bleMux);
        Serial2.print(q);
    }
    delay(1);
}
