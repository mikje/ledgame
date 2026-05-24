#include <FastLED.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// ─── Instellingen ─────────────────────────────────────────────────────────────
static const char*   DEVICE_MAC  = "7c:f9:61:b7:cc:ff";
static const int     W           = 64;
static const int     H           = 16;
static const uint8_t BRIGHTNESS  = 70;

// ─── BLE UUIDs ────────────────────────────────────────────────────────────────
static BLEUUID SERVICE_UUID((uint16_t)0x00FA);
static BLEUUID WRITE_UUID  ((uint16_t)0xFA02);
static BLEUUID NOTIFY_UUID ((uint16_t)0xFA03);

// ─── Handshake bytes ──────────────────────────────────────────────────────────
static const uint8_t HANDSHAKE_1[] = {0x08,0x00,0x01,0x80,0x0E,0x06,0x32,0x00};
static const uint8_t HANDSHAKE_2[] = {0x04,0x00,0x05,0x80};

// ─── Colors ───────────────────────────────────────────────────────────────────
struct Color { uint8_t r, g, b; };
static const Color COL_BLACK  = {0,   0,   0  };
static const Color COL_CYAN   = {0,   200, 255};
static const Color COL_GREEN  = {0,   220, 80 };
static const Color COL_YELLOW = {255, 200, 0  };
static const Color COL_GRAY   = {80,  80,  80 };
static const Color COL_RED    = {220, 0,   0  };

// ─── Globale staat ────────────────────────────────────────────────────────────
BLEClient*                bleClient  = nullptr;
BLERemoteCharacteristic*  writeChr   = nullptr;
BLERemoteCharacteristic*  notifyChr  = nullptr;

volatile bool    connected        = false;
volatile bool    handshakeDone    = false;
volatile uint8_t hsStage          = 0;
volatile bool    frameAck         = true;

// Acties die vanuit loop() uitgevoerd worden (niet vanuit BLE callback)
volatile bool    doSendHS2        = false;
volatile bool    doSendBrightness = false;

float temp_ = NAN;
float ph_   = NAN;
float orp_  = NAN;

// ─── CRC32 ────────────────────────────────────────────────────────────────────
static uint32_t crc32_byte(uint32_t crc, uint8_t b) {
  crc ^= b;
  for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
  return crc;
}
static uint32_t crc32(const uint8_t* d, size_t n, uint32_t init = 0xFFFFFFFF) {
  for (size_t i = 0; i < n; i++) init = crc32_byte(init, d[i]);
  return init;
}

// ─── Helper: push integers ────────────────────────────────────────────────────
static void push_u16le(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(x & 0xFF); v.push_back(x >> 8);
}
static void push_u32be(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x>>24); v.push_back(x>>16); v.push_back(x>>8); v.push_back(x);
}
static void push_u32le(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x); v.push_back(x>>8); v.push_back(x>>16); v.push_back(x>>24);
}

// ─── PNG chunk ────────────────────────────────────────────────────────────────
static void png_chunk(std::vector<uint8_t>& out, const char* tag,
                      const uint8_t* data, size_t len) {
  push_u32be(out, (uint32_t)len);
  uint8_t t[4]={(uint8_t)tag[0],(uint8_t)tag[1],(uint8_t)tag[2],(uint8_t)tag[3]};
  uint32_t c = crc32(t, 4);
  if (len > 0) c = crc32(data, len, c);
  c ^= 0xFFFFFFFF;
  out.insert(out.end(), t, t+4);
  if (len > 0) out.insert(out.end(), data, data+len);
  push_u32be(out, c);
}

// ─── DEFLATE ──────────────────────────────────────────────────────────────────
struct BitBuf {
  std::vector<uint8_t> out;
  uint32_t bits{0}; int nbits{0};
  void put(uint32_t val, int n) {
    bits |= (val&((1u<<n)-1))<<nbits; nbits+=n;
    while(nbits>=8){out.push_back(bits&0xFF);bits>>=8;nbits-=8;}
  }
  void flush(){if(nbits>0){out.push_back(bits&0xFF);bits=0;nbits=0;}}
};
static uint32_t rev(uint32_t v,int n){uint32_t r=0;for(int i=0;i<n;i++){r=(r<<1)|(v&1);v>>=1;}return r;}
static void emit_lit(BitBuf& bb,uint8_t b){if(b<=143)bb.put(rev(0x30+b,8),8);else bb.put(rev(0x190+(b-144),9),9);}
static void emit_eob(BitBuf& bb){bb.put(rev(0,7),7);}

static void emit_ref(BitBuf& bb,int len,int dist){
  struct LC{int sym,eb,base;};
  LC lc;
  if(len<=10)lc={257+len-3,0,len};
  else if(len<=12)lc={265,1,11};else if(len<=14)lc={266,1,13};
  else if(len<=16)lc={267,1,15};else if(len<=18)lc={268,1,17};
  else if(len<=22)lc={269,2,19};else if(len<=26)lc={270,2,23};
  else if(len<=30)lc={271,2,27};else if(len<=34)lc={272,2,31};
  else if(len<=42)lc={273,3,35};else if(len<=50)lc={274,3,43};
  else if(len<=58)lc={275,3,51};else if(len<=66)lc={276,3,59};
  else if(len<=82)lc={277,4,67};else if(len<=98)lc={278,4,83};
  else if(len<=114)lc={279,4,99};else if(len<=130)lc={280,4,115};
  else if(len<=162)lc={281,5,131};else if(len<=194)lc={282,5,163};
  else if(len<=226)lc={283,5,195};else if(len<=257)lc={284,5,227};
  else lc={285,0,258};
  if(lc.sym<=279)bb.put(rev(lc.sym-256,7),7);else bb.put(rev(0xC0+(lc.sym-280),8),8);
  if(lc.eb>0)bb.put(len-lc.base,lc.eb);
  int dc,de,db;
  if(dist==1){dc=0;de=0;db=1;}else if(dist==2){dc=1;de=0;db=2;}
  else if(dist==3){dc=2;de=0;db=3;}else if(dist==4){dc=3;de=0;db=4;}
  else if(dist<=6){dc=4;de=1;db=5;}else if(dist<=8){dc=5;de=1;db=7;}
  else if(dist<=12){dc=6;de=2;db=9;}else if(dist<=16){dc=7;de=2;db=13;}
  else if(dist<=24){dc=8;de=3;db=17;}else if(dist<=32){dc=9;de=3;db=25;}
  else if(dist<=48){dc=10;de=4;db=33;}else if(dist<=64){dc=11;de=4;db=49;}
  else if(dist<=96){dc=12;de=5;db=65;}else if(dist<=128){dc=13;de=5;db=97;}
  else if(dist<=192){dc=14;de=6;db=129;}else if(dist<=256){dc=15;de=6;db=193;}
  else if(dist<=384){dc=16;de=7;db=257;}else if(dist<=512){dc=17;de=7;db=385;}
  else if(dist<=768){dc=18;de=8;db=513;}else{dc=19;de=8;db=769;}
  bb.put(rev(dc,5),5);if(de>0)bb.put(dist-db,de);
}

static std::vector<uint8_t> deflate_compress(const std::vector<uint8_t>& raw){
  const int HSIZE=4096,MAX_DIST=4096,MAX_LEN=128;
  std::vector<int> head(HSIZE,-1),prev(raw.size(),-1);
  auto hash3=[&](size_t i)->int{return((raw[i]*31337+raw[i+1]*1337+raw[i+2])&(HSIZE-1));};
  BitBuf bb; bb.put(1,1);bb.put(1,2);
  size_t i=0,n=raw.size();
  while(i<n){
    if(i+3<=n){
      int h=hash3(i),best_len=2,best_dist=0,j=head[h],steps=0;
      while(j>=0&&(int)i-j<=MAX_DIST&&steps<32){
        int ml=0;
        while(ml<MAX_LEN&&i+ml<n&&raw[i+ml]==raw[j+ml])ml++;
        if(ml>best_len){best_len=ml;best_dist=(int)i-j;}
        j=prev[j];steps++;
      }
      prev[i]=head[h];head[h]=(int)i;
      if(best_len>=3&&best_dist>0){
        emit_ref(bb,best_len,best_dist);
        for(int k=1;k<best_len;k++)
          if(i+k+3<=n){int hk=hash3(i+k);prev[i+k]=head[hk];head[hk]=(int)(i+k);}
        i+=best_len;continue;
      }
    }
    emit_lit(bb,raw[i]);i++;
  }
  emit_eob(bb);bb.flush();return bb.out;
}

// ─── PNG generatie ────────────────────────────────────────────────────────────
static std::vector<uint8_t> make_png(const Color* px,int w,int h){
  std::vector<uint8_t> raw;raw.reserve((size_t)h*(1+w*3));
  for(int y=0;y<h;y++){raw.push_back(0);for(int x=0;x<w;x++){raw.push_back(px[y*w+x].r);raw.push_back(px[y*w+x].g);raw.push_back(px[y*w+x].b);}}
  auto deflated=deflate_compress(raw);
  uint32_t s1=1,s2=0;for(uint8_t b:raw){s1=(s1+b)%65521;s2=(s2+s1)%65521;}
  std::vector<uint8_t> zs;zs.push_back(0x78);zs.push_back(0x9C);
  zs.insert(zs.end(),deflated.begin(),deflated.end());push_u32be(zs,(s2<<16)|s1);
  std::vector<uint8_t> png;
  const uint8_t sig[]={0x89,'P','N','G','\r','\n',0x1a,'\n'};png.insert(png.end(),sig,sig+8);
  uint8_t ihdr[13]={(uint8_t)(w>>24),(uint8_t)(w>>16),(uint8_t)(w>>8),(uint8_t)w,
                    (uint8_t)(h>>24),(uint8_t)(h>>16),(uint8_t)(h>>8),(uint8_t)h,8,2,0,0,0};
  png_chunk(png,"IHDR",ihdr,13);png_chunk(png,"IDAT",zs.data(),zs.size());png_chunk(png,"IEND",nullptr,0);
  return png;
}

// ─── Frame wrapper ────────────────────────────────────────────────────────────
static std::vector<uint8_t> build_frame(const std::vector<uint8_t>& png){
  uint16_t dlen=(uint16_t)png.size(),tlen=dlen+15;
  uint32_t c=crc32(png.data(),png.size())^0xFFFFFFFF;
  std::vector<uint8_t> f;push_u16le(f,tlen);
  f.push_back(0x02);f.push_back(0x00);f.push_back(0x00);
  push_u16le(f,dlen);f.push_back(0x00);f.push_back(0x00);
  push_u32le(f,c);f.push_back(0x00);f.push_back(0x65);
  f.insert(f.end(),png.begin(),png.end());return f;
}

// ─── Framebuffer + font ───────────────────────────────────────────────────────
Color fb[H][W];

static const uint8_t FONT_DIGITS[10][5]={
  {0b111,0b101,0b101,0b101,0b111},{0b010,0b110,0b010,0b010,0b111},
  {0b111,0b001,0b111,0b100,0b111},{0b111,0b001,0b111,0b001,0b111},
  {0b101,0b101,0b111,0b001,0b001},{0b111,0b100,0b111,0b001,0b111},
  {0b111,0b100,0b111,0b101,0b111},{0b111,0b001,0b001,0b001,0b001},
  {0b111,0b101,0b111,0b101,0b111},{0b111,0b101,0b111,0b001,0b111},
};
static const uint8_t FONT_COLON[5] ={0b000,0b010,0b000,0b010,0b000};
static const uint8_t FONT_DOT[5]   ={0b000,0b000,0b000,0b000,0b010};
static const uint8_t FONT_MINUS[5] ={0b000,0b000,0b111,0b000,0b000};
static const uint8_t FONT_DEGREE[5]={0b110,0b110,0b000,0b000,0b000};
static const uint8_t FONT_LETTERS[8][5]={
  {0b111,0b010,0b010,0b010,0b010},{0b111,0b100,0b111,0b100,0b111},
  {0b101,0b111,0b101,0b101,0b101},{0b111,0b101,0b111,0b100,0b100},
  {0b101,0b101,0b111,0b101,0b101},{0b111,0b101,0b101,0b101,0b111},
  {0b111,0b101,0b111,0b110,0b101},{0b111,0b100,0b100,0b100,0b111},
};

void fb_clear(Color c=COL_BLACK){for(int y=0;y<H;y++)for(int x=0;x<W;x++)fb[y][x]=c;}
void fb_set(int x,int y,Color c){if(x>=0&&x<W&&y>=0&&y<H)fb[y][x]=c;}
void draw_glyph(int x,int y,const uint8_t g[5],Color col){for(int r=0;r<5;r++){uint8_t b=g[r];for(int c=0;c<3;c++)if(b&(0x4>>c))fb_set(x+c,y+r,col);}}
int draw_char(int x,int y,char c,Color col){
  if(c>='0'&&c<='9'){draw_glyph(x,y,FONT_DIGITS[c-'0'],col);return 4;}
  if(c==':'){draw_glyph(x,y,FONT_COLON,col);return 2;}
  if(c=='.'){draw_glyph(x,y,FONT_DOT,col);return 2;}
  if(c=='-'){draw_glyph(x,y,FONT_MINUS,col);return 4;}
  if(c=='~'){draw_glyph(x,y,FONT_DEGREE,col);return 4;}
  const char* letters="TEMPHОRC";
  for(int i=0;i<8;i++)if(c==letters[i]){draw_glyph(x,y,FONT_LETTERS[i],col);return 4;}
  return 4;
}
void draw_string(int x,int y,const char* s,Color col){while(*s){x+=draw_char(x,y,*s,col);s++;}}
void draw_value_right(int cx,int cw,int y,const char* s,Color col){
  int w=0;for(const char* p=s;*p;p++)w+=(*p==':'||*p=='.')?2:4;
  draw_string(cx+cw-w,y,s,col);
}

// ─── Frame bouwen ─────────────────────────────────────────────────────────────
std::vector<uint8_t> make_display_frame(){
  fb_clear(COL_BLACK);char buf[16];

  // Temperatuur
  Color tc=COL_CYAN;bool tv=!isnan(temp_);
  if(tv){if(temp_>35||temp_<10)tc=COL_RED;snprintf(buf,sizeof(buf),"%.1f",temp_);
    draw_char(4,1,buf[0],tc);draw_char(8,1,buf[1],tc);
    draw_char(11,1,'.',tc);draw_char(14,1,buf[3],tc);draw_char(18,1,'~',tc);
  }else{draw_char(6,1,'-',tc);draw_char(10,1,'-',tc);}
  draw_string(4,9,"TEMP",COL_GRAY);

  // pH
  Color pc=COL_GREEN;bool pv=!isnan(ph_);
  if(pv){if(ph_<7.0||ph_>7.6)pc=COL_RED;snprintf(buf,sizeof(buf),"%.1f",ph_);
    draw_char(28,1,buf[0],pc);draw_char(31,1,'.',pc);draw_char(34,1,buf[2],pc);
  }else{draw_char(28,1,'-',pc);draw_char(32,1,'-',pc);}
  draw_string(29,9,"PH",COL_GRAY);

  // ORP
  Color oc=COL_YELLOW;
  if(!isnan(orp_)){if(orp_<650||orp_>800)oc=COL_RED;snprintf(buf,sizeof(buf),"%d",(int)orp_);}
  else snprintf(buf,sizeof(buf),"---");
  draw_value_right(39,21,1,buf,oc);
  draw_string(48,9,"ORP",COL_GRAY);

  auto png=make_png(&fb[0][0],W,H);
  return build_frame(png);
}

// ─── BLE schrijven (alleen vanuit loop/main task aanroepen!) ──────────────────
void ble_write_cmd(const uint8_t* data,size_t len){
  if(!writeChr)return;
  writeChr->writeValue(const_cast<uint8_t*>(data),len,false);
  delay(50);
}
void ble_write_frame(const std::vector<uint8_t>& data){
  if(!writeChr)return;
  writeChr->writeValue(const_cast<uint8_t*>(data.data()),data.size(),true);
}

// ─── Notify callback — alleen vlaggen zetten, GEEN BLE writes ─────────────────
void notifyCallback(BLERemoteCharacteristic* chr,uint8_t* data,size_t len,bool isNotify){
  if(len<4)return;
  Serial.printf("Notify [%d]: %02X %02X %02X %02X  (stage=%d)\n",
                len,data[0],data[1],data[2],data[3],hsStage);

  if(data[2]==0x01&&data[3]==0x80&&hsStage==1){
    hsStage=2; doSendHS2=true;

  }else if(data[2]==0x05&&data[3]==0x80&&hsStage==2){
    hsStage=3; handshakeDone=true; doSendBrightness=true;
    Serial.println("Handshake complete!");

  }else if(len==5&&data[2]==0x02&&data[3]==0x00&&data[4]==0x03){
    Serial.println("Frame ACK");
    frameAck=true;
  }
}

// ─── BLE verbinding opbouwen ──────────────────────────────────────────────────
bool connectToDisplay(){
  Serial.printf("Verbinden met %s...\n",DEVICE_MAC);
  bleClient=BLEDevice::createClient();
  if(!bleClient->connect(BLEAddress(DEVICE_MAC))){
    Serial.println("Verbinding mislukt");return false;
  }
  Serial.println("BLE verbonden");

  BLERemoteService* svc=bleClient->getService(SERVICE_UUID);
  if(!svc){Serial.println("Service 0x00FA niet gevonden");bleClient->disconnect();return false;}

  writeChr =svc->getCharacteristic(WRITE_UUID);
  notifyChr=svc->getCharacteristic(NOTIFY_UUID);
  if(!writeChr){Serial.println("Write char niet gevonden");bleClient->disconnect();return false;}

  if(notifyChr&&notifyChr->canNotify()){
    notifyChr->registerForNotify(notifyCallback);
    Serial.println("Notify geregistreerd");
  }

  // Wacht even zodat eventuele welkomst-notify al binnenkomt vóór we starten
  delay(300);

  connected=true;
  hsStage=1;
  handshakeDone=false;
  doSendHS2=false;
  doSendBrightness=false;
  frameAck=true;

  ble_write_cmd(HANDSHAKE_1,8);
  Serial.println("Handshake stap 1 verstuurd");
  return true;
}


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


  // FIX: delay(3000) vervangen door non-blocking wacht via timer
  // Startup geluid speelt al via DFPlayer; geen blocking delay nodig
  playPhaseAudio(PHASE_IDLE);

Serial.println("Pool Display Direct — opstarten");
  BLEDevice::init("PoolController");

  if(!connectToDisplay()){
    Serial.println("Kan niet verbinden, herstart over 5s...");
    delay(5000);ESP.restart();
  }

  // Wacht op handshake via loop (max 8 seconden)
  uint32_t t=millis();
  while(!handshakeDone&&millis()-t<8000){
    // Verwerk vlaggen die de callback zette
    if(doSendHS2){
      doSendHS2=false;
      delay(50);
      Serial.println("Handshake stap 2 versturen...");
      ble_write_cmd(HANDSHAKE_2,4);
    }
    delay(50);
  }
  if(!handshakeDone){
    Serial.println("Handshake timeout, herstart...");
    delay(1000);ESP.restart();
  }

  // Helderheid instellen
  uint8_t bright[]={5,0,4,0x80,BRIGHTNESS};
  ble_write_cmd(bright,5);
  delay(100);
  Serial.println("Klaar voor gebruik");

}


void loop() {
  // FIX: cleanupClients() max 1x per seconde i.p.v. elke loop
  static unsigned long lastWsCleanup = 0;
  if (millis() - lastWsCleanup >= 1000) {
    ws.cleanupClients();
    lastWsCleanup = millis();
  }

  if(!bleClient||!bleClient->isConnected()){
    Serial.println("BLE verbroken, herstart...");
    delay(2000);ESP.restart();
  }

  // ── Sensorwaarden — vervang door je echte metingen ──────────────────
  temp_ = 26.5;
  ph_   = 7.2;
  orp_  = 710;
  // ─────────────────────────────────────────────────────────────────────

  if(frameAck){
    frameAck=false;
    auto frame=make_display_frame();
    Serial.printf("Frame sturen (%d bytes)\n",frame.size());
    ble_write_frame(frame);
  }

  delay(5000);


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