#include <FastLED.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// de opmerking is weg


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

// Buzzer pin
#define BUZZER_PIN 1

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
  STATE_PLAYING
};

GameState currentState = STATE_ATTRACT;

// Knight Rider variabelen
int knightRiderPos = 0;
bool knightRiderDirection = true;
unsigned long lastKnightRiderUpdate = 0;
#define KNIGHT_RIDER_SPEED 20

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
int ledsMissed =0;
bool win=false;

// Knop LED feedback timing
unsigned long buttonLedOffTime = 0;
int activeButtonLed = -1;

// Sound state machine voor non-blocking audio
enum SoundState {
  SOUND_IDLE,
  SOUND_WIN_1,
  SOUND_WIN_2,
  SOUND_WIN_3,
  SOUND_LEVELUP_1,
  SOUND_LEVELUP_2,
  SOUND_LEVELUP_3,
  SOUND_LEVELUP_4,
  SOUND_SAD_1,
  SOUND_SAD_2,
  SOUND_SAD_3
};

SoundState currentSound = SOUND_IDLE;
unsigned long soundTimer = 0;

// Interrupt handlers
void btnRedISR() {
  unsigned long currentTime = millis();
  if (currentTime - lastInterruptTime > lastInterruptMs) {  // Kortere debounce voor snellere detectie
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

// HTML pagina
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Ledstrip Game</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
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
        h1 {
            font-size: 2.5em;
            margin-bottom: 40px;
            text-shadow: 2px 2px 4px rgba(0, 0, 0, 0.3);
        }
        .score-container {
            margin: 30px 0;
        }
        .punten {
            font-size: 6em;
            font-weight: bold;
            color: #ffd700;
            text-shadow: 3px 3px 6px rgba(0, 0, 0, 0.5);
            margin: 20px 0;
            transition: transform 0.3s ease;
        }
        .punten.update {
            transform: scale(1.1);
        }
        .level {
            font-size: 2em;
            color: #87ceeb;
            margin: 15px 0;
            transition: transform 0.3s ease;
        }
        .level.update {
            transform: scale(1.1);
        }
        .label {
            font-size: 1.2em;
            opacity: 0.9;
            margin-bottom: 5px;
        }
        .status {
            margin-top: 30px;
            padding: 10px;
            border-radius: 10px;
            font-size: 0.9em;
        }
        .status.connected {
            background: rgba(0, 255, 0, 0.2);
            color: #90EE90;
        }
        .status.disconnected {
            background: rgba(255, 0, 0, 0.2);
            color: #FFB6C1;
        }
        .controls {
            margin-top: 30px;
            display: flex;
            gap: 10px;
            justify-content: center;
            flex-wrap: wrap;
        }
        button {
            padding: 12px 24px;
            font-size: 1em;
            border: none;
            border-radius: 10px;
            cursor: pointer;
            transition: all 0.3s ease;
            font-weight: bold;
        }
        button:hover {
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
        }
        .btn-add {
            background: #4CAF50;
            color: white;
        }
        .btn-reset {
            background: #f44336;
            color: white;
        }
        .btn-level {
            background: #2196F3;
            color: white;
        }
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
            websocket = new WebSocket('ws://' + window.location.hostname + '/ws');
            
            websocket.onopen = function(event) {
                console.log('WebSocket verbonden');
                document.getElementById('status').className = 'status connected';
                document.getElementById('status').textContent = '✓ Verbonden';
                clearInterval(reconnectInterval);
            };
            
            websocket.onclose = function(event) {
                console.log('WebSocket verbroken');
                document.getElementById('status').className = 'status disconnected';
                document.getElementById('status').textContent = '✗ Verbinding verbroken - Opnieuw verbinden...';
                reconnectInterval = setInterval(initWebSocket, 2000);
            };
            
            websocket.onmessage = function(event) {
                try {
                    const data = JSON.parse(event.data);
                    
                    if (data.punten !== undefined) {
                        const puntenEl = document.getElementById('punten');
                        puntenEl.textContent = data.punten;
                        puntenEl.classList.add('update');
                        setTimeout(() => puntenEl.classList.remove('update'), 300);
                    }
                    
                    if (data.level !== undefined) {
                        const levelEl = document.getElementById('level');
                        levelEl.textContent = data.level;
                        levelEl.classList.add('update');
                        setTimeout(() => levelEl.classList.remove('update'), 300);
                    }
                } catch (e) {
                    console.error('Fout bij verwerken bericht:', e);
                }
            };
        }
        
        function sendCommand(cmd) {
            if (websocket && websocket.readyState === WebSocket.OPEN) {
                websocket.send(cmd);
            }
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
void displayLeds();
void startWinSound();
void startLevelUpSound();
void startSadSound();
void updateSound();
void updateKnightRider();
void startGame();
void stopGame();
void buttonKnight();
void setLevel(int nieuwLevel);
void addPunten(int aantal);
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);


/////////////////////////
// Fastled patterns
// Forward declarations for pattern functions
/////////////////////////
void rainbow();
void rainbowWithGlitter();
void confetti();
void sinelon();
void juggle();
void bpm();
void nextPattern();
void addGlitter(fract8 chanceOfGlitter);

// List of patterns to cycle through.  Each is defined as a separate function below.
typedef void (*SimplePatternList[])();
SimplePatternList gPatterns = { rainbow, rainbowWithGlitter, confetti, sinelon, juggle, bpm };

uint8_t gCurrentPatternNumber = 0; // Index number of which pattern is current
uint8_t gHue = 0; // rotating "base color" used by many of the patterns
#define ARRAY_SIZE(A) (sizeof(A) / sizeof((A)[0]))
#define FRAMES_PER_SECOND  120
// fastled things end


void setup() {
  Serial.begin(115200);
  
  // LED strip initialiseren
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
  FastLED.show();
  
  // Druktoetsen configureren met interrupts
  pinMode(BTN_RED, INPUT_PULLUP);
  pinMode(BTN_BLUE, INPUT_PULLUP);
  pinMode(BTN_YELLOW, INPUT_PULLUP);
  pinMode(BTN_GREEN, INPUT_PULLUP);
  
  attachInterrupt(digitalPinToInterrupt(BTN_RED), btnRedISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(BTN_BLUE), btnBlueISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(BTN_YELLOW), btnYellowISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(BTN_GREEN), btnGreenISR, FALLING);
  
  // Knop LEDs configureren
  pinMode(LED_BTN_RED, OUTPUT);
  pinMode(LED_BTN_BLUE, OUTPUT);
  pinMode(LED_BTN_YELLOW, OUTPUT);
  pinMode(LED_BTN_GREEN, OUTPUT);
  
  ButtonLedsOff();
  
  // Buzzer configureren
  pinMode(BUZZER_PIN, OUTPUT);
  
  // Random seed
  randomSeed(analogRead(0));
  
  Serial.println("Knight Rider Mode - Druk een toets om te starten");

// Configureer Access Point
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  
  Serial.print("Access Point gestart: ");
  Serial.println(ssid);
  Serial.print("IP adres: ");
  Serial.println(IP);
  
  // WebSocket setup
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  
  // Serveer hoofdpagina
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });
  
  // Start server
  server.begin();
  Serial.println("Webserver gestart op http://" + IP.toString());

}

void loop() {

  // Cleanup WebSocket clients
  ws.cleanupClients();


  // Call the current pattern function once, updating the 'leds' array
  gPatterns[gCurrentPatternNumber]();

  // Non-blocking sound updates
  updateSound();
  
  if (currentState == STATE_ATTRACT) {
    //updateKnightRider();
    buttonKnight();
    // fastled show ///
    // Call the current pattern function once, updating the 'leds' array
    gPatterns[gCurrentPatternNumber]();

    // send the 'leds' array out to the actual LED strip
    FastLED.show();  
    // insert a delay to keep the framerate modest
    FastLED.delay(1000/FRAMES_PER_SECOND); 

    // do some periodic updates
    EVERY_N_MILLISECONDS( 20 ) { gHue++; } // slowly cycle the "base color" through the rainbow
    EVERY_N_SECONDS( 10 ) { nextPattern(); } // change patterns periodicall
    
    //fastled show end //////

    // Check voor button presses om te starten
    if (btnRedPressed || btnBluePressed || btnYellowPressed || btnGreenPressed) {
      btnRedPressed = false;
      btnBluePressed = false;
      btnYellowPressed = false;
      btnGreenPressed = false;
      startGame();
    }
  } 
  else if (currentState == STATE_PLAYING) {
    // LED beweging op vaste framerate
    if (millis() - lastMoveTime >= FRAME_DELAY) {
      lastMoveTime = millis();
      moveLeds();
      displayLeds();
    }
    
    // Handle button presses
    handleButtonPress();
    
    // Update knop LED status
    updateButtonLeds();
  }
}

void updateSound() {
  if (currentSound == SOUND_IDLE) return;
  
  unsigned long currentTime = millis();
  
  switch(currentSound) {
    case SOUND_WIN_1:
      if (currentTime >= soundTimer) {
        tone(BUZZER_PIN, 523, 100);
        soundTimer = currentTime + 120;
        currentSound = SOUND_WIN_2;
      }
      break;
    case SOUND_WIN_2:
      if (currentTime >= soundTimer) {
        tone(BUZZER_PIN, 659, 100);
        soundTimer = currentTime + 120;
        currentSound = SOUND_WIN_3;
      }
      break;
    case SOUND_WIN_3:
      if (currentTime >= soundTimer) {
        tone(BUZZER_PIN, 784, 150);
        soundTimer = currentTime + 200;
        currentSound = SOUND_IDLE;
      }
      break;
      
    case SOUND_LEVELUP_1:
      if (currentTime >= soundTimer) {
        tone(BUZZER_PIN, 523, 80);
        soundTimer = currentTime + 90;
        currentSound = SOUND_LEVELUP_2;
      }
      break;
    case SOUND_LEVELUP_2:
      if (currentTime >= soundTimer) {
        tone(BUZZER_PIN, 659, 80);
        soundTimer = currentTime + 90;
        currentSound = SOUND_LEVELUP_3;
      }
      break;
    case SOUND_LEVELUP_3:
      if (currentTime >= soundTimer) {
        tone(BUZZER_PIN, 784, 80);
        soundTimer = currentTime + 90;
        currentSound = SOUND_LEVELUP_4;
      }
      break;
    case SOUND_LEVELUP_4:
      if (currentTime >= soundTimer) {
        tone(BUZZER_PIN, 1047, 200);
        soundTimer = currentTime + 250;
        currentSound = SOUND_IDLE;
      }
      break;
      
    case SOUND_SAD_1:
      if (currentTime >= soundTimer) {
        tone(BUZZER_PIN, 392, 200);
        soundTimer = currentTime + 220;
        currentSound = SOUND_SAD_2;
      }
      break;
    case SOUND_SAD_2:
      if (currentTime >= soundTimer) {
        tone(BUZZER_PIN, 349, 200);
        soundTimer = currentTime + 220;
        currentSound = SOUND_SAD_3;
      }
      break;
    case SOUND_SAD_3:
      if (currentTime >= soundTimer) {
        tone(BUZZER_PIN, 294, 300);
        soundTimer = currentTime + 350;
        currentSound = SOUND_IDLE;
      }
      break;
      
    default:
      currentSound = SOUND_IDLE;
      break;
  }
}

void startWinSound() {
  currentSound = SOUND_WIN_1;
  soundTimer = millis();
}

void startLevelUpSound() {
  currentSound = SOUND_LEVELUP_1;
  soundTimer = millis();
}

void startSadSound() {
  currentSound = SOUND_SAD_1;
  soundTimer = millis();
}

void updateKnightRider() {
  if (millis() - lastKnightRiderUpdate >= KNIGHT_RIDER_SPEED) {
    lastKnightRiderUpdate = millis();
    
    FastLED.clear();

    // Bepaal de kleur op basis van de richting 
    // Heen (Direction true) = Rood, Terug (Direction false) = Blauw (als voorbeeld)
    CRGB currentColor = knightRiderDirection ? CRGB::Red : CRGB::Blue;

    // Teken Knight Rider LED met trailing effect
    int tailLength = 8;
    for (int i = 0; i < tailLength; i++) {
      int pos = knightRiderPos - (knightRiderDirection ? i : -i); // Tail volgt de beweging 
      
      if (pos >= 0 && pos < NUM_LEDS) {
        int brightness = 255 - (i * 30);
        if (brightness < 0) brightness = 0;
        
        // Pas de kleur toe met de berekende helderheid
        leds[pos] = currentColor;
        leds[pos].nscale8_video(brightness); 
      }
    }
    
    FastLED.show();

    // Update positie en knop-feedback
    if (knightRiderDirection) {
      knightRiderPos++;
      
      // Logica voor knop-LEDs tijdens de heenweg
      switch (knightRiderPos) {
        case 80: activateButtonLed(LED_BTN_GREEN); break;
        case 60: activateButtonLed(LED_BTN_YELLOW); break;
        case 40: activateButtonLed(LED_BTN_RED); break;
        case 20: activateButtonLed(LED_BTN_BLUE); break;
      }
      
      if (knightRiderPos >= NUM_LEDS - 1) {
        knightRiderDirection = false;
        ButtonLedsOff();
      }
    } else {
      knightRiderPos--;
      
      // Logica voor knop-LEDs tijdens de terugweg [cite: 69, 70, 71, 72]
      switch (knightRiderPos) {
        case 80: activateButtonLed(LED_BTN_GREEN); break;
        case 60: activateButtonLed(LED_BTN_YELLOW); break;
        case 40: activateButtonLed(LED_BTN_RED); break;
        case 20: activateButtonLed(LED_BTN_BLUE); break;
      }
      
      if (knightRiderPos <= 0) {
        knightRiderDirection = true;
        ButtonLedsOff();
      }
    }
  }
}


void startGame() {
  Serial.println("Start Spel");
  currentState = STATE_PLAYING;
  
  // Reset game variabelen
  numActiveLeds = 0;
  ledsExtinguished = 0;
  currentSpeed = baseSpeed;
  level = 1;
  punten = 0;

  
  // Clear strip
  FastLED.clear();
  FastLED.show();
  ButtonLedsOff();
  
  // Start met eerste LED
  addNewLed();
  
  // Korte startgeluid
  tone(BUZZER_PIN, 1000, 100);
}

void stopGame() {
  Serial.println("Stop Spel - Terug naar Knight Rider");
  currentState = STATE_ATTRACT;
  
  // Reset Knight Rider
  knightRiderPos = 0;
  knightRiderDirection = true;
  
  // Clear game data
  numActiveLeds = 0;
  
  // Clear strip
  FastLED.clear();
  FastLED.show();
  
  // Stop geluid (non-blocking)
  tone(BUZZER_PIN, 800, 100);
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
    activeLeds[numActiveLeds].color = (LedColor)random(0, 4);
    activeLeds[numActiveLeds].active = true;
    numActiveLeds++;
  }
}

void moveLeds() {
  for (int i = 0; i < numActiveLeds; i++) {
    if (activeLeds[i].active) {
      activeLeds[i].position -= currentSpeed;
      
      if (activeLeds[i].position < 0) {
        activeLeds[i].active = false;
        startSadSound();
        compactLedArray();
        addNewLed();
        ledsMissed++;
        if (ledsMissed>3){
          ledsMissed=0;
          stopGame();
        }
      }
    }
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
  //Serial.println("Button Pressed");
  // Check voor stop combinatie door direct hardware pins te lezen
  if (digitalRead(BTN_GREEN) == LOW && digitalRead(BTN_BLUE) == LOW) {
    // Clear alle flags
    delay(500); // Wacht tot knoppen losgelaten zijn
    btnRedPressed = false;
    btnBluePressed = false;
    btnYellowPressed = false;
    btnGreenPressed = false;
    stopGame(); 
    return;
  }
  
  LedColor pressedColor;
  bool buttonPressed = false;
  int buttonLedPin;
  
  if (btnRedPressed) {
    pressedColor = COLOR_RED;
    buttonLedPin = LED_BTN_RED;
    buttonPressed = true;
    btnRedPressed = false;
  } else if (btnBluePressed) {
    pressedColor = COLOR_BLUE;
    buttonLedPin = LED_BTN_BLUE;
    buttonPressed = true;
    btnBluePressed = false;
  } else if (btnYellowPressed) {
    pressedColor = COLOR_YELLOW;
    buttonLedPin = LED_BTN_YELLOW;
    buttonPressed = true;
    btnYellowPressed = false;
  } else if (btnGreenPressed) {
    pressedColor = COLOR_GREEN;
    buttonLedPin = LED_BTN_GREEN;
    buttonPressed = true;
    btnGreenPressed = false;
  }
  
  if (buttonPressed) {
    activateButtonLed(buttonLedPin);
    
    int closestLed = -1;
    float closestPosition = NUM_LEDS;
    
    for (int i = 0; i < numActiveLeds; i++) {
      if (activeLeds[i].active && activeLeds[i].position < closestPosition) {
        closestPosition = activeLeds[i].position;
        closestLed = i;
      }
    }
    if (closestLed >= 0 && activeLeds[closestLed].color == pressedColor) {
      activeLeds[closestLed].active = false;
      startWinSound();
      addPunten(10);
      win = true;
      ledsExtinguished++;
      ledsMissed=0;  // reset missed counter
      
      bool levelUp = (ledsExtinguished % 10 == 0);  // 5
      
      compactLedArray();
      
      if (ledsExtinguished >= 10 && levelUp) { // 5
        startLevelUpSound();
        setLevel(level + 1);
        int numToAdd = min(2, MAX_LEDS - numActiveLeds);
        for (int i = 0; i < numToAdd; i++) {
          addNewLed();
        }
      } else {
        addNewLed();
      }
    } 
    if (win) {
        //Serial.println("wrong Button");
      win = false;
    }
    else {
      // Korte boos geluid
      tone(BUZZER_PIN, 300, 300);
    } 
  }
}

void compactLedArray() {
  int writeIndex = 0;
  for (int readIndex = 0; readIndex < numActiveLeds; readIndex++) {
    if (activeLeds[readIndex].active) {
      if (writeIndex != readIndex) {
        activeLeds[writeIndex] = activeLeds[readIndex];
      }
      writeIndex++;
    }
  }
  numActiveLeds = writeIndex;
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
  
  FastLED.show();
}


void buttonKnight(){
 // Update positie en knop-feedback
    if (knightRiderDirection) {
      knightRiderPos++;
      
      // Logica voor knop-LEDs tijdens de heenweg
      switch (knightRiderPos) {
        case 80: activateButtonLed(LED_BTN_GREEN); break;
        case 60: activateButtonLed(LED_BTN_YELLOW); break;
        case 40: activateButtonLed(LED_BTN_RED); break;
        case 20: activateButtonLed(LED_BTN_BLUE); break;
      }
      
      if (knightRiderPos >= NUM_LEDS - 1) {
        knightRiderDirection = false;
        ButtonLedsOff();
      }
    } else {
      knightRiderPos--;
      
      // Logica voor knop-LEDs tijdens de terugweg [cite: 69, 70, 71, 72]
      switch (knightRiderPos) {
        case 80: activateButtonLed(LED_BTN_GREEN); break;
        case 60: activateButtonLed(LED_BTN_YELLOW); break;
        case 40: activateButtonLed(LED_BTN_RED); break;
        case 20: activateButtonLed(LED_BTN_BLUE); break;
      }
      
      if (knightRiderPos <= 0) {
        knightRiderDirection = true;
        ButtonLedsOff();
      }
    }
  }





///////////////////////////
// fastled show patterns //
///////////////////////////

void nextPattern()
{
  // add one to the current pattern number, and wrap around at the end
  gCurrentPatternNumber = (gCurrentPatternNumber + 1) % ARRAY_SIZE( gPatterns);
}

void rainbow() 
{
  // FastLED's built-in rainbow generator
  fill_rainbow( leds, NUM_LEDS, gHue, 7);
}

void rainbowWithGlitter() 
{
  // built-in FastLED rainbow, plus some random sparkly glitter
  rainbow();
  addGlitter(80);
}

void addGlitter( fract8 chanceOfGlitter) 
{
  if( random8() < chanceOfGlitter) {
    leds[ random16(NUM_LEDS) ] += CRGB::White;
  }
}

void confetti() 
{
  // random colored speckles that blink in and fade smoothly
  fadeToBlackBy( leds, NUM_LEDS, 10);
  int pos = random16(NUM_LEDS);
  leds[pos] += CHSV( gHue + random8(64), 200, 255);
}

void sinelon()
{
  // a colored dot sweeping back and forth, with fading trails
  fadeToBlackBy( leds, NUM_LEDS, 20);
  int pos = beatsin16( 13, 0, NUM_LEDS-1 );
  leds[pos] += CHSV( gHue, 255, 192);
}

void bpm()
{
  // colored stripes pulsing at a defined Beats-Per-Minute (BPM)
  uint8_t BeatsPerMinute = 62;
  CRGBPalette16 palette = PartyColors_p;
  uint8_t beat = beatsin8( BeatsPerMinute, 64, 255);
  for( int i = 0; i < NUM_LEDS; i++) { //9948
    leds[i] = ColorFromPalette(palette, gHue+(i*2), beat-gHue+(i*10));
  }
}


void juggle() {
  // eight colored dots, weaving in and out of sync with each other
  fadeToBlackBy( leds, NUM_LEDS, 20);
  uint8_t dothue = 0;
  for( int i = 0; i < 8; i++) {
    leds[beatsin16( i+7, 0, NUM_LEDS-1 )] |= CHSV(dothue, 200, 255);
    dothue += 32;
  }
}

//////////////////////////
// fastled patterns end //
//////////////////////////



// WebSocket event handler
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WebSocket client #%u verbonden\n", client->id());
    
    // Stuur huidige status naar nieuwe client
    String json = "{\"punten\":" + String(punten) + ",\"level\":" + String(level) + "}";
    client->text(json);
    
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WebSocket client #%u verbroken\n", client->id());
    
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      data[len] = 0;
      String msg = (char*)data;
      
      Serial.printf("Ontvangen commando: %s\n", msg.c_str());
      
      // Verwerk commando's
      if (msg == "add10") {
        punten += 10;
      } else if (msg == "add100") {
        punten += 100;
      } else if (msg == "levelup") {
        level++;
      } else if (msg == "reset") {
        punten = 0;
        level = 1;
      }
      
      // Broadcast update naar alle clients
      String json = "{\"punten\":" + String(punten) + ",\"level\":" + String(level) + "}";
      ws.textAll(json);
    }
  }
}

// Functie om punten toe te voegen (gebruik dit in je game logica)
void addPunten(int aantal) {
  punten += aantal;
  String json = "{\"punten\":" + String(punten) + ",\"level\":" + String(level) + "}";
  ws.textAll(json);
  Serial.printf("Punten toegevoegd: %d (Totaal: %d)\n", aantal, punten);
}

// Functie om level te verhogen (gebruik dit in je game logica)
void setLevel(int nieuwLevel) {
  level = nieuwLevel;
  String json = "{\"punten\":" + String(punten) + ",\"level\":" + String(level) + "}";
  ws.textAll(json);
  Serial.printf("Level gewijzigd naar: %d\n", level);
}
