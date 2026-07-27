#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <time.h>

// Libreria Telegram Bot
#include <UniversalTelegramBot.h>

// Librerie IR (Solo Trasmissione)
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>

#define SSD1306_WHITE SH110X_WHITE
#define SSD1306_BLACK SH110X_BLACK

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C

#define SDA_PIN 21
#define SCL_PIN 47

#define PIN_CLK 7
#define PIN_DT  6
#define PIN_SW  5

// --- PIN TRASMETTITORE IR ---
#define PIN_IR_TX  4

#define TOTAL_PAGES 6 // 0: Occhi, 1: Meteo, 2: Orologio, 3: Cronometro, 4: IR Control, 5: Sistema

const char* ntpServer = "pool.ntp.org";
const char* timeZone  = "CET-1CEST,M3.5.0,M10.5.0/3"; 

Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

WiFiClientSecure netClient;
UniversalTelegramBot* bot = NULL;
unsigned long lastTelegramCheck = 0;
const unsigned long telegramInterval = 2000; // Controlla messaggi ogni 2 secondi

// --- GESTIONE MESSAGGI TELEGRAM SU SCHERMO ---
bool hasTelegramMessage = false;
String telegramMessageText = "";

// --- IR TRANSMITTER ---
IRsend irsend(PIN_IR_TX);

struct IRSignal {
  decode_type_t protocol = UNKNOWN;
  uint64_t value = 0;
  uint16_t bits = 0;
};

IRSignal savedSignals[5];

bool irSubmenuActive = false; 
int irSelectedSlot = 0;       
int topItem = 0;              

// Feedback Visivo
String feedbackMsg = "";
unsigned long feedbackTime = 0;

volatile int currentPage = 0;
volatile unsigned long lastPageSwitch = 0;
bool isAPMode = false;

// --- STATO CRONOMETRO ---
bool swRunning = false;
unsigned long swStartTime = 0;
unsigned long swElapsedTime = 0;

// --- DATI METEO ---
struct WeatherData {
  float temp = 0.0;
  int humidity = 0;
  char description[32] = "In attesa...";
  bool updated = false;
  bool isRainy = false;
} weather;

SemaphoreHandle_t weatherMutex;

// --- FISICA OCCHI ---
struct Eye {
  float currentX = 0.0, currentY = 0.0;
  float targetX = 0.0, targetY = 0.0;
  float vx = 0.0, vy = 0.0;
  int radiusX = 18, radiusY = 22;
} leftEye, rightEye;

const float k = 0.12;
const float d = 0.60;
unsigned long lastSaccadeTime = 0;
unsigned long nextSaccadeInterval = 3000;
bool isBlinking = false;
unsigned long blinkStartTime = 0;

// PROTOTIPI FUNZIONI
void IRAM_ATTR handleEncoderRotation();
void handleButtonPress();
void setupCaptivePortal();
void weatherTask(void * parameter);
void updatePhysics();
void drawEyesPage();
void drawWeatherPage();
void drawClockPage();
void drawStopwatchPage();
void drawIRPage();
void drawSystemPage();
void drawTelegramMessagePage();
void loadIRSignals();
void saveIRSignals();
void showFeedback(String msg);
void handleTelegram();

void showFeedback(String msg) {
  feedbackMsg = msg;
  feedbackTime = millis();
}

void IRAM_ATTR handleEncoderRotation() {
  if (hasTelegramMessage) return; // Disattiva lo scorrimento se c'e' un messaggio a schermo

  unsigned long now = millis();
  if (now - lastPageSwitch > 150) {
    bool isCW = (digitalRead(PIN_DT) == HIGH);

    if (currentPage == 4 && irSubmenuActive) {
      if (isCW) {
        irSelectedSlot = (irSelectedSlot + 1) % 7;
      } else {
        irSelectedSlot = (irSelectedSlot - 1 + 7) % 7;
      }
    } else {
      irSubmenuActive = false;
      if (isCW) {
        currentPage = (currentPage + 1) % TOTAL_PAGES;
      } else {
        currentPage = (currentPage - 1 + TOTAL_PAGES) % TOTAL_PAGES;
      }
    }
    lastPageSwitch = now;
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_CLK, INPUT_PULLUP);
  pinMode(PIN_DT, INPUT_PULLUP);
  pinMode(PIN_SW, INPUT_PULLUP);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);

  if (!display.begin(SCREEN_ADDRESS, true)) {
    for (;;);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // Reset Wi-Fi se il pulsante dell'encoder e' tenuto premuto all'avvio
  if (digitalRead(PIN_SW) == LOW) {
    display.setCursor(10, 25);
    display.println(F("Reset Wi-Fi..."));
    display.display();
    preferences.begin("deskbuddy", false);
    preferences.clear();
    preferences.end();
    delay(2000);
    ESP.restart();
  }

  attachInterrupt(digitalPinToInterrupt(PIN_CLK), handleEncoderRotation, FALLING);

  weatherMutex = xSemaphoreCreateMutex();

  preferences.begin("deskbuddy", false);
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("pass", "");
  String botToken = preferences.getString("bottoken", "");

  loadIRSignals();
  irsend.begin();

  if (ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 15) {
      delay(500);
      attempts++;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTzTime(timeZone, ntpServer);
    xTaskCreatePinnedToCore(weatherTask, "WeatherTask", 8192, NULL, 1, NULL, 0);

    if (botToken.length() > 0) {
      netClient.setInsecure(); // Salva RAM saltando la verifica del certificato SSL
      bot = new UniversalTelegramBot(botToken, netClient);
    }
  } else {
    setupCaptivePortal();
    isAPMode = true;
  }
}

void loop() {
  if (isAPMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F("--- CONFIG MODE ---"));
    display.println(F("Connect to AP:"));
    display.println(F("DeskBuddy-Setup"));
    display.println(F("192.168.4.1"));
    display.display();
    return;
  }

  handleButtonPress();

  // Controllo periodico dei messaggi Telegram
  if (bot != NULL && millis() - lastTelegramCheck > telegramInterval) {
    handleTelegram();
    lastTelegramCheck = millis();
  }

  updatePhysics();

  display.clearDisplay();

  if (hasTelegramMessage) {
    drawTelegramMessagePage();
  } else {
    switch (currentPage) {
      case 0: drawEyesPage(); break;
      case 1: drawWeatherPage(); break;
      case 2: drawClockPage(); break;
      case 3: drawStopwatchPage(); break;
      case 4: drawIRPage(); break;
      case 5: drawSystemPage(); break;
    }
  }

  display.display();
}

void handleTelegram() {
  // Usa last_message_received anziche last_message_id
  int numNewMessages = bot->getUpdates(bot->last_message_received + 1);

  while (numNewMessages) {
    for (int i = 0; i < numNewMessages; i++) {
      String chat_id = String(bot->messages[i].chat_id);
      String text = bot->messages[i].text;

      if (text.startsWith("/msg ")) {
        telegramMessageText = text.substring(5);
        hasTelegramMessage = true;
        bot->sendMessage(chat_id, "📩 Messaggio inviato allo schermo OLED!", "");
      } 
      else if (text.startsWith("/save ")) {
        // Sintassi: /save <slot 1-5> <protocollo> <hex> <bits>
        int slotNum = 0;
        char protoStr[20];
        char hexStr[30];
        int bits = 32;

        int parsed = sscanf(text.c_str(), "/save %d %s %s %d", &slotNum, protoStr, hexStr, &bits);
        if (parsed >= 3) {
          if (slotNum >= 1 && slotNum <= 5) {
            int slotIdx = slotNum - 1;
            decode_type_t proto = strToDecodeType(protoStr);
            uint64_t val = strtoull(hexStr, NULL, 0);

            savedSignals[slotIdx].protocol = proto;
            savedSignals[slotIdx].value = val;
            savedSignals[slotIdx].bits = bits;
            saveIRSignals();

            String resp = "✅ Codice IR salvato in Slot " + String(slotNum) + "!\n";
            resp += "Protocollo: " + String(protoStr) + "\n";
            resp += "Valore: " + String(hexStr) + "\n";
            resp += "Bit: " + String(bits);
            bot->sendMessage(chat_id, resp, "");
          } else {
            bot->sendMessage(chat_id, "❌ Slot non valido (usa da 1 a 5).", "");
          }
        } else {
          bot->sendMessage(chat_id, "❌ Sintassi errata!\nUsa: `/save <1-5> <PROTOCOLLO> <HEX> <BIT>`\nEsempio: `/save 1 SAMSUNG 0xE0E040BF 32`", "Markdown");
        }
      }
      else if (text.startsWith("/send ")) {
        int slotNum = text.substring(6).toInt();
        if (slotNum >= 1 && slotNum <= 5) {
          int slotIdx = slotNum - 1;
          if (savedSignals[slotIdx].protocol != UNKNOWN) {
            irsend.send(savedSignals[slotIdx].protocol, savedSignals[slotIdx].value, savedSignals[slotIdx].bits);
            bot->sendMessage(chat_id, "📡 Segnale IR trasmesso!", "");
            showFeedback("TX da Telegram!");
          } else {
            bot->sendMessage(chat_id, "⚠️ Lo slot selezionato e' vuoto.", "");
          }
        } else {
          bot->sendMessage(chat_id, "❌ Numero slot non valido (1-5).", "");
        }
      }
      else if (text == "/list") {
        String listMsg = "📋 *Slot IR Memorizzati:*\n\n";
        for (int k = 0; k < 5; k++) {
          listMsg += "Slot " + String(k + 1) + ": ";
          if (savedSignals[k].protocol != UNKNOWN) {
            listMsg += typeToString(savedSignals[k].protocol) + " - 0x" + String((uint32_t)savedSignals[k].value, HEX) + "\n";
          } else {
            listMsg += "[Vuoto]\n";
          }
        }
        bot->sendMessage(chat_id, listMsg, "Markdown");
      }
      else {
        String welcome = "🤖 *DeskBuddy Bot*\n\n";
        welcome += "Comandi disponibili:\n";
        welcome += "📩 `/msg <testo>` - Mostra messaggio a schermo\n";
        welcome += "💾 `/save <1-5> <PROTO> <HEX> <BIT>` - Salva codice IR\n";
        welcome += "📡 `/send <1-5>` - Invia codice IR dallo slot\n";
        welcome += "📋 `/list` - Elenca codici salvati";
        bot->sendMessage(chat_id, welcome, "Markdown");
      }
    }
    numNewMessages = bot->getUpdates(bot->last_message_received + 1);
  }
}

void loadIRSignals() {
  preferences.getBytes("ir_data", savedSignals, sizeof(savedSignals));
}

void saveIRSignals() {
  preferences.putBytes("ir_data", savedSignals, sizeof(savedSignals));
}

void handleButtonPress() {
  static bool lastBtnState = HIGH;
  static unsigned long pressStartTime = 0;
  bool currentBtnState = digitalRead(PIN_SW);

  if (lastBtnState == HIGH && currentBtnState == LOW) {
    pressStartTime = millis();
  } else if (lastBtnState == LOW && currentBtnState == HIGH) {
    unsigned long duration = millis() - pressStartTime;

    if (duration > 50 && duration < 800) { // CLICK BREVE
      // Chiude il messaggio Telegram a schermo
      if (hasTelegramMessage) {
        hasTelegramMessage = false;
        return;
      }

      if (currentPage == 3) {
        if (swRunning) {
          swRunning = false;
          swElapsedTime += millis() - swStartTime;
        } else {
          swRunning = true;
          swStartTime = millis();
        }
      } else if (currentPage == 4) { // Pagina IR
        if (!irSubmenuActive) {
          irSubmenuActive = true;
          irSelectedSlot = 0;
        } else {
          if (irSelectedSlot == 0) {
            irSubmenuActive = false; // Esci dal menu
          } else if (irSelectedSlot == 1) {
            irsend.send(SAMSUNG, 0xE0E040BF, 32);
            showFeedback("TX TV Samsung!");
          } else if (irSelectedSlot >= 2 && irSelectedSlot <= 6) {
            int slotIdx = irSelectedSlot - 2;
            if (savedSignals[slotIdx].protocol != UNKNOWN) {
              irsend.send(savedSignals[slotIdx].protocol, savedSignals[slotIdx].value, savedSignals[slotIdx].bits);
              showFeedback("Segnale Inviato!");
            } else {
              showFeedback("Slot Vuoto!");
            }
          }
        }
      }
    } else if (duration >= 800) { // CLICK LUNGO
      if (currentPage == 3) {
        swRunning = false;
        swElapsedTime = 0;
      } else if (currentPage == 4 && irSubmenuActive) {
        irSubmenuActive = false;
      }
    }
  }
  lastBtnState = currentBtnState;
}

void drawTelegramMessagePage() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("--- TELEGRAM MSG ---"));
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  display.setCursor(0, 16);
  display.println(telegramMessageText);

  // Footer con indicazione di chiusura
  display.fillRect(0, 52, 128, 12, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  display.setCursor(4, 54);
  display.print(F("[Premere per chiudere]"));
}

void updatePhysics() {
  unsigned long now = millis();
  if (now - lastSaccadeTime > nextSaccadeInterval) {
    leftEye.targetX = random(-12, 13);
    leftEye.targetY = random(-8, 9);
    rightEye.targetX = leftEye.targetX;
    rightEye.targetY = leftEye.targetY;
    lastSaccadeTime = now;
    nextSaccadeInterval = random(1500, 5000);
  }

  if (!isBlinking && random(0, 300) == 42) {
    isBlinking = true;
    blinkStartTime = now;
  }
  if (isBlinking && (now - blinkStartTime > 150)) {
    isBlinking = false;
  }

  leftEye.vx += (leftEye.targetX - leftEye.currentX) * k - leftEye.vx * d;
  leftEye.currentX += leftEye.vx;
  leftEye.vy += (leftEye.targetY - leftEye.currentY) * k - leftEye.vy * d;
  leftEye.currentY += leftEye.vy;

  rightEye.vx += (rightEye.targetX - rightEye.currentX) * k - rightEye.vx * d;
  rightEye.currentX += rightEye.vx;
  rightEye.vy += (rightEye.targetY - rightEye.currentY) * k - rightEye.vy * d;
  rightEye.currentY += rightEye.vy;
}

void drawWeatherAnimation(int x, int y) {
  unsigned long frame = millis() / 50;
  if (weather.isRainy) {
    display.fillCircle(x, y, 10, SSD1306_WHITE);
    display.fillCircle(x + 10, y - 4, 12, SSD1306_WHITE);
    display.fillCircle(x + 22, y, 9, SSD1306_WHITE);
    display.fillRect(x - 2, y + 2, 26, 9, SSD1306_WHITE);
    for (int i = 0; i < 3; i++) {
      int dropY = y + 14 + ((frame + i * 8) % 12);
      int dropX = x + 3 + (i * 9);
      display.drawFastVLine(dropX, dropY, 3, SSD1306_WHITE);
    }
  } else {
    int radius = 10;
    display.drawCircle(x + 10, y + 5, radius, SSD1306_WHITE);
    int toggle = (frame / 8) % 2;
    for (int i = 0; i < 8; i++) {
      float angle = i * (3.14159 / 4.0);
      int len = (i % 2 == toggle) ? 6 : 3;
      int x1 = (x + 10) + cos(angle) * (radius + 2);
      int y1 = (y + 5) + sin(angle) * (radius + 2);
      int x2 = (x + 10) + cos(angle) * (radius + 2 + len);
      int y2 = (y + 5) + sin(angle) * (radius + 2 + len);
      display.drawLine(x1, y1, x2, y2, SSD1306_WHITE);
    }
  }
}

void drawEyesPage() {
  int leftCenterX = 36, rightCenterX = 92, centerY = 32;
  if (isBlinking) {
    display.drawFastHLine(leftCenterX - leftEye.radiusX, centerY, leftEye.radiusX * 2, SSD1306_WHITE);
    display.drawFastHLine(rightCenterX - rightEye.radiusX, centerY, rightEye.radiusX * 2, SSD1306_WHITE);
  } else {
    display.fillRoundRect(leftCenterX - leftEye.radiusX, centerY - leftEye.radiusY, leftEye.radiusX * 2, leftEye.radiusY * 2, 8, SSD1306_WHITE);
    display.fillRoundRect(rightCenterX - rightEye.radiusX, centerY - rightEye.radiusY, rightEye.radiusX * 2, rightEye.radiusY * 2, 8, SSD1306_WHITE);
    int pupL_X = leftCenterX + (int)leftEye.currentX - 5;
    int pupL_Y = centerY + (int)leftEye.currentY - 7;
    int pupR_X = rightCenterX + (int)rightEye.currentX - 5;
    int pupR_Y = centerY + (int)rightEye.currentY - 7;
    display.fillRoundRect(pupL_X, pupL_Y, 10, 14, 3, SSD1306_BLACK);
    display.fillRoundRect(pupR_X, pupR_Y, 10, 14, 3, SSD1306_BLACK);
  }
}

void drawWeatherPage() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("--- METEO LOCALE ---"));
  if (xSemaphoreTake(weatherMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (weather.updated) {
      drawWeatherAnimation(90, 25);
      display.setTextSize(2);
      display.setCursor(0, 18);
      display.printf("%.1f C", weather.temp);
      display.setTextSize(1);
      display.setCursor(0, 40);
      display.printf("Umidita: %d%%", weather.humidity);
      display.setCursor(0, 52);
      display.println(weather.description);
    } else {
      display.setCursor(0, 25);
      display.println(F("Caricamento..."));
    }
    xSemaphoreGive(weatherMutex);
  }
}

void drawClockPage() {
  struct tm timeinfo;
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("--- OROLOGIO NTP ---"));
  if (!getLocalTime(&timeinfo)) {
    display.setCursor(0, 25);
    display.println(F("Sincronizzazione..."));
    return;
  }
  display.setTextSize(2);
  display.setCursor(16, 20);
  display.printf("%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  display.setTextSize(1);
  const char* days[] = {"Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab"};
  char dateStr[32];
  snprintf(dateStr, sizeof(dateStr), "%s %02d/%02d/%04d", days[timeinfo.tm_wday], timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
  display.setCursor(10, 48);
  display.println(dateStr);
}

void drawStopwatchPage() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("--- CRONOMETRO ---"));

  unsigned long totalMs = swElapsedTime;
  if (swRunning) {
    totalMs += (millis() - swStartTime);
  }

  unsigned long minutes = (totalMs / 60000) % 60;
  unsigned long seconds = (totalMs / 1000) % 60;
  unsigned long tenths = (totalMs / 100) % 10;

  display.setTextSize(2);
  display.setCursor(16, 22);
  display.printf("%02lu:%02lu.%lu", minutes, seconds, tenths);

  display.setTextSize(1);
  display.setCursor(0, 48);
  if (swRunning) {
    display.println(F("[Click] Pausa"));
  } else {
    display.println(F("[Click] Start  [Hold] Reset"));
  }
}

void drawIRPage() {
  if (!irSubmenuActive) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("--- IR CONTROL ---"));

    int countSaved = 0;
    for (int i = 0; i < 5; i++) {
      if (savedSignals[i].protocol != UNKNOWN) countSaved++;
    }
    display.setCursor(0, 18);
    display.printf("Slot Usati: %d / 5\n", countSaved);
    display.println(F("Gestisci da Telegram!"));

    display.drawRoundRect(0, 42, 128, 22, 4, SSD1306_WHITE);
    display.setCursor(6, 49);
    display.print(F("> Click per Trasmettere"));
    return;
  }

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(F("MENU TRASMETTITORE"));
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  const int maxVisible = 4;
  const int itemHeight = 12;
  const int startY = 14;

  if (irSelectedSlot < topItem) {
    topItem = irSelectedSlot;
  } else if (irSelectedSlot >= topItem + maxVisible) {
    topItem = irSelectedSlot - maxVisible + 1;
  }

  for (int i = 0; i < maxVisible; i++) {
    int idx = topItem + i;
    if (idx >= 7) break;

    int y = startY + (i * itemHeight);
    bool isSelected = (idx == irSelectedSlot);

    if (isSelected) {
      display.fillRect(0, y, 122, itemHeight, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    }

    display.setCursor(3, y + 2);

    switch (idx) {
      case 0:
        display.print(F("< ESCI DAL MENU"));
        break;
      case 1:
        display.print(F("Test: Samsung ON/OFF"));
        break;
      default: {
        int slotNum = idx - 1;
        int slotIdx = idx - 2;
        if (savedSignals[slotIdx].protocol != UNKNOWN) {
          display.printf("%d. Slot %d (Pronto)", slotNum, slotNum);
        } else {
          display.printf("%d. Slot %d [Vuoto]", slotNum, slotNum);
        }
        break;
      }
    }
  }

  int barHeight = (maxVisible * 48) / 7;
  int barY = startY + ((topItem * (48 - barHeight)) / (7 - maxVisible));
  display.drawFastVLine(126, startY, 48, SSD1306_WHITE);
  display.fillRect(125, barY, 3, barHeight, SSD1306_WHITE);

  if (millis() - feedbackTime < 1200 && feedbackMsg.length() > 0) {
    display.fillRect(10, 20, 108, 26, SSD1306_BLACK);
    display.drawRect(10, 20, 108, 26, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    display.setCursor(14, 29);
    display.print(feedbackMsg);
  }
}

void drawSystemPage() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("--- INFO SISTEMA ---"));
  display.setCursor(0, 18);
  display.printf("IP: %s", WiFi.localIP().toString().c_str());
  display.setCursor(0, 32);
  display.printf("RSSI: %d dBm", WiFi.RSSI());
  display.setCursor(0, 46);
  display.printf("Uptime: %lu s", millis() / 1000);
}

void weatherTask(void * parameter) {
  char url[160];
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      String city = preferences.getString("city", "Rome");
      String apiKey = preferences.getString("apikey", "");
      if (apiKey.length() > 0) {
        snprintf(url, sizeof(url), "http://api.openweathermap.org/data/2.5/weather?q=%s&units=metric&appid=%s", city.c_str(), apiKey.c_str());
        HTTPClient http;
        http.begin(url);
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
          StaticJsonDocument<1024> doc;
          DeserializationError error = deserializeJson(doc, http.getStream());
          if (!error) {
            if (xSemaphoreTake(weatherMutex, portMAX_DELAY) == pdTRUE) {
              weather.temp = doc["main"]["temp"];
              weather.humidity = doc["main"]["humidity"];
              const char* desc = doc["weather"][0]["main"];
              snprintf(weather.description, sizeof(weather.description), "%s", desc);
              weather.isRainy = (strstr(desc, "Rain") || strstr(desc, "Drizzle") || strstr(desc, "Thunderstorm"));
              weather.updated = true;
              xSemaphoreGive(weatherMutex);
            }
          }
        }
        http.end();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(600000));
  }
}

void setupCaptivePortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("DeskBuddy-Setup");
  dnsServer.start(53, "*", WiFi.softAPIP());
  server.on("/", []() {
    String html = "<h2>DeskBuddy Config</h2><form action='/save' method='POST'>"
                  "SSID: <input type='text' name='s'><br>"
                  "Pass: <input type='password' name='p'><br>"
                  "Citta: <input type='text' name='c'><br>"
                  "OpenWeather API Key: <input type='text' name='k'><br>"
                  "Telegram Bot Token: <input type='text' name='bt'><br><br>"
                  "<input type='submit' value='Salva e Riavvia'></form>";
    server.send(200, "text/html", html);
  });
  server.on("/save", []() {
    preferences.putString("ssid", server.arg("s"));
    preferences.putString("pass", server.arg("p"));
    preferences.putString("city", server.arg("c"));
    preferences.putString("apikey", server.arg("k"));
    preferences.putString("bottoken", server.arg("bt"));
    server.send(200, "text/html", "Salvato! Riavvio...");
    delay(2000);
    ESP.restart();
  });
  server.onNotFound([]() {
    server.sendHeader("Location", String("http://192.168.4.1/"), true);
    server.send(302, "text/plain", "");
  });
  server.begin();
}