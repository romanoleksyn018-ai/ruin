/*
 * RomaOS v1.0 — CLI OS for ESP32 + ILI9488 (3.5" RPi Display)
 * Hardware: ESP32 DevKit v1, ILI9488 480x320, BLE Keyboard, SD card
 *
 * Libraries required (install via Arduino Library Manager):
 *   - TFT_eSPI          by Bodmer
 *   - TJpg_Decoder      by Bodmer
 *   - NimBLE-Arduino    by h2zero  (BLE HID host)
 *   - SD                (built-in ESP32)
 *   - WiFi              (built-in ESP32)
 *
 * User_Setup.h for TFT_eSPI — see User_Setup.h file in this project.
 *
 * Pinout:
 *   MOSI  → GPIO 23
 *   SCK   → GPIO 18
 *   CS    → GPIO 15
 *   DC    → GPIO 2
 *   RST   → GPIO 4
 *   LED   → 3.3V (always on)
 *   SD_CS → GPIO 5  (SD card on display board)
 */

// ─── Core ────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>

// ─── Display ─────────────────────────────────────────────────────────────────
#include <TFT_eSPI.h>
TFT_eSPI tft = TFT_eSPI();

// ─── JPEG decoder for TV stream ───────────────────────────────────────────────
#include <TJpg_Decoder.h>

// ─── SD card ─────────────────────────────────────────────────────────────────
#include <SD.h>
#define SD_CS 5

// ─── BLE HID Host (keyboard + touchpad) ──────────────────────────────────────
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEClient.h>

// ═════════════════════════════════════════════════════════════════════════════
//  DISPLAY / TERMINAL SETTINGS
// ═════════════════════════════════════════════════════════════════════════════
#define SCREEN_W    480
#define SCREEN_H    320
#define FG_COLOR    TFT_GREEN
#define BG_COLOR    TFT_BLACK
#define PROMPT      "RomaOS> "
#define FONT_W      6    // pixel width  per char (Font 1)
#define FONT_H      8    // pixel height per char (Font 1)
#define COLS        (SCREEN_W / FONT_W)   // ~80
#define ROWS        (SCREEN_H / FONT_H)   // ~40

// ═════════════════════════════════════════════════════════════════════════════
//  TERMINAL STATE
// ═════════════════════════════════════════════════════════════════════════════
static int  curRow   = 0;
static int  curCol   = 0;
String      cmdLine  = "";
bool        tvMode   = false;
String      tvURL    = "";

// ═════════════════════════════════════════════════════════════════════════════
//  BLE KEYBOARD HID
// ═════════════════════════════════════════════════════════════════════════════
static NimBLEClient* bleClient = nullptr;
static NimBLERemoteCharacteristic* bleHIDChar = nullptr;
static bool bleConnected = false;
static QueueHandle_t keyQueue;

// HID usage → ASCII (simplified, US layout)
char hidToAscii(uint8_t keycode, bool shift) {
    if (keycode >= 4 && keycode <= 29) {   // a-z
        char c = 'a' + (keycode - 4);
        return shift ? (c - 32) : c;
    }
    if (keycode >= 30 && keycode <= 38) {  // 1-9
        const char nums[]  = "1234567890";
        const char shfNums[]= "!@#$%^&*(";
        return shift ? shfNums[keycode-30] : nums[keycode-30];
    }
    if (keycode == 39) return shift ? ')' : '0';
    if (keycode == 40) return '\n';  // ENTER
    if (keycode == 42) return '\b';  // BACKSPACE
    if (keycode == 44) return ' ';   // SPACE
    if (keycode == 54) return shift ? '<' : ',';
    if (keycode == 55) return shift ? '>' : '.';
    if (keycode == 56) return shift ? '?' : '/';
    if (keycode == 51) return shift ? ':' : ';';
    if (keycode == 52) return shift ? '"' : '\'';
    if (keycode == 45) return shift ? '_' : '-';
    if (keycode == 46) return shift ? '+' : '=';
    return 0;
}

// BLE HID report callback
void hidNotifyCallback(NimBLERemoteCharacteristic* pChar,
                        uint8_t* pData, size_t length, bool isNotify) {
    if (length < 3) return;
    uint8_t modifier = pData[0];
    bool    shift    = (modifier & 0x02) || (modifier & 0x20);
    uint8_t keycode  = pData[2];
    if (keycode == 0) return;
    char c = hidToAscii(keycode, shift);
    if (c != 0) xQueueSend(keyQueue, &c, 0);
}

// ─── BLE scan + connect ───────────────────────────────────────────────────────
class BLEScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
    NimBLEAdvertisedDevice* found = nullptr;
    void onResult(NimBLEAdvertisedDevice* dev) override {
        // Accept any BLE HID device (keyboard appearance = 0x03C1)
        if (dev->haveAppearance() && dev->getAppearance() == 0x03C1) {
            found = dev;
            NimBLEDevice::getScan()->stop();
        }
        // Also match by name containing "keyboard"
        if (dev->haveName()) {
            String n = dev->getName().c_str();
            n.toLowerCase();
            if (n.indexOf("keyboard") >= 0 || n.indexOf("kbd") >= 0) {
                found = dev;
                NimBLEDevice::getScan()->stop();
            }
        }
    }
};
static BLEScanCallbacks* scanCB = nullptr;

void bleScanAndConnect() {
    scanCB = new BLEScanCallbacks();
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(scanCB);
    scan->setActiveScan(true);
    scan->start(10, false);   // 10 seconds

    if (!scanCB->found) return;

    bleClient = NimBLEDevice::createClient();
    if (!bleClient->connect(scanCB->found)) return;

    // Find HID service 0x1812
    NimBLERemoteService* hidSvc = bleClient->getService("1812");
    if (!hidSvc) { bleClient->disconnect(); return; }

    // Find Report characteristic 0x2A4D
    bleHIDChar = hidSvc->getCharacteristic("2A4D");
    if (!bleHIDChar) { bleClient->disconnect(); return; }

    if (bleHIDChar->canNotify())
        bleHIDChar->subscribe(true, hidNotifyCallback);

    bleConnected = true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  TERMINAL PRIMITIVES
// ═════════════════════════════════════════════════════════════════════════════
void termClear() {
    tft.fillScreen(BG_COLOR);
    curRow = 0; curCol = 0;
}

void termScroll() {
    // Scroll up by one text row (8 pixels)
    tft.scroll(FONT_H);
    // Clear bottom row
    tft.fillRect(0, (ROWS - 1) * FONT_H, SCREEN_W, FONT_H, BG_COLOR);
    curRow = ROWS - 1;
}

void termPutChar(char c) {
    if (c == '\n' || curCol >= COLS) {
        curCol = 0;
        curRow++;
        if (curRow >= ROWS) termScroll();
        if (c == '\n') return;
    }
    tft.drawChar(curCol * FONT_W, curRow * FONT_H, c, FG_COLOR, BG_COLOR, 1);
    curCol++;
}

void termPrint(const String& s) {
    for (char c : s) termPutChar(c);
}

void termPrintln(const String& s) {
    termPrint(s);
    termPutChar('\n');
}

void termPrompt() {
    if (curCol != 0) termPutChar('\n');
    termPrint(PROMPT);
}

// ─── Blinking cursor ──────────────────────────────────────────────────────────
static bool   cursorVisible = false;
static uint32_t lastCursorMs = 0;

void drawCursor(bool show) {
    int x = curCol * FONT_W;
    int y = curRow * FONT_H;
    uint16_t col = show ? FG_COLOR : BG_COLOR;
    tft.fillRect(x, y + FONT_H - 2, FONT_W, 2, col);
}

void tickCursor() {
    if (millis() - lastCursorMs > 500) {
        lastCursorMs = millis();
        cursorVisible = !cursorVisible;
        drawCursor(cursorVisible);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  COMMAND: scan — Wi-Fi network scan
// ═════════════════════════════════════════════════════════════════════════════
void cmdScan() {
    termPrintln("Scanning Wi-Fi...");
    int n = WiFi.scanNetworks();
    if (n == 0) {
        termPrintln("No networks found.");
        return;
    }
    for (int i = 0; i < n; i++) {
        String line = String(i + 1) + ". ";
        line += WiFi.SSID(i);
        line += "  [" + String(WiFi.RSSI(i)) + " dBm]";
        line += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " OPEN" : " ENC";
        termPrintln(line);
    }
    WiFi.scanDelete();
}

// ═════════════════════════════════════════════════════════════════════════════
//  COMMAND: connect <ssid> <pass>
// ═════════════════════════════════════════════════════════════════════════════
void cmdConnect(const String& args) {
    int sp = args.indexOf(' ');
    if (sp < 0) { termPrintln("Usage: connect <ssid> <pass>"); return; }
    String ssid = args.substring(0, sp);
    String pass = args.substring(sp + 1);
    termPrint("Connecting to "); termPrintln(ssid);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
        termPrint(".");
        delay(500);
    }
    if (WiFi.status() == WL_CONNECTED) {
        termPrint("\nConnected! IP: ");
        termPrintln(WiFi.localIP().toString());
    } else {
        termPrintln("\nFailed.");
        WiFi.disconnect();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  COMMAND: tv <url> — MJPEG stream viewer
// ═════════════════════════════════════════════════════════════════════════════
WiFiClient tvClient;
static uint8_t  mjpegBuf[1024 * 50];   // 50 KB frame buffer
static uint32_t mjpegLen = 0;

// TJpg callback — renders one 16x16 MCU block to display
bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (y >= SCREEN_H) return 0;
    tft.pushImage(x, y, w, h, bitmap);
    return 1;
}

// Read until boundary marker
bool mjpegReadFrame(WiFiClient& client) {
    // Find JPEG SOI 0xFF 0xD8
    int b, prev = 0;
    mjpegLen = 0;
    bool inJpeg = false;
    while (client.connected()) {
        if (!client.available()) { delay(1); continue; }
        b = client.read();
        if (!inJpeg) {
            if (prev == 0xFF && b == 0xD8) {
                mjpegBuf[0] = 0xFF; mjpegBuf[1] = 0xD8;
                mjpegLen = 2;
                inJpeg = true;
            }
            prev = b;
        } else {
            if (mjpegLen < sizeof(mjpegBuf))
                mjpegBuf[mjpegLen++] = b;
            // Detect EOI 0xFF 0xD9
            if (prev == 0xFF && b == 0xD9) return true;
            prev = b;
        }
    }
    return false;
}

void cmdTV(const String& url) {
    if (WiFi.status() != WL_CONNECTED) {
        termPrintln("Not connected to Wi-Fi. Use: connect <ssid> <pass>");
        return;
    }

    // Parse URL: http://host:port/path
    String u = url;
    if (u.startsWith("http://")) u = u.substring(7);
    int portIdx = u.indexOf(':');
    int pathIdx = u.indexOf('/');
    String host, path;
    int port = 80;
    if (portIdx > 0 && portIdx < pathIdx) {
        host = u.substring(0, portIdx);
        port = u.substring(portIdx + 1, pathIdx).toInt();
        path = u.substring(pathIdx);
    } else if (pathIdx > 0) {
        host = u.substring(0, pathIdx);
        path = u.substring(pathIdx);
    } else {
        host = u; path = "/";
    }

    termPrint("Connecting to "); termPrint(host);
    termPrint(":"); termPrintln(String(port));

    if (!tvClient.connect(host.c_str(), port)) {
        termPrintln("Connection failed.");
        return;
    }
    tvClient.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: RomaOS/1.0\r\n\r\n",
                    path.c_str(), host.c_str());

    // Skip HTTP headers
    while (tvClient.connected()) {
        String line = tvClient.readStringUntil('\n');
        if (line == "\r") break;
    }

    termPrintln("Streaming... (press any key to stop)");
    tft.fillScreen(BG_COLOR);

    TJpgDec.setJpgScale(1);
    TJpgDec.setCallback(tftOutput);

    tvMode = true;
    while (tvMode && tvClient.connected()) {
        if (mjpegReadFrame(tvClient)) {
            TJpgDec.drawJpg(0, 0, mjpegBuf, mjpegLen);
        }
        // Check key press to exit
        char c;
        if (xQueueReceive(keyQueue, &c, 0) == pdTRUE) {
            tvMode = false;
        }
    }
    tvClient.stop();
    tft.fillScreen(BG_COLOR);
    curRow = 0; curCol = 0;
    termPrintln("TV stopped.");
}

// ═════════════════════════════════════════════════════════════════════════════
//  COMMAND: ls [path] — list SD files
// ═════════════════════════════════════════════════════════════════════════════
void cmdLS(const String& path) {
    String p = path.isEmpty() ? "/" : path;
    File dir = SD.open(p);
    if (!dir || !dir.isDirectory()) {
        termPrintln("Cannot open: " + p);
        return;
    }
    File f = dir.openNextFile();
    while (f) {
        String line = f.isDirectory() ? "[DIR] " : "      ";
        line += f.name();
        if (!f.isDirectory()) {
            line += "  (" + String(f.size()) + " B)";
        }
        termPrintln(line);
        f = dir.openNextFile();
    }
}

// ─── cat — print file content ─────────────────────────────────────────────────
void cmdCAT(const String& path) {
    File f = SD.open(path);
    if (!f) { termPrintln("Not found: " + path); return; }
    while (f.available()) {
        char c = f.read();
        termPutChar(c);
    }
    f.close();
    termPutChar('\n');
}

// ─── rm — delete file ─────────────────────────────────────────────────────────
void cmdRM(const String& path) {
    if (SD.remove(path)) termPrintln("Deleted: " + path);
    else termPrintln("Failed to delete: " + path);
}

// ─── mkdir ────────────────────────────────────────────────────────────────────
void cmdMKDIR(const String& path) {
    if (SD.mkdir(path)) termPrintln("Created: " + path);
    else termPrintln("Failed: " + path);
}

// ═════════════════════════════════════════════════════════════════════════════
//  COMMAND: help
// ═════════════════════════════════════════════════════════════════════════════
void cmdHelp() {
    termPrintln("=== RomaOS Commands ===");
    termPrintln("scan              - scan Wi-Fi networks");
    termPrintln("connect <s> <p>   - connect to Wi-Fi");
    termPrintln("disconnect        - disconnect Wi-Fi");
    termPrintln("ip                - show IP address");
    termPrintln("tv <url>          - MJPEG stream (IPTV)");
    termPrintln("ls [path]         - list SD files");
    termPrintln("cat <file>        - print file");
    termPrintln("rm <file>         - delete file");
    termPrintln("mkdir <dir>       - create directory");
    termPrintln("ble               - scan & connect BLE keyboard");
    termPrintln("bleinfo           - show BLE status");
    termPrintln("clear             - clear screen");
    termPrintln("reboot            - restart ESP32");
    termPrintln("help              - this help");
}

// ═════════════════════════════════════════════════════════════════════════════
//  COMMAND DISPATCH
// ═════════════════════════════════════════════════════════════════════════════
void processCommand(const String& raw) {
    String line = raw;
    line.trim();
    if (line.isEmpty()) return;

    String cmd, args;
    int sp = line.indexOf(' ');
    if (sp > 0) {
        cmd  = line.substring(0, sp);
        args = line.substring(sp + 1);
        args.trim();
    } else {
        cmd = line;
    }
    cmd.toLowerCase();

    if      (cmd == "help")       cmdHelp();
    else if (cmd == "clear")      termClear();
    else if (cmd == "scan")       cmdScan();
    else if (cmd == "connect")    cmdConnect(args);
    else if (cmd == "disconnect") { WiFi.disconnect(); termPrintln("Disconnected."); }
    else if (cmd == "ip")         termPrintln(WiFi.localIP().toString());
    else if (cmd == "tv")         cmdTV(args);
    else if (cmd == "ls")         cmdLS(args);
    else if (cmd == "cat")        cmdCAT(args);
    else if (cmd == "rm")         cmdRM(args);
    else if (cmd == "mkdir")      cmdMKDIR(args);
    else if (cmd == "reboot")     ESP.restart();
    else if (cmd == "ble") {
        termPrintln("Scanning BLE...");
        bleScanAndConnect();
        if (bleConnected) termPrintln("Keyboard connected!");
        else              termPrintln("No keyboard found.");
    }
    else if (cmd == "bleinfo") {
        termPrintln(bleConnected ? "BLE: Connected" : "BLE: Not connected");
    }
    else {
        termPrint("Unknown command: "); termPrintln(cmd);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);

    // ── Display ──────────────────────────────────────────────────────────────
    tft.init();
    tft.setRotation(1);          // Landscape
    tft.fillScreen(BG_COLOR);
    tft.setTextColor(FG_COLOR, BG_COLOR);
    tft.setTextSize(1);

    // TJpg decoder
    TJpgDec.setJpgScale(1);
    TJpgDec.setCallback(tftOutput);

    // ── SD ───────────────────────────────────────────────────────────────────
    bool sdOK = SD.begin(SD_CS);

    // ── BLE ──────────────────────────────────────────────────────────────────
    keyQueue = xQueueCreate(64, sizeof(char));
    NimBLEDevice::init("RomaOS");

    // ── Boot banner ──────────────────────────────────────────────────────────
    termPrintln("  ____                        ___  ____  ");
    termPrintln(" |  _ \\ ___  _ __ ___   __ _ / _ \\/ ___| ");
    termPrintln(" | |_) / _ \\| '_ ` _ \\ / _` | | | \\___ \\ ");
    termPrintln(" |  _ < (_) | | | | | | (_| | |_| |___) |");
    termPrintln(" |_| \\_\\___/|_| |_| |_|\\__,_|\\___/|____/ ");
    termPrintln(" v1.0  by Roman                           ");
    termPrintln("------------------------------------------");
    termPrint  ("SD Card: ");
    termPrintln(sdOK ? "OK" : "Not found");
    termPrintln("Type 'help' for commands.");
    termPrintln("Type 'ble' to pair keyboard.");
    termPrintln("------------------------------------------");
    termPrompt();
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════════════════════
void loop() {
    tickCursor();

    // ── Get character (BLE queue or Serial fallback for debug) ────────────────
    char c = 0;
    if (xQueueReceive(keyQueue, &c, 0) != pdTRUE) {
        // Serial fallback so you can test without BLE keyboard
        if (Serial.available()) c = Serial.read();
    }

    if (c == 0) return;

    // Hide cursor while processing
    drawCursor(false);
    cursorVisible = false;

    if (c == '\n' || c == '\r') {
        termPutChar('\n');
        processCommand(cmdLine);
        cmdLine = "";
        termPrompt();
    }
    else if (c == '\b' || c == 127) {
        // Backspace
        if (cmdLine.length() > 0) {
            cmdLine.remove(cmdLine.length() - 1);
            // Erase last char on screen
            if (curCol > 0) curCol--;
            tft.fillRect(curCol * FONT_W, curRow * FONT_H,
                         FONT_W, FONT_H, BG_COLOR);
        }
    }
    else if (c >= 32 && c < 127) {
        cmdLine += c;
        termPutChar(c);
    }
}
