#include <FastLED.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>


/*
/ Version 2.2
FIX: stotteren opgelost
- FastLED.delay() vervangen door millis()-gebaseerde timing
- myDFPlayer.read() verwijderd uit playPhaseAudio() (blokkerende UART)
- delay() in stopGame() en handleButtonPress() vervangen door non-blocking state machine
- Dubbele aanroep gPatterns[] in attract-loop verwijderd
/ Version 2.1
sound added
still stuttering
/ Version 2.0
DF player implementation
*/

// Access Point configuratie
const char* ssid = "Ledstrip_Game";
const char* password = "12345678";

// Webserver en WebSocket
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Game variabelen
int punten = 0;
int level = 1;

// LED configuratie
#define LED_PIN 3
#define NUM_LEDS 300
#define LED_TYPE WS2811
#define COLOR_ORDER RGB
#define BRIGHTNESS 100

CRGB leds[NUM_LEDS];

// Druktoets pinnen
#define BTN_RED 8
#define BTN_BLUE 20
#define BTN_YELLOW 10
#define BTN_GREEN 21

// LED pinnen in de knoppen
#define LED_BTN_RED 5
#define LED_BTN_BLUE 7
#define LED_BTN_YELLOW 6
#define LED_BTN_GREEN 9

/* Bedrading:
 *   DFPlayer TX  -> ESP32-C3 D2 (RX)
 *   DFPlayer RX  -> ESP32-C3 D0 (TX)
 *   DFPlayer VCC -> 5V
 *   DFPlayer GND -> GND
 *   DFPlayer SPK1/SPK2 -> Luidspreker (4-8 Ω)

 * SD-kaart indeling (FAT32):
 * 1 music.mp3
 * 2 game-countdown.mp3
 * 3 failure.mp3
 * 4 lost-the-game-effect.mp3
 * 5 winning.mp3
 * 6 startup.mp3
 * 7 fail
 * 8 winner-bell.mp3
 * 9 game-ui-6.mp3
 * 10 key.mp3
 * 11 lose_funny.mp3
 * 12 game-bonus.mp3
 * 13 gamer-music.mp3
 * 14 arcade-collection.mp3
 * 15 what-a-fuck.mp3
*/ 

// ── Spelfasen ───────────────────────────────────────────────────
enum GamePhase {
  PHASE_IDLE       = 1,
  PHASE_STARTING   = 2,
  PHASE_PLAYING    = 4,
  PHASE_WARNING    = 3,
  PHASE_WIN        = 5,
  PHASE_GAMEOVER   = 7,
  PHASE_PAUSED     = 0,
  PHASE_BONUS      = 9,
  PHASE_STARTUP    = 6,
  PHASE_SAD        = 11,
  PHASE_KEY        = 10,
  PHASE_ARCADE     = 14
};

// ── Pin definities ──────────────────────────────────────────────
#define DFPLAYER_RX_PIN   D0
#define DFPLAYER_TX_PIN   D2
int VOLUME = 10;

// ── Globals ─────────────────────────────────────────────────────
HardwareSerial mySerial(1);
DFRobotDFPlayerMini myDFPlayer;

// Audio wachtrij — playPhaseAudio() stuurt NOOIT direct vanuit de game-loop
// maar zet een verzoek in de wachtrij. serviceAudio() verstuurt het veilig.
static GamePhase audioQueue      = (GamePhase)-1;  // -1 = geen verzoek
static bool      audioQueueReady = false;
static unsigned long lastAudioTime = 0;
#define AUDIO_MIN_INTERVAL_MS 300  // minimale tijd tussen twee DFPlayer commando's

// Interrupt flags
volatile bool btnRedPressed = false;
volatile bool btnBluePressed = false;
volatile bool btnYellowPressed = false;
volatile bool btnGreenPressed = false;
volatile unsigned long lastInterruptTime = 0;
#define lastInterruptMs 400
#define DEBOUNCE_DELAY 200

// Game states
enum GameState {
  STATE_ATTRACT,
  STATE_PLAYING,
  STATE_STOPPING   // FIX: non-blocking stop toestand
};

GameState currentState = STATE_ATTRACT;

// FIX: Non-blocking stop state machine
unsigned long stopPhaseTimer = 0;
int stopPhaseStep = 0;

// Knight Rider variabelen
int knightRiderPos = 0;
bool knightRiderDirection = true;
unsigned long lastKnightRiderUpdate = 0;
#define KNIGHT_RIDER_SPEED 20

// FIX: attract timing zonder FastLED.delay()
unsigned long lastAttractFrame = 0;
#define ATTRACT_FRAME_MS 8   // ~120 fps

// Kleuren definitie
enum LedColor {
  COLOR_RED = 0,
  COLOR_BLUE = 1,
  COLOR_YELLOW = 2,
  COLOR_GREEN = 3
};

CRGB getColorRGB(LedColor color) {
  switch(color) {
    case COLOR_RED: return CRGB::Red;
    case COLOR_BLUE: return CRGB::Blue;
    case COLOR_YELLOW: return CRGB::Yellow;
    case COLOR_GREEN: return CRGB::Green;
    default: return CRGB::Black;
  }
}

int getButtonLedPin(LedColor color) {
  switch(color) {
    case COLOR_RED: return LED_BTN_RED;
    case COLOR_BLUE: return LED_BTN_BLUE;
    case COLOR_YELLOW: return LED_BTN_YELLOW;
    case COLOR_GREEN: return LED_BTN_GREEN;
    default: return LED_BTN_RED;
  }
}

// Spelstatus
struct Led {
  float position;
  LedColor color;
  bool active;
};

#define MAX_LEDS 10
Led activeLeds[MAX_LEDS];
int numActiveLeds = 0;
int ledsExtinguished = 0;
float baseSpeed = 0.2;
float currentSpeed = baseSpeed;
unsigned long lastMoveTime = 0;
#define FRAME_DELAY 20
int ledsMissed = 0;
bool win = false;

// Bullet systeem
struct Bullet {
  float position;
  LedColor color;
  bool active;
};

#define MAX_BULLETS 4
#define BULLET_SPEED 1.5f
Bullet bullets[MAX_BULLETS];
int numBullets = 0;

// Knop LED feedback timing
unsigned long buttonLedOffTime = 0;
int activeButtonLed = -1;


// Interrupt handlers
void btnRedISR() {
  unsigned long currentTime = millis();
  if (currentTime - lastInterruptTime > lastInterruptMs) {
    btnRedPressed = true;
    lastInterruptTime = currentTime;
  }
}

void btnBlueISR() {
  unsigned long currentTime = millis();
  if (currentTime - lastInterruptTime > lastInterruptMs) {
    btnBluePressed = true;
    lastInterruptTime = currentTime;
  }
}

void btnYellowISR() {
  unsigned long currentTime = millis();
  if (currentTime - lastInterruptTime > lastInterruptMs) {
    btnYellowPressed = true;
    lastInterruptTime = currentTime;
  }
}

void btnGreenISR() {
  unsigned long currentTime = millis();
  if (currentTime - lastInterruptTime > lastInterruptMs) {
    btnGreenPressed = true;
    lastInterruptTime = currentTime;
  }
}


//////////////////
/// webserver ///
/////////////////

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Ledstrip Game</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            color: white;
        }
        .container {
            text-align: center;
            padding: 20px;
            background: rgba(255, 255, 255, 0.1);
            border-radius: 20px;
            backdrop-filter: blur(10px);
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
            min-width: 300px;
        }
        h1 { font-size: 2.5em; margin-bottom: 40px; text-shadow: 2px 2px 4px rgba(0,0,0,0.3); }
        .score-container { margin: 30px 0; }
        .punten {
            font-size: 6em; font-weight: bold; color: #ffd700;
            text-shadow: 3px 3px 6px rgba(0,0,0,0.5);
            margin: 20px 0; transition: transform 0.3s ease;
        }
        .punten.update { transform: scale(1.1); }
        .level { font-size: 2em; color: #87ceeb; margin: 15px 0; transition: transform 0.3s ease; }
        .level.update { transform: scale(1.1); }
        .label { font-size: 1.2em; opacity: 0.9; margin-bottom: 5px; }
        .status { margin-top: 30px; padding: 10px; border-radius: 10px; font-size: 0.9em; }
        .status.connected { background: rgba(0,255,0,0.2); color: #90EE90; }
        .status.disconnected { background: rgba(255,0,0,0.2); color: #FFB6C1; }
    </style>
</head>
<body>
    <div class="container">
        <h1>🎮 Ledstrip Game</h1>
        <div class="score-container">
            <div class="label">PUNTEN</div>
            <div class="punten" id="punten">0</div>
        </div>
        <div class="score-container">
            <div class="label">LEVEL</div>
            <div class="level" id="level">1</div>
        </div>
        <div class="status disconnected" id="status">Verbinding maken...</div>
    </div>
    <script>
        let websocket;
        let reconnectInterval;
        function initWebSocket() {
            if (websocket && websocket.readyState === WebSocket.CONNECTING) return; // FIX: geen dubbele verbinding
            websocket = new WebSocket('ws://' + window.location.hostname + '/ws');
            websocket.onopen = function() {
                document.getElementById('status').className = 'status connected';
                document.getElementById('status').textContent = '✓ Verbonden';
                clearInterval(reconnectInterval);
                // FIX: heartbeat elke 5s zodat ESP verbinding actief houdt
                clearInterval(window._hb);
                window._hb = setInterval(function() {
                    if (websocket.readyState === WebSocket.OPEN) websocket.send('ping');
                }, 5000);
            };
            websocket.onclose = function() {
                document.getElementById('status').className = 'status disconnected';
                document.getElementById('status').textContent = '✗ Verbinding verbroken - Opnieuw verbinden...';
                clearInterval(window._hb);
                clearInterval(reconnectInterval);
                reconnectInterval = setInterval(initWebSocket, 500); // FIX: was 2000ms, nu 500ms
            };
            websocket.onmessage = function(event) {
                try {
                    const data = JSON.parse(event.data);
                    if (data.punten !== undefined) {
                        const el = document.getElementById('punten');
                        el.textContent = data.punten;
                        el.classList.add('update');
                        setTimeout(() => el.classList.remove('update'), 300);
                    }
                    if (data.level !== undefined) {
                        const el = document.getElementById('level');
                        el.textContent = data.level;
                        el.classList.add('update');
                        setTimeout(() => el.classList.remove('update'), 300);
                    }
                } catch(e) { console.error('Fout:', e); }
            };
        }
        initWebSocket();
    </script>
</body>
</html>
)rawliteral";


// Functie declaraties
void addNewLed();
void moveLeds();
void handleButtonPress();
void updateButtonLeds();
void activateButtonLed(int ledPin);
void deactivateButtonLed(int ledPin);
void ButtonLedsOff();
void compactLedArray();
void compactBulletArray();
void displayLeds();
void startGame();
void stopGame();
void updateStopSequence();   // FIX: non-blocking stop
void buttonKnight();
void setLevel(int nieuwLevel);
void addPunten(int aantal);
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void playPhaseAudio(GamePhase phase);
void serviceAudio();
void fireBullet(LedColor color);
void moveBullets();
void checkBulletCollisions();

// FastLED patronen
void rainbow();
void rainbowWithGlitter();
void confetti();
void sinelon();
void juggle();
void bpm();
void nextPattern();
void addGlitter(fract8 chanceOfGlitter);

typedef void (*SimplePatternList[])();
SimplePatternList gPatterns = { rainbow, rainbowWithGlitter, confetti, sinelon, juggle, bpm };

uint8_t gCurrentPatternNumber = 0;
uint8_t gHue = 0;
#define ARRAY_SIZE(A) (sizeof(A) / sizeof((A)[0]))
#define FRAMES_PER_SECOND 120


void setup() {
  Serial.begin(115200);
  delay(1000);
  mySerial.begin(9600, SERIAL_8N1, 4, 1);
  delay(1000);
  if (!myDFPlayer.begin(mySerial)) {
     Serial.println("DFPlayer niet gevonden!");
     while (true);
   }
  myDFPlayer.volume(VOLUME);
  playPhaseAudio(PHASE_STARTUP);

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();

  pinMode(BTN_RED, INPUT_PULLUP);
  pinMode(BTN_BLUE, INPUT_PULLUP);
  pinMode(BTN_YELLOW, INPUT_PULLUP);
  pinMode(BTN_GREEN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(BTN_RED), btnRedISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(BTN_BLUE), btnBlueISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(BTN_YELLOW), btnYellowISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(BTN_GREEN), btnGreenISR, FALLING);

  pinMode(LED_BTN_RED, OUTPUT);
  pinMode(LED_BTN_BLUE, OUTPUT);
  pinMode(LED_BTN_YELLOW, OUTPUT);
  pinMode(LED_BTN_GREEN, OUTPUT);

  ButtonLedsOff();
  randomSeed(analogRead(0));

  // FIX: WiFi mode expliciet instellen vóór softAP
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(
    IPAddress(192, 168, 4, 1),
    IPAddress(192, 168, 4, 1),
    IPAddress(255, 255, 255, 0)
  );
  // FIX: kanaal 6, max 4 clients, hidden=false
  WiFi.softAP(ssid, password, 6, 0, 4);

  // FIX: wacht tot AP echt actief is (anders start server te vroeg)
  unsigned long apStart = millis();
  while (WiFi.softAPIP() == IPAddress(0,0,0,0) && millis() - apStart < 3000) {
    delay(100);
  }

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP gestart: "); Serial.println(ssid);
  Serial.print("IP: "); Serial.println(IP);

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });
  // FIX: 404 handler zodat browser niet blijft hangen bij onbekende paden
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->redirect("/");
  });
  server.begin();

  // FIX: delay(3000) vervangen door non-blocking wacht via timer
  // Startup geluid speelt al via DFPlayer; geen blocking delay nodig
  playPhaseAudio(PHASE_IDLE);
}


void loop() {
  // FIX: cleanupClients() max 1x per seconde i.p.v. elke loop
  static unsigned long lastWsCleanup = 0;
  if (millis() - lastWsCleanup >= 1000) {
    ws.cleanupClients();
    lastWsCleanup = millis();
  }

  // Stuur audio-verzoek veilig buiten de game-update
  serviceAudio();

  if (currentState == STATE_ATTRACT) {
    // FIX: millis()-gebaseerde timing i.p.v. FastLED.delay()
    unsigned long now = millis();
    if (now - lastAttractFrame >= ATTRACT_FRAME_MS) {
      lastAttractFrame = now;

      buttonKnight();

      // FIX: gPatterns[] slechts EENMAAL aanroepen per frame (was dubbel)
      gPatterns[gCurrentPatternNumber]();
      FastLED.show();

      EVERY_N_MILLISECONDS(20) { gHue++; }
      EVERY_N_SECONDS(10) { nextPattern(); }
    }

    if (btnRedPressed || btnBluePressed || btnYellowPressed || btnGreenPressed) {
      btnRedPressed = false;
      btnBluePressed = false;
      btnYellowPressed = false;
      btnGreenPressed = false;
      startGame();
    }
  }
  else if (currentState == STATE_PLAYING) {
    if (millis() - lastMoveTime >= FRAME_DELAY) {
      lastMoveTime = millis();
      moveLeds();
      moveBullets();
      checkBulletCollisions();
      displayLeds();
    }
    handleButtonPress();
    updateButtonLeds();
  }
  else if (currentState == STATE_STOPPING) {
    // FIX: non-blocking stop sequentie
    updateStopSequence();
  }
}


// ════════════════════════════════════════════════════════════════
//  FIX: Non-blocking stop sequentie (vervangt delay() in stopGame)
// ════════════════════════════════════════════════════════════════
void updateStopSequence() {
  unsigned long now = millis();
  switch (stopPhaseStep) {
    case 0:
      // Direct na stopGame(): clear strip, start gameover geluid
      FastLED.clear();
      FastLED.show();
      playPhaseAudio(PHASE_GAMEOVER);
      stopPhaseTimer = now + 2000;
      stopPhaseStep = 1;
      break;
    case 1:
      if (now >= stopPhaseTimer) {
        playPhaseAudio(PHASE_ARCADE);
        stopPhaseTimer = now + 2000;
        stopPhaseStep = 2;
      }
      break;
    case 2:
      if (now >= stopPhaseTimer) {
        // Klaar: ga naar attract mode
        currentState = STATE_ATTRACT;
        stopPhaseStep = 0;
      }
      break;
  }
}


// ════════════════════════════════════════════════════════════════
//  Audio wachtrij
//  playPhaseAudio() zet alleen een verzoek klaar — geen UART in game-loop
//  serviceAudio()   verstuurt het verzoek veilig vanuit loop()
// ════════════════════════════════════════════════════════════════
void playPhaseAudio(GamePhase phase) {
  // Sla het verzoek op; hogere prioriteit overschrijft lagere
  // WIN en GAMEOVER mogen een BONUS-verzoek overschrijven
  if (!audioQueueReady ||
      phase == PHASE_WIN || phase == PHASE_GAMEOVER ||
      phase == PHASE_STARTUP || phase == PHASE_IDLE) {
    audioQueue      = phase;
    audioQueueReady = true;
  }
}

void serviceAudio() {
  if (!audioQueueReady) return;
  if (millis() - lastAudioTime < AUDIO_MIN_INTERVAL_MS) return;

  GamePhase phase = audioQueue;
  audioQueueReady = false;
  lastAudioTime   = millis();

  Serial.print("Audio>> "); Serial.println((int)phase);
  switch (phase) {
    case PHASE_IDLE:     myDFPlayer.play(PHASE_IDLE);     break;
    case PHASE_KEY:      myDFPlayer.play(PHASE_KEY);      break;
    case PHASE_SAD:      myDFPlayer.play(PHASE_SAD);      break;
    case PHASE_STARTING: myDFPlayer.play(PHASE_STARTING); break;
    case PHASE_PLAYING:  myDFPlayer.play(PHASE_PLAYING);  break;
    case PHASE_WARNING:  myDFPlayer.play(PHASE_WARNING);  break;
    case PHASE_WIN:      myDFPlayer.play(PHASE_WIN);      break;
    case PHASE_GAMEOVER: myDFPlayer.play(PHASE_GAMEOVER); break;
    case PHASE_ARCADE:   myDFPlayer.play(PHASE_ARCADE);   break;
    case PHASE_BONUS:    myDFPlayer.play(PHASE_BONUS);    break;
    case PHASE_PAUSED:   myDFPlayer.pause();              break;
    case PHASE_STARTUP:  myDFPlayer.play(PHASE_STARTUP);  break;
    default:             myDFPlayer.stop();               break;
  } 
}


void startGame() {
  Serial.println("Start Spel");
  currentState = STATE_PLAYING;

  numActiveLeds = 0;
  ledsExtinguished = 0;
  currentSpeed = baseSpeed;
  level = 1;
  punten = 0;
  numBullets = 0;

  FastLED.clear();
  FastLED.show();
  ButtonLedsOff();

  addNewLed();
  playPhaseAudio(PHASE_STARTING);
}

void stopGame() {
  Serial.println("Stop Spel");

  // Reset Knight Rider voor als we terugkeren
  knightRiderPos = 0;
  knightRiderDirection = true;
  numActiveLeds = 0;
  numBullets = 0;

  // FIX: geen delay() meer hier — overgaan naar non-blocking STATE_STOPPING
  stopPhaseStep = 0;
  currentState = STATE_STOPPING;
}


// ─── Bullet functies ──────────────────────────────────────────────────────────

void fireBullet(LedColor color) {
  if (numBullets >= MAX_BULLETS) return;
  bullets[numBullets].position = 0.0f;
  bullets[numBullets].color    = color;
  bullets[numBullets].active   = true;
  numBullets++;
  Serial.print("Bullet afgeschoten, kleur: "); Serial.println(color);
}

// ── Hulpfunctie: compacteer arrays ───────────────────────────────────────────
void compactLedArray() {
  int w = 0;
  for (int r = 0; r < numActiveLeds; r++) {
    if (activeLeds[r].active) {
      if (w != r) activeLeds[w] = activeLeds[r];
      w++;
    }
  }
  numActiveLeds = w;
}

void compactBulletArray() {
  int w = 0;
  for (int r = 0; r < numBullets; r++) {
    if (bullets[r].active) {
      if (w != r) bullets[w] = bullets[r];
      w++;
    }
  }
  numBullets = w;
}

// ── Verplaats bullets ─────────────────────────────────────────────────────────
void moveBullets() {
  for (int b = 0; b < numBullets; b++) {
    if (!bullets[b].active) continue;
    bullets[b].position += BULLET_SPEED;
    if (bullets[b].position >= NUM_LEDS) bullets[b].active = false;
  }
  compactBulletArray();
}

// ── Botsingsdetectie ──────────────────────────────────────────────────────────
// Aanroepvolgorde in loop: moveLeds → moveBullets → checkBulletCollisions
// Na moveLeds en moveBullets zijn beide arrays al gecompacteerd.
// checkBulletCollisions voegt GEEN nieuwe LEDs toe tijdens de botsingsloop;
// alle array-mutaties gebeuren NA beide loops.
void checkBulletCollisions() {
  int hitsThisFrame = 0;
  bool wrongColor   = false;

  for (int b = 0; b < numBullets; b++) {
    if (!bullets[b].active) continue;

    for (int i = 0; i < numActiveLeds; i++) {
      if (!activeLeds[i].active) continue;  // al geraakt door eerdere bullet

      float dist = bullets[b].position - activeLeds[i].position;
      if (dist < 0) dist = -dist;

      if (dist <= 1.5f) {
        if (bullets[b].color == activeLeds[i].color) {
          // Juiste kleur: markeer beide inactief — array NIET aanpassen tijdens loop
          bullets[b].active    = false;
          activeLeds[i].active = false;
          hitsThisFrame++;
        } else {
          // Verkeerde kleur: alleen bullet weg
          bullets[b].active = false;
          wrongColor = true;
        }
        break;  // één bullet → maximaal één blokje
      }
    }
  }

  // Compacteer NA beide loops — nooit tijdens
  if (hitsThisFrame > 0 || wrongColor) compactBulletArray();

  // ── Verwerk treffers ──────────────────────────────────────────────────────
  if (hitsThisFrame > 0) {
    compactLedArray();  // verwijder inactieve LEDs

    // Punten en teller bijhouden
    addPunten(10 * hitsThisFrame);
    int prevExtinguished = ledsExtinguished;
    ledsExtinguished += hitsThisFrame;
    ledsMissed = 0;

    // Level-up: controleer of we de drempel van 10 gepasseerd zijn
    bool levelUp = (ledsExtinguished / 10) > (prevExtinguished / 10);

    if (levelUp) {
      playPhaseAudio(PHASE_WIN);  // stop + win via queue
      setLevel(level + 1);
      // Bij level-up: voeg 2 nieuwe LEDs toe, maar nooit boven MAX_LEDS
      int numToAdd = min(2, MAX_LEDS - numActiveLeds);
      for (int j = 0; j < numToAdd; j++) addNewLed();
    } else {
      playPhaseAudio(PHASE_BONUS);
      // Vervang precies de geraakte LEDs — nooit boven MAX_LEDS
      int numToAdd = min(hitsThisFrame, MAX_LEDS - numActiveLeds);
      for (int j = 0; j < numToAdd; j++) addNewLed();
    }
  }

  if (wrongColor) {
    playPhaseAudio(PHASE_WARNING);
    Serial.println("Verkeerde kleur hit");
  }
}

void displayLeds() {
  FastLED.clear();
  for (int i = 0; i < numActiveLeds; i++) {
    if (activeLeds[i].active) {
      int mainPos = (int)activeLeds[i].position;
      if (mainPos >= 0 && mainPos < NUM_LEDS) {
        leds[mainPos] = getColorRGB(activeLeds[i].color);
      }
    }
  }
  for (int b = 0; b < numBullets; b++) {
    if (bullets[b].active) {
      int bPos = (int)bullets[b].position;
      if (bPos >= 0 && bPos < NUM_LEDS) {
        CRGB bulletColor = getColorRGB(bullets[b].color);
        bulletColor.nscale8(180);
        leds[bPos] = bulletColor + CRGB(40, 40, 40);
      }
    }
  }
  FastLED.show();
}


void buttonKnight() {
  if (knightRiderDirection) {
    knightRiderPos++;
    switch (knightRiderPos) {
      case 80: activateButtonLed(LED_BTN_GREEN);  break;
      case 60: activateButtonLed(LED_BTN_YELLOW); break;
      case 40: activateButtonLed(LED_BTN_RED);    break;
      case 20: activateButtonLed(LED_BTN_BLUE);   break;
    }
    if (knightRiderPos >= NUM_LEDS - 1) {
      knightRiderDirection = false;
      ButtonLedsOff();
    }
  } else {
    knightRiderPos--;
    switch (knightRiderPos) {
      case 80: activateButtonLed(LED_BTN_GREEN);  break;
      case 60: activateButtonLed(LED_BTN_YELLOW); break;
      case 40: activateButtonLed(LED_BTN_RED);    break;
      case 20: activateButtonLed(LED_BTN_BLUE);   break;
    }
    if (knightRiderPos <= 0) {
      knightRiderDirection = true;
      ButtonLedsOff();
    }
  }
}


///////////////////////////
// FastLED show patterns //
///////////////////////////

void nextPattern() {
  gCurrentPatternNumber = (gCurrentPatternNumber + 1) % ARRAY_SIZE(gPatterns);
}

void rainbow() {
  fill_rainbow(leds, NUM_LEDS, gHue, 7);
}

void rainbowWithGlitter() {
  rainbow();
  addGlitter(80);
}

void addGlitter(fract8 chanceOfGlitter) {
  if (random8() < chanceOfGlitter) {
    leds[random16(NUM_LEDS)] += CRGB::White;
  }
}

void confetti() {
  fadeToBlackBy(leds, NUM_LEDS, 10);
  int pos = random16(NUM_LEDS);
  leds[pos] += CHSV(gHue + random8(64), 200, 255);
}

void sinelon() {
  fadeToBlackBy(leds, NUM_LEDS, 20);
  int pos = beatsin16(13, 0, NUM_LEDS - 1);
  leds[pos] += CHSV(gHue, 255, 192);
}

void bpm() {
  uint8_t BeatsPerMinute = 62;
  CRGBPalette16 palette = PartyColors_p;
  uint8_t beat = beatsin8(BeatsPerMinute, 64, 255);
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = ColorFromPalette(palette, gHue + (i * 2), beat - gHue + (i * 10));
  }
}

void juggle() {
  fadeToBlackBy(leds, NUM_LEDS, 20);
  uint8_t dothue = 0;
  for (int i = 0; i < 8; i++) {
    leds[beatsin16(i + 7, 0, NUM_LEDS - 1)] |= CHSV(dothue, 200, 255);
    dothue += 32;
  }
}


// WebSocket event handler
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WebSocket client #%u verbonden\n", client->id());
    // FIX: ping elke 10s zodat verbinding niet stil wegvalt (keep-alive)
    client->ping();
    String json = "{\"punten\":" + String(punten) + ",\"level\":" + String(level) + "}";
    client->text(json);
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WebSocket client #%u verbroken\n", client->id());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      data[len] = 0;
      String msg = (char*)data;
      Serial.printf("Commando: %s\n", msg.c_str());
      if      (msg == "ping")    { /* heartbeat, geen actie */ }
      else if (msg == "add10")   punten += 10;
      else if (msg == "add100")  punten += 100;
      else if (msg == "levelup") level++;
      else if (msg == "reset")   { punten = 0; level = 1; }
      String json = "{\"punten\":" + String(punten) + ",\"level\":" + String(level) + "}";
      ws.textAll(json);
    }
  }
}

void addPunten(int aantal) {
  punten += aantal;
  String json = "{\"punten\":" + String(punten) + ",\"level\":" + String(level) + "}";
  ws.textAll(json);
  Serial.printf("Punten: +%d (Totaal: %d)\n", aantal, punten);
}

void setLevel(int nieuwLevel) {
  level = nieuwLevel;
  String json = "{\"punten\":" + String(punten) + ",\"level\":" + String(level) + "}";
  ws.textAll(json);
  Serial.printf("Level: %d\n", level);
}

void addNewLed() {
  if (numActiveLeds < MAX_LEDS) {
    float highestPosition = -10.0;
    for (int i = 0; i < numActiveLeds; i++) {
      if (activeLeds[i].active && activeLeds[i].position > highestPosition) {
        highestPosition = activeLeds[i].position;
      }
    }
    float newPosition = NUM_LEDS - 1;
    if (highestPosition > NUM_LEDS - 6) {
      newPosition = highestPosition + 5;
    }
    activeLeds[numActiveLeds].position = newPosition;
    activeLeds[numActiveLeds].color    = (LedColor)random(0, 4);
    activeLeds[numActiveLeds].active   = true;
    numActiveLeds++;
  }
}

void moveLeds() {
  int missedThisFrame = 0;
  for (int i = 0; i < numActiveLeds; i++) {
    if (activeLeds[i].active) {
      activeLeds[i].position -= currentSpeed;
      if (activeLeds[i].position < 0) {
        activeLeds[i].active = false;
        missedThisFrame++;
      }
    }
  }
  if (missedThisFrame > 0) {
    compactLedArray();
    ledsMissed += missedThisFrame;
    playPhaseAudio(PHASE_SAD);
    if (ledsMissed > 2) {
      ledsMissed = 0;
      stopGame();
      return;  // niet verder — spel stopt
    }
    // FIX: voeg LEDs toe na compact, maar bewaar ruimte voor checkBulletCollisions
    // Maximaal tot (MAX_LEDS - numBullets) zodat gelijktijdige botsing niet overloopt
    int safeMax = MAX_LEDS - numBullets;
    if (safeMax < 1) safeMax = 1;
    int numToAdd = min(missedThisFrame, safeMax - numActiveLeds);
    for (int j = 0; j < numToAdd; j++) addNewLed();
  }
}

void activateButtonLed(int ledPin) {
  digitalWrite(ledPin, HIGH);
  activeButtonLed = ledPin;
  buttonLedOffTime = millis() + 200;
}

void deactivateButtonLed(int ledPin) {
  digitalWrite(ledPin, LOW);
}

void ButtonLedsOff() {
  deactivateButtonLed(LED_BTN_YELLOW);
  deactivateButtonLed(LED_BTN_GREEN);
  deactivateButtonLed(LED_BTN_BLUE);
  deactivateButtonLed(LED_BTN_RED);
  activeButtonLed = -1;
}

void updateButtonLeds() {
  if (activeButtonLed >= 0 && millis() >= buttonLedOffTime) {
    digitalWrite(activeButtonLed, LOW);
    activeButtonLed = -1;
  }
}

void handleButtonPress() {
  // FIX: delay(1000) vervangen — bij twee-knops combinatie direct vlaggen wissen
  // en stoppen zonder te blokkeren
  if (digitalRead(BTN_GREEN) == LOW && digitalRead(BTN_BLUE) == LOW) {
    btnRedPressed = false;
    btnBluePressed = false;
    btnYellowPressed = false;
    btnGreenPressed = false;

    Serial.println("2 knoppen: volume + stop");
    VOLUME += 10;
    if (VOLUME > 30) VOLUME = 5;
     myDFPlayer.volume(VOLUME);
    Serial.print("Volume > "); Serial.println(VOLUME);

    stopGame();
    return;
  }

  LedColor pressedColor;
  bool buttonPressed = false;
  int buttonLedPin;

  if (btnRedPressed) {
    pressedColor = COLOR_RED;   buttonLedPin = LED_BTN_RED;    buttonPressed = true; btnRedPressed = false;
  } else if (btnBluePressed) {
    pressedColor = COLOR_BLUE;  buttonLedPin = LED_BTN_BLUE;   buttonPressed = true; btnBluePressed = false;
  } else if (btnYellowPressed) {
    pressedColor = COLOR_YELLOW; buttonLedPin = LED_BTN_YELLOW; buttonPressed = true; btnYellowPressed = false;
  } else if (btnGreenPressed) {
    pressedColor = COLOR_GREEN; buttonLedPin = LED_BTN_GREEN;  buttonPressed = true; btnGreenPressed = false;
  }

  if (buttonPressed) {
    activateButtonLed(buttonLedPin);
    fireBullet(pressedColor);
  }
}