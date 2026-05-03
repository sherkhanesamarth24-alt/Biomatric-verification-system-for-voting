*
 * ============================================================
 *   IoT BIOMETRIC VOTING MACHINE — ESP32
 *   ADVANCED: WiFi + Live Web Dashboard v3.0
 * ============================================================
 *  Fingerprint Sensor : UART  → GPIO16 (RX2), GPIO17 (TX2)
 *  OLED SSD1306 (SPI) : CS→5, DC→2, RES→4, SCK→18, SDA→23
 *  Buttons            : GPIO32 (UP), GPIO33 (DOWN),
 *                       GPIO25 (SELECT), GPIO26 (BACK)
 *  Buzzer             : GPIO27
 * ============================================================
 */

#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <HardwareSerial.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// ── WiFi CREDENTIALS ── CHANGE THESE ─────────
#define WIFI_SSID     "000"
#define WIFI_PASSWORD "yashmali.01"

// ── PIN DEFINITIONS ──────────────────────────
#define FP_RX_PIN      16
#define FP_TX_PIN      17
#define OLED_CS        5
#define OLED_DC        2
#define OLED_RESET     4
#define OLED_MOSI      23
#define OLED_CLK       18
#define BTN_UP         32
#define BTN_DOWN       33
#define BTN_SELECT     25
#define BTN_BACK       26
#define BUZZER_PIN     27

// ── OLED ─────────────────────────────────────
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64

// ── EEPROM ───────────────────────────────────
#define EEPROM_SIZE        256
#define EEPROM_VOTE_BASE   0
#define EEPROM_VOTED_BASE  50

// ── ELECTION CONFIG ──────────────────────────
#define MAX_CANDIDATES   5
#define MAX_VOTERS       50
#define ADMIN_FP_ID      1

// ── CANDIDATE NAMES ──────────────────────────
// Short names for OLED (max ~12 chars)
const char* candidates[MAX_CANDIDATES] = {
  "V. Patil",
  "P. Patil",
  "V. More",
  "Y. Mali",
  "Y. Khopade"
};

// Full names for web dashboard
const char* candidatesFull[MAX_CANDIDATES] = {
  "Varsha Patil",
  "Priyanka Patil",
  "Vedant More",
  "Yash Mali",
  "Yash Khopade"
};

// ── GLOBALS ──────────────────────────────────
HardwareSerial fpSerial(2);
Adafruit_Fingerprint finger(&fpSerial);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT,
                          OLED_MOSI, OLED_CLK,
                          OLED_DC, OLED_RESET, OLED_CS);
WebServer server(80);

int  votes[MAX_CANDIDATES]    = {0};
bool hasVoted[MAX_VOTERS + 1] = {false};
int  selectedCandidate        = 0;
int  verifiedFingerID         = -1;
int  enrollID                 = 2;
int  adminMenuSel             = 0;
int  backPressCount           = 0;
unsigned long backPressTimer  = 0;
unsigned long stateTimer      = 0;
String wifiIP                 = "";
bool   wifiConnected          = false;
int    totalVotesCast         = 0;

unsigned long lastBtnTime[4]  = {0};
const unsigned long DEBOUNCE  = 200;
const int BTN_PINS[4]         = {BTN_UP, BTN_DOWN, BTN_SELECT, BTN_BACK};

enum State {
  STATE_IDLE,
  STATE_SCAN_FINGER,
  STATE_SELECT_CANDIDATE,
  STATE_CONFIRM_VOTE,
  STATE_VOTE_SUCCESS,
  STATE_ALREADY_VOTED,
  STATE_FP_NOT_FOUND,
  STATE_ADMIN_MENU,
  STATE_RESULTS
};
State currentState = STATE_IDLE;

// ── BUZZER ───────────────────────────────────
void beepShort()   { digitalWrite(BUZZER_PIN,HIGH);delay(80);digitalWrite(BUZZER_PIN,LOW); }
void beepSuccess() { for(int i=0;i<2;i++){digitalWrite(BUZZER_PIN,HIGH);delay(100);digitalWrite(BUZZER_PIN,LOW);delay(80);} }
void beepError()   { digitalWrite(BUZZER_PIN,HIGH);delay(500);digitalWrite(BUZZER_PIN,LOW); }
void beepLong()    { digitalWrite(BUZZER_PIN,HIGH);delay(800);digitalWrite(BUZZER_PIN,LOW); }

// ── EEPROM ───────────────────────────────────
void saveVotes() {
  for(int i=0;i<MAX_CANDIDATES;i++) EEPROM.put(EEPROM_VOTE_BASE+i*sizeof(int),votes[i]);
  EEPROM.commit();
}
void loadVotes() {
  for(int i=0;i<MAX_CANDIDATES;i++){
    EEPROM.get(EEPROM_VOTE_BASE+i*sizeof(int),votes[i]);
    if(votes[i]<0||votes[i]>9999) votes[i]=0;
  }
  totalVotesCast=0;
  for(int i=0;i<MAX_CANDIDATES;i++) totalVotesCast+=votes[i];
}
void saveVotedFlags() {
  for(int i=1;i<=MAX_VOTERS;i++) EEPROM.write(EEPROM_VOTED_BASE+i,hasVoted[i]?1:0);
  EEPROM.commit();
}
void loadVotedFlags() {
  for(int i=1;i<=MAX_VOTERS;i++) hasVoted[i]=(EEPROM.read(EEPROM_VOTED_BASE+i)==1);
}
void clearAllData() {
  for(int i=0;i<MAX_CANDIDATES;i++) votes[i]=0;
  for(int i=0;i<=MAX_VOTERS;i++)    hasVoted[i]=false;
  totalVotesCast=0;
  saveVotes(); saveVotedFlags();
  finger.emptyDatabase();
}

// ── WEB DASHBOARD HTML ────────────────────────
const char DASHBOARD_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Live Voting Dashboard</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Orbitron:wght@400;700;900&family=Share+Tech+Mono&display=swap');
  *{margin:0;padding:0;box-sizing:border-box;}
  body{
    background:#080808;
    color:#e0e0e0;
    font-family:'Share Tech Mono',monospace;
    min-height:100vh;
    padding:20px;
  }
  .header{
    text-align:center;
    padding:28px 0 18px;
    border-bottom:1px solid #1a1a2e;
    margin-bottom:28px;
  }
  .header h1{
    font-family:'Orbitron',sans-serif;
    font-size:1.5rem;
    font-weight:900;
    color:#e0e0ff;
    letter-spacing:4px;
  }
  .header .sub{
    color:#444466;
    font-size:0.75rem;
    margin-top:6px;
    letter-spacing:3px;
  }
  .stats-row{
    display:flex;
    gap:12px;
    margin-bottom:24px;
    flex-wrap:wrap;
  }
  .stat-card{
    flex:1;
    min-width:90px;
    background:#0d0d1a;
    border:1px solid #1a1a3a;
    border-radius:8px;
    padding:14px;
    text-align:center;
  }
  .stat-card .num{
    font-family:'Orbitron',sans-serif;
    font-size:1.7rem;
    font-weight:700;
    color:#7b7bff;
  }
  .stat-card .lbl{
    font-size:0.65rem;
    color:#333355;
    margin-top:4px;
    letter-spacing:1px;
  }

  /* ── LIVE INDICATOR WITH BLINKING RED DOT ── */
  .live-bar{
    display:flex;
    align-items:center;
    justify-content:center;
    gap:8px;
    margin-bottom:22px;
    font-size:0.72rem;
    color:#555577;
    letter-spacing:2px;
  }
  .red-dot{
    width:10px;
    height:10px;
    background:#ff2222;
    border-radius:50%;
    box-shadow:0 0 6px #ff2222, 0 0 12px #ff222244;
    animation:blink 1s step-start infinite;
  }
  @keyframes blink{
    0%,100%{ opacity:1; }
    50%{ opacity:0; }
  }

  .candidates{ display:flex; flex-direction:column; gap:11px; margin-bottom:24px; }
  .ccard{
    background:#0d0d1a;
    border:1px solid #1a1a3a;
    border-radius:10px;
    padding:14px 16px;
    position:relative;
    overflow:hidden;
    transition:border-color 0.4s, box-shadow 0.4s;
  }
  .ccard.leader{
    border-color:#7b7bff;
    box-shadow:0 0 20px #7b7bff18;
  }
  .ctop{
    display:flex;
    justify-content:space-between;
    align-items:center;
    margin-bottom:9px;
  }
  .cname{
    font-family:'Orbitron',sans-serif;
    font-size:0.85rem;
    color:#c8c8ff;
  }
  .cvotes{
    font-size:1.4rem;
    font-weight:700;
    color:#7b7bff;
  }
  .bar-bg{
    height:6px;
    background:#111128;
    border-radius:3px;
    overflow:hidden;
  }
  .bar-fill{
    height:100%;
    background:linear-gradient(90deg,#4444bb,#9999ff);
    border-radius:3px;
    transition:width 1s ease;
  }
  .cpct{
    text-align:right;
    font-size:0.68rem;
    color:#4444aa;
    margin-top:4px;
  }
  .badge{
    position:absolute;
    top:10px; right:14px;
    background:#7b7bff;
    color:#000;
    font-size:0.56rem;
    font-family:'Orbitron',sans-serif;
    padding:2px 7px;
    border-radius:3px;
    font-weight:700;
    letter-spacing:1px;
  }
  .rank{
    position:absolute;
    left:0; top:0; bottom:0;
    width:4px;
    border-radius:10px 0 0 10px;
  }
  .rank-1{ background:#7b7bff; }
  .rank-2{ background:#4444aa; }
  .rank-3{ background:#222266; }
  .rank-other{ background:#111133; }

  .refresh-btn{
    display:block;
    margin:0 auto 18px;
    background:transparent;
    border:1px solid #7b7bff;
    color:#7b7bff;
    font-family:'Share Tech Mono',monospace;
    font-size:0.8rem;
    padding:9px 26px;
    border-radius:4px;
    cursor:pointer;
    letter-spacing:2px;
    transition:all 0.2s;
  }
  .refresh-btn:hover{ background:#7b7bff18; }
  .footer{
    text-align:center;
    font-size:0.65rem;
    color:#1a1a33;
    padding:14px 0;
    letter-spacing:1px;
  }
  .last-updated{
    text-align:center;
    font-size:0.65rem;
    color:#333355;
    margin-bottom:16px;
  }
</style>
</head>
<body>

<div class="header">
  <h1>VOTING MACHINE</h1>
  <p class="sub">LIVE ELECTION DASHBOARD</p>
</div>

<div class="stats-row">
  <div class="stat-card">
    <div class="num" id="totalVotes">--</div>
    <div class="lbl">TOTAL VOTES</div>
  </div>
  <div class="stat-card">
    <div class="num" id="leadPct">--%</div>
    <div class="lbl">LEADER %</div>
  </div>
  <div class="stat-card">
    <div class="num">5</div>
    <div class="lbl">CANDIDATES</div>
  </div>
</div>

<!-- BLINKING RED DOT LIVE INDICATOR -->
<div class="live-bar">
  <div class="red-dot"></div>
  <span>LIVE &nbsp;&bull;&nbsp; AUTO-REFRESH EVERY 5s</span>
</div>

<div class="candidates" id="candidateList">
  <div style="text-align:center;color:#222244;padding:40px 0;">Loading...</div>
</div>

<div class="last-updated" id="lastUpdated"></div>

<button class="refresh-btn" onclick="fetchData()">&#8635; REFRESH NOW</button>

<div class="footer">ESP32 BIOMETRIC VOTING MACHINE &nbsp;|&nbsp; IOT PROJECT</div>

<script>
function pad(n){ return n<10?'0'+n:n; }
function timeNow(){
  const d=new Date();
  return pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());
}
const rankClass=['rank-1','rank-2','rank-3','rank-other','rank-other'];

function fetchData(){
  fetch('/api/results')
    .then(r=>r.json())
    .then(data=>{
      document.getElementById('totalVotes').textContent = data.total;
      let maxV=0;
      data.candidates.forEach(c=>{ if(c.votes>maxV) maxV=c.votes; });
      const lp = data.total>0 ? Math.round((maxV/data.total)*100) : 0;
      document.getElementById('leadPct').textContent = lp+'%';

      const list = document.getElementById('candidateList');
      list.innerHTML = '';

      const sorted = [...data.candidates].sort((a,b)=>b.votes-a.votes);
      sorted.forEach((c,idx)=>{
        const pct = data.total>0 ? Math.round((c.votes/data.total)*100) : 0;
        const isLeader = (c.votes===maxV && data.total>0);
        const card = document.createElement('div');
        card.className = 'ccard' + (isLeader?' leader':'');
        card.innerHTML = `
          <div class="rank ${rankClass[idx]||'rank-other'}"></div>
          ${isLeader ? '<div class="badge">LEADING</div>' : ''}
          <div class="ctop">
            <div class="cname">${c.name}</div>
            <div class="cvotes">${c.votes}</div>
          </div>
          <div class="bar-bg">
            <div class="bar-fill" style="width:${pct}%"></div>
          </div>
          <div class="cpct">${pct}%</div>
        `;
        list.appendChild(card);
      });

      document.getElementById('lastUpdated').textContent =
        'Last updated: ' + timeNow();
    })
    .catch(()=>{
      document.getElementById('candidateList').innerHTML =
        '<div style="text-align:center;color:#661111;padding:40px 0;">Connection lost. Retrying...</div>';
    });
}

fetchData();
setInterval(fetchData, 5000);
</script>
</body>
</html>
)rawhtml";

// ── WEB SERVER ROUTES ─────────────────────────
void handleRoot() {
  server.send(200, "text/html", DASHBOARD_HTML);
}

void handleResults() {
  StaticJsonDocument<512> doc;
  doc["total"] = totalVotesCast;
  JsonArray arr = doc.createNestedArray("candidates");
  for(int i=0;i<MAX_CANDIDATES;i++){
    JsonObject c = arr.createNestedObject();
    c["name"]    = candidatesFull[i];
    c["votes"]   = votes[i];
    c["percent"] = (totalVotesCast>0)?(votes[i]*100)/totalVotesCast:0;
  }
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ── WIFI SETUP ────────────────────────────────
void setupWiFi() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);  display.print("Connecting WiFi...");
  display.setCursor(0,14); display.print(WIFI_SSID);
  display.display();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries=0;
  while(WiFi.status()!=WL_CONNECTED && tries<20){
    delay(500); tries++;
    display.print("."); display.display();
  }

  if(WiFi.status()==WL_CONNECTED){
    wifiConnected=true;
    wifiIP=WiFi.localIP().toString();
    server.on("/",            handleRoot);
    server.on("/api/results", handleResults);
    server.onNotFound(handleNotFound);
    server.begin();
    display.clearDisplay();
    display.setCursor(0,0);  display.print("WiFi Connected!");
    display.setCursor(0,14); display.print("Dashboard:");
    display.setCursor(0,28); display.print("http://");
    display.print(wifiIP);
    display.display();
    delay(3000);
  } else {
    wifiConnected=false;
    display.clearDisplay();
    display.setCursor(0,0);  display.print("WiFi FAILED");
    display.setCursor(0,16); display.print("Running offline");
    display.display();
    delay(2000);
  }
}

// ── DISPLAY HELPERS ───────────────────────────
void dispTitle(const char* t){
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0); display.println(t);
  display.drawLine(0,10,127,10,SSD1306_WHITE);
}

// OLED candidate list — scrollable for 5 candidates
int candidateScrollOffset = 0;
void showSelectCandidate(){
  display.clearDisplay();
  dispTitle(" SELECT CANDIDATE");
  // Show 4 candidates at a time, scroll based on selection
  int startIdx = selectedCandidate > 3 ? selectedCandidate - 3 : 0;
  for(int i=startIdx; i<MAX_CANDIDATES && i<startIdx+4; i++){
    int y = 14 + (i - startIdx) * 12;
    if(i==selectedCandidate){
      display.fillRect(0,y-1,128,11,SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4,y); display.setTextSize(1);
    display.print(i+1); display.print(". ");
    display.print(candidates[i]);
  }
  // Scroll indicator
  if(MAX_CANDIDATES > 4){
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(118, 14); display.print(selectedCandidate<MAX_CANDIDATES-1?"v":"");
    display.setCursor(118, 2);  display.print(selectedCandidate>0?"^":"");
  }
  display.setTextColor(SSD1306_WHITE);
  display.display();
}

void showIdle(){
  display.clearDisplay();
  dispTitle("  VOTING MACHINE");
  display.setCursor(10,14); display.print("Press [SELECT]");
  display.setCursor(10,24); display.print("to cast your vote");
  if(wifiConnected){
    display.setCursor(0,38); display.print("Live:");
    display.setCursor(0,50); display.print(wifiIP);
  } else {
    display.setCursor(0,40); display.print("WiFi: Offline");
    display.setCursor(0,52); display.print("[BACK]x3=Admin");
  }
  display.display();
}

void showScanFinger(){
  display.clearDisplay();
  dispTitle("  SCAN FINGER");
  display.setCursor(15,20); display.print("Place your");
  display.setCursor(15,32); display.print("finger on");
  display.setCursor(15,44); display.print("the sensor...");
  display.display();
}

void showConfirmVote(){
  display.clearDisplay();
  dispTitle("  CONFIRM VOTE");
  display.setCursor(0,14); display.print("You selected:");
  display.setCursor(0,26); display.println(candidates[selectedCandidate]);
  display.setCursor(0,42); display.print("[SELECT] Confirm");
  display.setCursor(0,53); display.print("[BACK]   Cancel");
  display.display();
}

void showVoteSuccess(){
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(20,8);  display.print("VOTED!");
  display.setTextSize(1);
  display.setCursor(0,32);  display.print("Your vote counts!");
  display.setCursor(0,44);  display.print("Total: ");
  display.print(totalVotesCast);
  display.display();
}

void showAlreadyVoted(){
  display.clearDisplay();
  dispTitle("  ACCESS DENIED");
  display.setCursor(0,16); display.print("Already voted!");
  display.setCursor(0,28); display.print("One vote per");
  display.setCursor(0,40); display.print("voter only.");
  display.display();
}

void showFPNotFound(){
  display.clearDisplay();
  dispTitle("  NOT REGISTERED");
  display.setCursor(0,16); display.print("Fingerprint not");
  display.setCursor(0,28); display.print("in system.");
  display.setCursor(0,44); display.print("See admin.");
  display.display();
}

void showAdminMenu(int sel){
  display.clearDisplay();
  dispTitle("  ADMIN MENU");
  const char* items[]={"1. Enroll Voter","2. View Results","3. Reset Election","4. Exit"};
  for(int i=0;i<4;i++){
    int y=14+i*12;
    if(i==sel){
      display.fillRect(0,y-1,128,11,SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4,y); display.setTextSize(1);
    display.print(items[i]);
  }
  display.setTextColor(SSD1306_WHITE);
  display.display();
}

void showResults(){
  display.clearDisplay();
  dispTitle("  RESULTS");
  int total=0;
  for(int i=0;i<MAX_CANDIDATES;i++) total+=votes[i];
  // Show first 4, scroll not needed for results view
  for(int i=0;i<MAX_CANDIDATES&&i<4;i++){
    int y=14+i*12;
    display.setCursor(0,y); display.setTextSize(1);
    display.print(candidates[i]);
    display.print(":");
    display.print(votes[i]);
    if(total>0){
      display.print("(");
      display.print((votes[i]*100)/total);
      display.print("%)");
    }
  }
  display.display();
}

void showMessage(const char* l1, const char* l2="", const char* l3=""){
  display.clearDisplay(); display.setTextSize(1);
  display.setCursor(0,16); display.print(l1);
  display.setCursor(0,30); display.print(l2);
  display.setCursor(0,44); display.print(l3);
  display.display();
}

// ── BUTTON ───────────────────────────────────
bool btnPressed(int idx){
  if(digitalRead(BTN_PINS[idx])==LOW&&(millis()-lastBtnTime[idx])>DEBOUNCE){
    lastBtnTime[idx]=millis(); beepShort(); return true;
  }
  return false;
}

// ── FINGERPRINT ──────────────────────────────
int getFingerprintID(){
  if(finger.getImage()!=FINGERPRINT_OK)         return -1;
  if(finger.image2Tz()!=FINGERPRINT_OK)         return -1;
  if(finger.fingerFastSearch()!=FINGERPRINT_OK) return -2;
  return finger.fingerID;
}

bool enrollFinger(uint8_t id){
  int p=-1;
  showMessage("Enroll Voter","Step 1/2:","Place finger...");
  while(p!=FINGERPRINT_OK){
    p=finger.getImage();
    if(btnPressed(3)) return false;
    delay(50);
  }
  if(finger.image2Tz(1)!=FINGERPRINT_OK){ showMessage("Image error","Try again"); delay(2000); return false; }
  showMessage("Remove","finger","");
  delay(1500);
  while(finger.getImage()!=FINGERPRINT_NOFINGER) delay(100);
  p=-1;
  showMessage("Step 2/2:","Same finger","again...");
  while(p!=FINGERPRINT_OK){
    p=finger.getImage();
    if(btnPressed(3)) return false;
    delay(50);
  }
  if(finger.image2Tz(2)!=FINGERPRINT_OK){ showMessage("Image error","Try again"); delay(2000); return false; }
  if(finger.createModel()!=FINGERPRINT_OK){ showMessage("No match!","Try again"); beepError(); delay(2000); return false; }
  if(finger.storeModel(id)!=FINGERPRINT_OK){ showMessage("Store failed!"); beepError(); delay(2000); return false; }
  return true;
}

// ── ADMIN MENU ────────────────────────────────
void handleAdminMenu(){
  if(btnPressed(0)){ adminMenuSel=(adminMenuSel-1+4)%4; showAdminMenu(adminMenuSel); }
  if(btnPressed(1)){ adminMenuSel=(adminMenuSel+1)%4;   showAdminMenu(adminMenuSel); }
  if(btnPressed(2)){
    switch(adminMenuSel){
      case 0:
        if(enrollFinger(enrollID)){
          showMessage("Enrolled!",("ID:"+String(enrollID)).c_str(),"");
          beepSuccess(); enrollID++;
        } else {
          showMessage("Enroll Failed"); beepError();
        }
        delay(2000); showAdminMenu(adminMenuSel);
        break;
      case 1:
        showResults(); delay(5000);
        showAdminMenu(adminMenuSel);
        break;
      case 2:
        showMessage("Hold BACK to","confirm reset","");
        delay(2000);
        if(digitalRead(BTN_BACK)==LOW){
          clearAllData(); beepLong();
          showMessage("Reset Complete","All cleared","");
          delay(2000);
        } else {
          showMessage("Cancelled","","");
          delay(1500);
        }
        showAdminMenu(adminMenuSel);
        break;
      case 3:
        currentState=STATE_IDLE; showIdle();
        break;
    }
  }
  if(btnPressed(3)){ currentState=STATE_IDLE; showIdle(); }
}

// ── SETUP ────────────────────────────────────
void setup(){
  Serial.begin(115200);
  pinMode(BTN_UP,INPUT_PULLUP);     pinMode(BTN_DOWN,INPUT_PULLUP);
  pinMode(BTN_SELECT,INPUT_PULLUP); pinMode(BTN_BACK,INPUT_PULLUP);
  pinMode(BUZZER_PIN,OUTPUT);       digitalWrite(BUZZER_PIN,LOW);

  EEPROM.begin(EEPROM_SIZE);
  loadVotes(); loadVotedFlags();

  if(!display.begin(SSD1306_SWITCHCAPVCC)){
    Serial.println("OLED failed"); while(true);
  }
  display.clearDisplay(); display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(10,5);  display.print("  VOTING MACHINE");
  display.setCursor(20,20); display.print("Biometric v3.0");
  display.setCursor(10,36); display.print("Initializing...");
  display.display();

  fpSerial.begin(57600,SERIAL_8N1,FP_RX_PIN,FP_TX_PIN);
  finger.begin(57600);
  delay(1000);

  if(finger.verifyPassword()){
    display.setCursor(10,50); display.print("Sensor OK");
  } else {
    display.setCursor(10,50); display.print("Sensor ERROR!");
    beepError();
  }
  display.display();
  delay(2000);

  setupWiFi();

  beepSuccess();
  currentState=STATE_IDLE;
  showIdle();
}

// ── LOOP ─────────────────────────────────────
void loop(){
  if(wifiConnected) server.handleClient();

  // Secret admin: BACK x3 within 3 seconds
  if(digitalRead(BTN_BACK)==LOW&&(millis()-lastBtnTime[3])>DEBOUNCE){
    lastBtnTime[3]=millis();
    if(millis()-backPressTimer>3000){ backPressCount=0; backPressTimer=millis(); }
    backPressCount++;
    if(backPressCount>=3&&currentState==STATE_IDLE){
      backPressCount=0;
      currentState=STATE_ADMIN_MENU; adminMenuSel=0;
      showAdminMenu(adminMenuSel); beepShort(); return;
    }
  }

  switch(currentState){

    case STATE_IDLE:
      if(btnPressed(2)){ currentState=STATE_SCAN_FINGER; showScanFinger(); }
      break;

    case STATE_SCAN_FINGER:{
      int id=getFingerprintID();
      if(id>0){
        verifiedFingerID=id;
        if(id==ADMIN_FP_ID){
          currentState=STATE_ADMIN_MENU; adminMenuSel=0;
          showAdminMenu(adminMenuSel); beepShort();
        } else if(hasVoted[id]){
          currentState=STATE_ALREADY_VOTED;
          showAlreadyVoted(); beepError();
          stateTimer=millis();
        } else {
          selectedCandidate=0;
          currentState=STATE_SELECT_CANDIDATE;
          showSelectCandidate(); beepSuccess();
        }
      } else if(id==-2){
        currentState=STATE_FP_NOT_FOUND;
        showFPNotFound(); beepError();
        stateTimer=millis();
      }
      if(btnPressed(3)){ currentState=STATE_IDLE; showIdle(); }
      break;
    }

    case STATE_SELECT_CANDIDATE:
      if(btnPressed(0)){ selectedCandidate=(selectedCandidate-1+MAX_CANDIDATES)%MAX_CANDIDATES; showSelectCandidate(); }
      if(btnPressed(1)){ selectedCandidate=(selectedCandidate+1)%MAX_CANDIDATES; showSelectCandidate(); }
      if(btnPressed(2)){ currentState=STATE_CONFIRM_VOTE; showConfirmVote(); }
      if(btnPressed(3)){ currentState=STATE_IDLE; showIdle(); }
      break;

    case STATE_CONFIRM_VOTE:
      if(btnPressed(2)){
        votes[selectedCandidate]++;
        totalVotesCast++;
        hasVoted[verifiedFingerID]=true;
        saveVotes(); saveVotedFlags();
        Serial.printf("[VOTE] FP_ID:%d → %s | Total:%d\n",
                      verifiedFingerID, candidatesFull[selectedCandidate], totalVotesCast);
        currentState=STATE_VOTE_SUCCESS;
        showVoteSuccess(); beepSuccess();
        stateTimer=millis();
      }
      if(btnPressed(3)){ currentState=STATE_SELECT_CANDIDATE; showSelectCandidate(); }
      break;

    case STATE_VOTE_SUCCESS:
    case STATE_ALREADY_VOTED:
    case STATE_FP_NOT_FOUND:
      if(millis()-stateTimer>3000){ currentState=STATE_IDLE; showIdle(); }
      break;

    case STATE_ADMIN_MENU:
      handleAdminMenu();
      break;

    default:
      currentState=STATE_IDLE; showIdle();
      break;
  }
  delay(20);
}
