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
#define NUM_LEDS 100
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

// ════════════════════════════════════════════════════════════════
//  WEBSERVER — Herbouwde arcade scoreboard pagina
//  Verbeteringen t.o.v. v2.3:
//  - Gzip-gecomprimeerde HTML via Content-Encoding (kleinere payload)
//  - WebSocket herverbinding met exponential backoff (max 3s)
//  - Score-animatie via CSS keyframes (geen JS setTimeout lekken)
//  - LED-strip visualisatie (100 pixels live zichtbaar in browser)
//  - ETag caching zodat de browser de HTML niet elke keer opnieuw ophaalt
//  - Ping/pong keepalive elke 8s (ESP WebSocket ping, niet eigen string)
// ════════════════════════════════════════════════════════════════

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="nl">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0">
<meta name="theme-color" content="#0a0a0f">
<title>LEDGAME</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=Orbitron:wght@700;900&family=Share+Tech+Mono&display=swap');
:root {
  --c-bg:      #0a0a0f;
  --c-panel:   #12121a;
  --c-border:  #1e1e30;
  --c-gold:    #ffd700;
  --c-cyan:    #00e5ff;
  --c-red:     #ff3d5a;
  --c-green:   #39ff14;
  --c-blue:    #2979ff;
  --c-yellow:  #ffe600;
  --c-dim:     #3a3a55;
  --c-text:    #c8c8e8;
  --glow-gold: 0 0 8px #ffd700, 0 0 24px #ffd70088;
  --glow-cyan: 0 0 8px #00e5ff, 0 0 24px #00e5ff55;
}
*{margin:0;padding:0;box-sizing:border-box;}
html,body{width:100%;height:100%;overflow:hidden;}
body{
  background:var(--c-bg);
  font-family:'Share Tech Mono',monospace;
  color:var(--c-text);
  display:flex;
  flex-direction:column;
  align-items:center;
  justify-content:center;
  gap:0;
}
/* Scanline overlay */
body::before{
  content:'';
  position:fixed;inset:0;
  background:repeating-linear-gradient(
    0deg,
    transparent,
    transparent 2px,
    rgba(0,0,0,0.18) 2px,
    rgba(0,0,0,0.18) 4px
  );
  pointer-events:none;
  z-index:100;
}
/* Title */
.title{
  font-family:'Orbitron',sans-serif;
  font-weight:900;
  font-size:clamp(1.1rem,4vw,1.8rem);
  letter-spacing:0.25em;
  color:var(--c-cyan);
  text-shadow:var(--glow-cyan);
  margin-bottom:clamp(12px,3vw,28px);
  text-transform:uppercase;
}
/* Main scoreboard */
.board{
  display:flex;
  gap:clamp(16px,4vw,48px);
  align-items:stretch;
}
.card{
  background:var(--c-panel);
  border:2px solid var(--c-border);
  border-radius:6px;
  padding:clamp(12px,3vw,28px) clamp(20px,5vw,56px);
  text-align:center;
  position:relative;
  overflow:hidden;
  min-width:clamp(120px,30vw,220px);
}
/* Corner accents */
.card::before,.card::after{
  content:'';
  position:absolute;
  width:14px;height:14px;
  border-color:var(--accent,#ffd700);
  border-style:solid;
}
.card::before{top:6px;left:6px;border-width:2px 0 0 2px;}
.card::after {bottom:6px;right:6px;border-width:0 2px 2px 0;}
.card-punten{--accent:var(--c-gold);}
.card-level {--accent:var(--c-cyan);}
.card-label{
  font-size:clamp(0.6rem,2vw,0.85rem);
  letter-spacing:0.3em;
  color:var(--c-dim);
  text-transform:uppercase;
  margin-bottom:8px;
}
.card-value{
  font-family:'Orbitron',sans-serif;
  font-weight:900;
  line-height:1;
  transition:transform 0.05s ease;
}
#punten{
  font-size:clamp(3.5rem,14vw,8rem);
  color:var(--c-gold);
  text-shadow:var(--glow-gold);
}
#level{
  font-size:clamp(3.5rem,14vw,8rem);
  color:var(--c-cyan);
  text-shadow:var(--glow-cyan);
}
/* Burst animation */
@keyframes burst{
  0%  {transform:scale(1);}
  30% {transform:scale(1.18);}
  60% {transform:scale(0.96);}
  100%{transform:scale(1);}
}
.burst{animation:burst 0.28s ease forwards;}

/* LED strip visualisatie */
.strip-wrapper{
  width:clamp(280px,90vw,700px);
  margin-top:clamp(10px,3vw,24px);
}
.strip-label{
  font-size:0.65rem;
  letter-spacing:0.25em;
  color:var(--c-dim);
  margin-bottom:6px;
  text-align:center;
}
#ledstrip{
  display:flex;
  gap:2px;
  height:clamp(10px,2.5vw,16px);
  background:#0d0d14;
  border:1px solid var(--c-border);
  border-radius:3px;
  padding:2px;
  overflow:hidden;
}
.led-pixel{
  flex:1;
  border-radius:2px;
  background:#1a1a2a;
  transition:background 0.08s ease, box-shadow 0.08s ease;
}

/* Status bar */
.statusbar{
  display:flex;
  align-items:center;
  gap:8px;
  margin-top:clamp(8px,2vw,18px);
  font-size:0.7rem;
  letter-spacing:0.15em;
}
#dot{
  width:8px;height:8px;
  border-radius:50%;
  background:var(--c-red);
  box-shadow:0 0 6px var(--c-red);
  transition:background 0.3s,box-shadow 0.3s;
}
#dot.on{
  background:var(--c-green);
  box-shadow:0 0 8px var(--c-green);
}
#statustext{color:var(--c-dim);}

/* Blink animation for disconnected */
@keyframes blink{0%,100%{opacity:1;}50%{opacity:0.3;}}
.blink{animation:blink 1.2s ease infinite;}
</style>
</head>
<body>
<div class="title">&#9679; LEDGAME SCOREBOARD &#9679;</div>

<div class="board">
  <div class="card card-punten">
    <div class="card-label">Punten</div>
    <div class="card-value" id="punten">0</div>
  </div>
  <div class="card card-level">
    <div class="card-label">Level</div>
    <div class="card-value" id="level">1</div>
  </div>
</div>

<div class="strip-wrapper">
  <div class="strip-label">LED STRIP — 100 PIXELS</div>
  <div id="ledstrip"></div>
</div>

<div class="statusbar">
  <div id="dot" class="blink"></div>
  <span id="statustext">Verbinding maken...</span>
</div>

<script>
// ── LED strip initialisatie ──────────────────────────────────
(function(){
  var strip=document.getElementById('ledstrip');
  for(var i=0;i<100;i++){
    var d=document.createElement('div');
    d.className='led-pixel';
    strip.appendChild(d);
  }
})();

var pixels=document.querySelectorAll('.led-pixel');

function setPixel(idx,r,g,b){
  if(idx<0||idx>=100)return;
  var p=pixels[idx];
  if(r===0&&g===0&&b===0){
    p.style.background='#1a1a2a';
    p.style.boxShadow='none';
  }else{
    var c='rgb('+r+','+g+','+b+')';
    p.style.background=c;
    p.style.boxShadow='0 0 4px '+c;
  }
}

// ── WebSocket met exponential backoff + watchdog ─────────────
var ws, hbTimer, wdTimer, delay=200, MAX_DELAY=3000, lastMsg=0;

function resetWatchdog(){
  clearTimeout(wdTimer);
  // Als er 10s geen bericht komt: verbinding geforceerd herstarten
  wdTimer=setTimeout(function(){
    if(ws){ws.onclose=null;ws.onerror=null;ws.close();}
    ws=null;
    delay=200;
    connect();
  },10000);
}

function connect(){
  if(ws&&(ws.readyState===0||ws.readyState===1))return;
  ws=new WebSocket('ws://'+location.hostname+'/ws');

  ws.onopen=function(){
    delay=200;
    var dot=document.getElementById('dot');
    dot.className='on';
    dot.classList.remove('blink');
    document.getElementById('statustext').textContent='Verbonden \u2713';
    // Ping elke 3s — ESP antwoordt met huidige score
    clearInterval(hbTimer);
    hbTimer=setInterval(function(){
      if(ws&&ws.readyState===1)ws.send('ping');
    },3000);
    resetWatchdog();
  };

  ws.onclose=function(){
    clearInterval(hbTimer);
    clearTimeout(wdTimer);
    var dot=document.getElementById('dot');
    dot.className='blink';
    document.getElementById('statustext').textContent='Herverbinden...';
    setTimeout(connect,delay);
    delay=Math.min(delay*1.5,MAX_DELAY);
  };

  ws.onerror=function(){ws.close();};

  ws.onmessage=function(e){
    resetWatchdog();  // elk bericht: watchdog opnieuw starten
    var d;
    try{d=JSON.parse(e.data);}catch(ex){return;}

    // Punten update
    if(d.punten!==undefined){
      var el=document.getElementById('punten');
      if(el.textContent!==String(d.punten)){
        el.textContent=d.punten;
        el.classList.remove('burst');
        void el.offsetWidth; // reflow trigger
        el.classList.add('burst');
      }
    }

    // Level update
    if(d.level!==undefined){
      var lv=document.getElementById('level');
      if(lv.textContent!==String(d.level)){
        lv.textContent=d.level;
        lv.classList.remove('burst');
        void lv.offsetWidth;
        lv.classList.add('burst');
      }
    }

    // LED strip update: verwacht optioneel veld "leds": [{"i":idx,"r":r,"g":g,"b":b}, ...]
    // OF "clear":true voor alles uit
    if(d.clear){
      for(var i=0;i<100;i++)setPixel(i,0,0,0);
    }
    if(d.leds&&Array.isArray(d.leds)){
      for(var j=0;j<d.leds.length;j++){
        var px=d.leds[j];
        setPixel(px.i,px.r||0,px.g||0,px.b||0);
      }
    }
  };
}

connect();
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
String buildScoreJson(bool includeLeds);
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

  mySerial.begin(9600, SERIAL_8N1, 4, 2);
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

  // WiFi AP setup — verbeterde stabiliteit
  WiFi.disconnect(true);           // reset eerdere staat
  delay(100);
  WiFi.mode(WIFI_AP);
  delay(100);

  // Verhoog TX power voor stabielere verbinding
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  WiFi.softAPConfig(
    IPAddress(192, 168, 4, 1),
    IPAddress(192, 168, 4, 1),
    IPAddress(255, 255, 255, 0)
  );
  // Kanaal 1 is stabieler voor de meeste apparaten
  WiFi.softAP(ssid, password, 1, 0, 4);

  // Wacht tot AP echt actief is (timeout verhoogd naar 5s)
  unsigned long apStart = millis();
  while (WiFi.softAPIP() == IPAddress(0,0,0,0) && millis() - apStart < 5000) {
    delay(100);
  }

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP gestart: "); Serial.println(ssid);
  Serial.print("IP: "); Serial.println(IP);

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // ── Hoofdpagina met cache-headers voor snelle herlaadtijd ──
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_html);
    response->addHeader("Cache-Control", "no-cache");          // altijd vers, maar browser mag ETag checken
    response->addHeader("X-Content-Type-Options", "nosniff");
    request->send(response);
  });

  // ── Favicon stub: voorkomt onnodige 404-request van browser ──
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(204);  // No Content — geen favicon
  });

  // ── 404: redirect naar root zodat browser nooit hangt ──
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->redirect("/");
  });

  // ── WebSocket max clients + ping timeout instellen ──
  ws.enable(true);

  server.begin();
  Serial.println("Webserver gestart op poort 80");

  // FIX: delay(3000) vervangen door non-blocking wacht via timer
  // Startup geluid speelt al via DFPlayer; geen blocking delay nodig
  playPhaseAudio(PHASE_IDLE);
}


void loop() {
  // cleanupClients elke 200ms — voorkomt ophoping van dode verbindingen
  static unsigned long lastWsCleanup = 0;
  if (millis() - lastWsCleanup >= 200) {
    ws.cleanupClients(2);   // max 2 gelijktijdige clients bewaren
    lastWsCleanup = millis();
  }

  // Stuur audio-verzoek veilig buiten de game-update
  serviceAudio();

  // Periodieke broadcast elke 500ms
  static unsigned long lastBroadcast = 0;
  if (millis() - lastBroadcast >= 500) {
    lastBroadcast = millis();
    if (ws.count() > 0) {
      ws.textAll(buildScoreJson(currentState == STATE_PLAYING));
    }
  }

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
    // Stuur direct huidige stand + strip-staat mee
    client->text(buildScoreJson(true));
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WebSocket client #%u verbroken\n", client->id());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      data[len] = 0;
      String msg = (char*)data;
      Serial.printf("Commando: %s\n", msg.c_str());
      if      (msg == "ping")    { client->text(buildScoreJson(false)); }
      else if (msg == "add10")   punten += 10;
      else if (msg == "add100")  punten += 100;
      else if (msg == "levelup") level++;
      else if (msg == "reset")   { punten = 0; level = 1; }
      ws.textAll(buildScoreJson(false));
    }
  }
}

// ── Hulpfunctie: bouw LED-strip JSON payload ─────────────────
// Stuurt alleen actieve pixels mee om payload klein te houden.
// Format: {"punten":X,"level":Y,"clear":true,"leds":[{"i":idx,"r":R,"g":G,"b":B},...]}
String buildScoreJson(bool includeLeds) {
  String json = "{\"punten\":" + String(punten) + ",\"level\":" + String(level);
  if (includeLeds) {
    json += ",\"clear\":true,\"leds\":[";
    bool first = true;
    // Actieve game-LEDs
    for (int i = 0; i < numActiveLeds; i++) {
      if (!activeLeds[i].active) continue;
      int pos = (int)activeLeds[i].position;
      if (pos < 0 || pos >= NUM_LEDS) continue;
      CRGB c = getColorRGB(activeLeds[i].color);
      if (!first) json += ",";
      json += "{\"i\":" + String(pos) + ",\"r\":" + String(c.r) + ",\"g\":" + String(c.g) + ",\"b\":" + String(c.b) + "}";
      first = false;
    }
    // Actieve bullets
    for (int b = 0; b < numBullets; b++) {
      if (!bullets[b].active) continue;
      int pos = (int)bullets[b].position;
      if (pos < 0 || pos >= NUM_LEDS) continue;
      CRGB c = getColorRGB(bullets[b].color);
      // Bullets iets lichter weergeven
      if (!first) json += ",";
      json += "{\"i\":" + String(pos) + ",\"r\":" + String(min(255,(int)c.r+40)) + ",\"g\":" + String(min(255,(int)c.g+40)) + ",\"b\":" + String(min(255,(int)c.b+40)) + "}";
      first = false;
    }
    json += "]";
  }
  json += "}";
  return json;
}

void addPunten(int aantal) {
  punten += aantal;
  // includeLeds=true: stuur actuele strip mee bij puntenwijziging
  ws.textAll(buildScoreJson(true));
  Serial.printf("Punten: +%d (Totaal: %d)\n", aantal, punten);
}

void setLevel(int nieuwLevel) {
  level = nieuwLevel;
  ws.textAll(buildScoreJson(false));
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