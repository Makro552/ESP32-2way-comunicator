/*
 * ESP32 WROOM + nRF24L01+ PA+LNA + ST7735S 128x160 + M5Stack CardKB
 * Two-way non-blocking chat communicator
 * 
 * Hardware:
 *   MCU: ESP32-WROOM-32 (30-pin DevKit)
 *   TFT : 1.8" 128x160 ST7735S (SPI)
 *   RF  : nRF24L01+ PA+LNA (SPI, shared bus)
 *   KB  : M5Stack Unit CardKB (I2C @ 0x5F)
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RF24.h>
#include <TFT_eSPI.h>

// ==================== HARDWARE CONFIGURATION ====================
#define UNIT_NUMBER 2           // <<<< CHANGE TO 2 ON THE SECOND UNIT BEFORE UPLOADING

// SPI Bus (VSPI) — Shared between TFT and nRF24
#define PIN_SPI_SCK     18
#define PIN_SPI_MISO    19
#define PIN_SPI_MOSI    23

// TFT Display (ST7735S 128x160)
#define PIN_TFT_CS      5
#define PIN_TFT_DC      16
#define PIN_TFT_RST     17

// nRF24L01+ PA+LNA
#define PIN_NRF_CS      15
#define PIN_NRF_CE      4

// I2C — M5Stack CardKB
#define PIN_I2C_SDA     21
#define PIN_I2C_SCL     22
#define CARDKB_ADDR     0x5F

// ==================== DISPLAY LAYOUT ====================
#define SCREEN_W        128
#define SCREEN_H        160
#define INPUT_H         28                  // ~17.5% of screen
#define CHAT_H          (SCREEN_H - INPUT_H - 1)  // 131 px chat area
#define SEPARATOR_Y     CHAT_H             // Y-coordinate of the line
#define INPUT_Y         (SEPARATOR_Y + 1)  // 132

// ==================== FONT METRICS (GLCD Font 1) ====================
#define LINE_HEIGHT     8                   // Fixed GLCD font height
#define CHAR_WIDTH      6                   // Fixed GLCD font width

// ==================== COLORS ====================
#define C_BG            TFT_BLACK
#define C_SENT          TFT_GREEN
#define C_RECV          TFT_WHITE
#define C_INPUT_BG      TFT_DARKGREY
#define C_SEPARATOR     TFT_LIGHTGREY
#define C_CURSOR        TFT_GREEN

// ==================== CHAT & RADIO LIMITS ====================
#define MAX_MESSAGES    30                  // Circular buffer depth
#define MAX_MSG_LEN     31                  // 31 chars + null = 32 bytes (one nRF24 payload)
#define MAX_VISIBLE_INPUT_CHARS ((SCREEN_W - 8) / CHAR_WIDTH)

// ==================== TIMING ====================
#define KEY_POLL_MS         20
#define CURSOR_BLINK_MS     500
#define KEY_DEBOUNCE_MS     120

// ==================== RADIO PIPES ====================
#define PIPE_ADDR_1         0xE8E8F0F0E1LL
#define PIPE_ADDR_2         0xE8E8F0F0E2LL

// ==================== DATA STRUCTURES ====================
struct ChatMessage {
    char text[MAX_MSG_LEN + 1];
    bool isSent;        // true = Me, false = Remote
};

// ==================== GLOBAL OBJECTS ====================
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite chatSprite = TFT_eSprite(&tft);   // Flicker-free chat buffer
RF24 radio(PIN_NRF_CE, PIN_NRF_CS);

ChatMessage chatBuffer[MAX_MESSAGES];
uint8_t chatHead = 0;       // Next write position
uint8_t chatCount = 0;      // Valid messages in buffer

char inputBuffer[MAX_MSG_LEN + 1];
uint8_t inputLen = 0;

volatile bool chatDirty = true;
volatile bool inputDirty = true;

unsigned long lastKeyPoll = 0;
unsigned long lastCursorBlink = 0;
bool cursorVisible = true;

char lastKeyPressed = 0;
unsigned long lastKeyTime = 0;

// ==================== FUNCTION PROTOTYPES ====================
void addMessage(const char* text, bool isSent);
void redrawChat();
void redrawInput(bool showCursor);
void sendMessage(const char* text);
void checkRadio();
void checkKeyboard();
void processKey(char c);

// ==================== SETUP ====================
void setup() {
    Serial.begin(115200);
    delay(100);

    // Start I2C for CardKB (100 kHz per M5Stack spec)
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);

    // Start shared SPI bus (VSPI default pins: SCK=18, MISO=19, MOSI=23)
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_NRF_CS);

    // Initialize TFT
    tft.init();
    tft.setRotation(0);                 // Portrait. Use 2 to flip 180° if needed
    tft.fillScreen(C_BG);
    tft.setTextFont(1);                 // GLCD 6x8 font

    // Allocate chat sprite (128 x 131 @ 16-bit = ~34 KB of RAM)
    chatSprite.createSprite(SCREEN_W, CHAT_H);
    chatSprite.setTextFont(1);
    chatSprite.setTextWrap(false);      // We handle wrapping manually for alignment

    // Initialize nRF24
    if (!radio.begin()) {
        tft.setTextColor(TFT_RED, C_BG);
        tft.drawString("RADIO INIT FAIL!", 4, 70);
        Serial.println("ERROR: nRF24 failed to initialize.");
        while (1) { delay(100); }
    }

    // NOTE: Use RF24_PA_MAX for maximum range ONLY if you have a 10uF cap
    // directly across the nRF24 module's 3.3V/GND pins. Without that cap,
    // the ESP32 may brownout and reboot during TX. Use RF24_PA_LOW for safe
    // bench testing without the external capacitor.
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_250KBPS);        // Slower = longer range + reliability
    radio.setChannel(108);                  // 2.508 GHz (away from WiFi ch 1-6)
    radio.setRetries(5, 15);                // 5 retries, 15*250 µs delay
    radio.setPayloadSize(32);               // Fixed 32-byte payloads
    radio.setAutoAck(true);

#if UNIT_NUMBER == 1
    radio.openWritingPipe(PIPE_ADDR_2);     // Unit 1 sends to Unit 2's address
    radio.openReadingPipe(1, PIPE_ADDR_1);  // Unit 1 listens on its own address
#else
    radio.openWritingPipe(PIPE_ADDR_1);     // Unit 2 sends to Unit 1's address
    radio.openReadingPipe(1, PIPE_ADDR_2);  // Unit 2 listens on its own address
#endif

    radio.startListening();

    // Splash screen
    tft.setTextColor(TFT_WHITE, C_BG);
    tft.drawString("ESP32 Chat", 34, 45);
    tft.drawString(UNIT_NUMBER == 1 ? "UNIT 1" : "UNIT 2", 46, 65);
    tft.drawString("Ready...", 40, 85);
    delay(1200);                            // Only blocking delay in entire program

    // Draw initial UI
    tft.fillScreen(C_BG);
    redrawChat();
    redrawInput(true);
}

// ==================== MAIN LOOP (NON-BLOCKING) ====================
void loop() {
    unsigned long now = millis();

    // 1. Poll CardKB at fixed interval
    if (now - lastKeyPoll >= KEY_POLL_MS) {
        lastKeyPoll = now;
        checkKeyboard();
    }

    // 2. Poll nRF24 for incoming messages
    checkRadio();

    // 3. Cursor blink timing
    if (now - lastCursorBlink >= CURSOR_BLINK_MS) {
        lastCursorBlink = now;
        cursorVisible = !cursorVisible;
        inputDirty = true;
    }

    // 4. Render only what changed
    if (chatDirty) {
        chatDirty = false;
        redrawChat();
    }
    if (inputDirty) {
        inputDirty = false;
        redrawInput(cursorVisible);
    }
}

// ==================== CHAT HISTORY RENDERING ====================
void addMessage(const char* text, bool isSent) {
    if (!text || text[0] == '\0') return;

    strncpy(chatBuffer[chatHead].text, text, MAX_MSG_LEN);
    chatBuffer[chatHead].text[MAX_MSG_LEN] = '\0';
    chatBuffer[chatHead].isSent = isSent;

    chatHead = (chatHead + 1) % MAX_MESSAGES;
    if (chatCount < MAX_MESSAGES) chatCount++;

    chatDirty = true;
}

void redrawChat() {
    chatSprite.fillSprite(C_BG);

    const int16_t maxCharsPerLine = (SCREEN_W - 4) / CHAR_WIDTH; // 2 px side padding
    const int16_t msgSpacing = 2;                       // Gap between messages

    int16_t y = CHAT_H;                                 // Start at bottom

    // Iterate newest -> oldest so newest messages anchor at the bottom
    for (uint8_t i = 0; i < chatCount; i++) {
        uint8_t idx = (chatHead - 1 - i + MAX_MESSAGES) % MAX_MESSAGES;
        ChatMessage& msg = chatBuffer[idx];

        int16_t msgLen = strlen(msg.text);
        if (msgLen == 0) continue;

        int16_t lines = (msgLen + maxCharsPerLine - 1) / maxCharsPerLine;
        if (lines < 1) lines = 1;
        int16_t msgHeight = lines * LINE_HEIGHT + msgSpacing;

        y -= msgHeight;
        if (y < 0) break;                               // Oldest messages scroll off

        uint16_t color = msg.isSent ? C_SENT : C_RECV;
        int16_t lineY = y;
        int16_t charPos = 0;

        while (charPos < msgLen && lineY < CHAT_H) {
            int16_t remaining = msgLen - charPos;
            int16_t lineLen = (remaining > maxCharsPerLine) ? maxCharsPerLine : remaining;

            char lineBuf[MAX_MSG_LEN + 1];
            strncpy(lineBuf, msg.text + charPos, lineLen);
            lineBuf[lineLen] = '\0';

            chatSprite.setTextColor(color, C_BG);

            if (msg.isSent) {
                // Right-align within chat window
                int16_t textW = chatSprite.textWidth(lineBuf);
                chatSprite.drawString(lineBuf, SCREEN_W - 2 - textW, lineY);
            } else {
                // Left-align with 2 px padding
                chatSprite.drawString(lineBuf, 2, lineY);
            }

            lineY += LINE_HEIGHT;
            charPos += lineLen;
        }
    }

    // Push the fully rendered chat sprite to the screen in one operation
    chatSprite.pushSprite(0, 0);
}

// ==================== INPUT BOX RENDERING ====================
void redrawInput(bool showCursor) {
    // Clear only the input rectangle
    tft.fillRect(0, INPUT_Y, SCREEN_W, INPUT_H, C_INPUT_BG);

    // Draw separator line between chat and input
    tft.drawFastHLine(0, SEPARATOR_Y, SCREEN_W, C_SEPARATOR);

    // Scroll input text if it exceeds visible width
    int16_t startChar = 0;
    if (inputLen > MAX_VISIBLE_INPUT_CHARS) {
        startChar = inputLen - MAX_VISIBLE_INPUT_CHARS;
    }

    int16_t visibleLen = inputLen - startChar;
    if (visibleLen > MAX_VISIBLE_INPUT_CHARS) visibleLen = MAX_VISIBLE_INPUT_CHARS;
    if (visibleLen < 0) visibleLen = 0;

    char visibleBuf[MAX_MSG_LEN + 1];
    strncpy(visibleBuf, inputBuffer + startChar, visibleLen);
    visibleBuf[visibleLen] = '\0';

    // Vertically center text in input box
    int16_t textY = INPUT_Y + (INPUT_H - LINE_HEIGHT) / 2;
    tft.setTextColor(C_SENT, C_INPUT_BG);
    tft.drawString(visibleBuf, 4, textY);

    // Draw cursor line
    if (showCursor) {
        int16_t cursorX = 4 + tft.textWidth(visibleBuf);
        tft.drawFastVLine(cursorX, textY, LINE_HEIGHT, C_CURSOR);
    }
}

// ==================== RADIO LOGIC ====================
void sendMessage(const char* text) {
    if (!text || text[0] == '\0') return;

    // Zero-pad to exactly 32 bytes so receiver never sees trailing garbage
    char sendBuf[32] = {0};
    strncpy(sendBuf, text, MAX_MSG_LEN);

    radio.stopListening();
    bool ok = radio.write(sendBuf, sizeof(sendBuf));
    radio.startListening();

    if (ok) {
        addMessage(text, true);
        Serial.print("Sent: ");
        Serial.println(text);
    } else {
        addMessage("!Send Fail", true);
        Serial.println("Send failed (no ACK)");
    }
}

void checkRadio() {
    if (!radio.available()) return;

    char buf[32] = {0};
    radio.read(buf, sizeof(buf));
    buf[31] = '\0';                     // Safety terminator

    if (strlen(buf) > 0) {
        addMessage(buf, false);
        Serial.print("Received: ");
        Serial.println(buf);
    }
}

// ==================== KEYBOARD LOGIC ====================
void checkKeyboard() {
    Wire.requestFrom(CARDKB_ADDR, (uint8_t)1);
    if (!Wire.available()) return;

    char c = Wire.read();
    if (c == 0x00) return;              // No key pressed

    // Debounce: ignore identical key if it arrives too quickly
    if (c == lastKeyPressed && (millis() - lastKeyTime) < KEY_DEBOUNCE_MS) {
        return;
    }
    lastKeyPressed = c;
    lastKeyTime = millis();

    processKey(c);
}

void processKey(char c) {
    // Backspace / Delete
    if (c == 0x08 || c == 0x7F) {
        if (inputLen > 0) {
            inputLen--;
            inputBuffer[inputLen] = '\0';
            cursorVisible = true;
            lastCursorBlink = millis();
            inputDirty = true;
        }
        return;
    }

    // Enter (CR or LF)
    if (c == 0x0D || c == 0x0A) {
        if (inputLen > 0) {
            inputBuffer[inputLen] = '\0';
            sendMessage(inputBuffer);
            inputLen = 0;
            inputBuffer[0] = '\0';
            cursorVisible = true;
            lastCursorBlink = millis();
            inputDirty = true;
        }
        return;
    }

    // Printable ASCII (space through ~)
    if (c >= 32 && c <= 126) {
        if (inputLen < MAX_MSG_LEN) {
            inputBuffer[inputLen++] = c;
            inputBuffer[inputLen] = '\0';
            cursorVisible = true;
            lastCursorBlink = millis();
            inputDirty = true;
        }
        // At max length: silently ignore further characters (backspace/enter still work)
    }
    // All other keys (arrows, Esc, Tab, Fn combos) are ignored
}