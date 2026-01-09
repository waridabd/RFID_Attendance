#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP8266WiFi.h>
#include <LiquidCrystal_I2C.h>
#include <Firebase_ESP_Client.h>

#include <WiFiUdp.h>
#include <NTPClient.h>

// ------------------- Hardware pins -------------------
#define RST_PIN D3       // RC522 RST
#define SS_PIN  D4       // RC522 SDA/SS
#define BUZZER  D8       // Buzzer +

// Wi‑Fi
const char* WIFI_SSID     = "Anika's A36";
const char* WIFI_PASSWORD = "anika7766";

// Firebase
#define FIREBASE_HOST "rfidattendance-78e12-default-rtdb.asia-southeast1.firebasedatabase.app"
// =========================================================

const String DEVICE_ID = "DEV1";

// ------------------- Session rules -------------------
const unsigned long SESSION_SECONDS = 40UL * 60UL;   // 40 minutes
const unsigned long DUPLICATE_MS    = 2000;          

// ------------------- Firebase objects -------------------
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ------------------- LCD -------------------
LiquidCrystal_I2C lcd(0x27, 16, 2);
//LiquidCrystal_I2C lcd(0x3F, 16, 2);

// ------------------- RFID -------------------
MFRC522 rfid(SS_PIN, RST_PIN);

// ------------------- NTP time -------------------
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 21600, 60 * 1000);

// ------------------- State -------------------
String currentCourseCode = "";
String currentCourseUid  = "";
String currentSessionKey = "";          
unsigned long sessionStartEpoch = 0;    
bool sessionActive = false;

String lastUid = "";
unsigned long lastScanMs = 0;

// ------------------- Helpers -------------------
String uidToHex(const MFRC522::Uid& uid) {
  String s;
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 16) s += "0";
    s += String(uid.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

void beep(uint8_t times=1, uint16_t onMs=120, uint16_t offMs=120){
  for(uint8_t i=0;i<times;i++){
    digitalWrite(BUZZER,HIGH);
    delay(onMs);
    digitalWrite(BUZZER,LOW);
    delay(offMs);
  }
}

void show(const String& l1, const String& l2=""){
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(l1.substring(0,16));
  lcd.setCursor(0,1); lcd.print(l2.substring(0,16));
}

bool getStringPath(const String& path, String &out) {
  if (Firebase.RTDB.getString(&fbdo, path)) {
    out = fbdo.stringData();
    return true;
  }
  return false;
}

unsigned long nowEpoch() {
  timeClient.update();               
  return (unsigned long)timeClient.getEpochTime();
}

String two(int v) { return (v < 10) ? "0" + String(v) : String(v); }

String dateYYYYMMDD(unsigned long epoch) {
  time_t raw = (time_t)epoch;
  struct tm *ptm = gmtime(&raw);       
  int y = ptm->tm_year + 1900;
  int m = ptm->tm_mon + 1;
  int d = ptm->tm_mday;
  return String(y) + "-" + two(m) + "-" + two(d);
}

String timeHHMMSS(unsigned long epoch) {
  time_t raw = (time_t)epoch;
  struct tm *ptm = gmtime(&raw);
  return two(ptm->tm_hour) + ":" + two(ptm->tm_min) + ":" + two(ptm->tm_sec);
}

String makeSessionKey(unsigned long epoch) {
  time_t raw = (time_t)epoch;
  struct tm *ptm = gmtime(&raw);
  return dateYYYYMMDD(epoch) + "_" + two(ptm->tm_hour) + "-" + two(ptm->tm_min);
}

bool isSessionExpired(unsigned long nowE) {
  if (!sessionActive) return true;
  return (nowE - sessionStartEpoch) >= SESSION_SECONDS;
}

String indexPathForStudent(const String& course, const String& sessionKey, const String& uid) {
  return "/attendanceIndex/" + course + "/" + sessionKey + "/" + uid;
}

String logPathForCourse(const String& course, const String& sessionKey) {
  return "/attendanceLogs/" + course + "/" + sessionKey;
}

bool studentAlreadyScanned(const String& uid) {
  if (!sessionActive) return false;
  String p = indexPathForStudent(currentCourseCode, currentSessionKey, uid);
  if (Firebase.RTDB.getBool(&fbdo, p)) {
    return fbdo.boolData();  
  return false; 

void markStudentScanned(const String& uid) {
  String p = indexPathForStudent(currentCourseCode, currentSessionKey, uid);
  Firebase.RTDB.setBool(&fbdo, p, true);
}

void logAttendance(const String& uid, const String& studentName, unsigned long ts) {
  FirebaseJson json;
  json.set("uid", uid);
  json.set("studentName", studentName);
  json.set("courseCode", currentCourseCode);
  json.set("device", DEVICE_ID);

  json.set("sessionKey", currentSessionKey);
  json.set("sessionStartEpoch", (int)sessionStartEpoch);
  json.set("ts_epoch", (int)ts);
  json.set("ts_date", dateYYYYMMDD(ts));
  json.set("ts_time", timeHHMMSS(ts));

  String logPath = logPathForCourse(currentCourseCode, currentSessionKey);

  if (Firebase.RTDB.pushJSON(&fbdo, logPath, &json)) {
    Serial.println("Log OK: " + fbdo.pushName());
  } else {
    Serial.println("Log FAIL: " + fbdo.errorReason());
  }
}

void endSessionUI() {
  sessionActive = false;
  currentCourseCode = "";
  currentCourseUid  = "";
  currentSessionKey = "";
  sessionStartEpoch = 0;
  show("Session ended", "Scan course");
  beep(3, 100, 100);
}

void setup() {
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  Serial.begin(115200);

  lcd.init();
  lcd.backlight();
  show("Initializing...", "");

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    show("Connecting WiFi", "");
    delay(500);
  }
  show("WiFi connected", WiFi.localIP().toString());
  delay(600);

  // Start NTP
  timeClient.begin();
  unsigned long t0 = millis();
  while (nowEpoch() < 1700000000UL && (millis() - t0) < 8000) {  
    delay(200);
  }

  // Firebase
  show("Connecting", "Firebase");
  config.database_url = "https://" FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // RFID
  SPI.begin();
  rfid.PCD_Init();

  show("Ready", "Scan course");
}

void loop() {
  unsigned long ts = nowEpoch();

  // Auto end session if expired
  if (sessionActive && isSessionExpired(ts)) {
    endSessionUI();
  }

  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  String uid = uidToHex(rfid.uid);

  // Very quick duplicate filter (2 seconds)
  if (uid == lastUid && (millis() - lastScanMs) < DUPLICATE_MS) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }
  lastUid = uid;
  lastScanMs = millis();

  Serial.println("UID=" + uid);

  // 1) Course tag?
  String courseCode;
  if (getStringPath("/config/courses/" + uid + "/code", courseCode)) {

    // If session already active -> block re-scan
    if (sessionActive && !isSessionExpired(ts)) {
      if (uid == currentCourseUid) {
        show("Already selected", currentCourseCode);
        beep(2, 80, 80);
      } else {
        show("Session running", "Reset board");
        beep(3, 120, 120);
      }
    } else {
      // Start NEW session
      currentCourseCode = courseCode;
      currentCourseUid  = uid;
      sessionStartEpoch = ts;
      currentSessionKey = makeSessionKey(ts);
      sessionActive = true;

      show("Course selected", currentCourseCode);
      beep(1);

      Firebase.RTDB.setString(&fbdo, "/devices/" + DEVICE_ID + "/currentCourse", currentCourseCode);
      Firebase.RTDB.setString(&fbdo, "/devices/" + DEVICE_ID + "/sessionKey", currentSessionKey);
      Firebase.RTDB.setInt(&fbdo, "/devices/" + DEVICE_ID + "/sessionStartEpoch", (int)sessionStartEpoch);
    }

  } else {

    // 2) Student tag?
    String studentName;
    if (getStringPath("/config/students/" + uid + "/name", studentName)) {

      if (!sessionActive) {
        show("Scan course", "card first!");
        beep(3, 80, 80);
      } else if (isSessionExpired(ts)) {
        endSessionUI();
      } else if (studentAlreadyScanned(uid)) {
        show("Already scanned", studentName);
        beep(2, 100, 100);
      } else {
        // OK to record
        show(studentName, "Recorded");
        beep(2);
        logAttendance(uid, studentName, ts);
        markStudentScanned(uid);
      }

    } else {
      // 3) Unknown tag
      show("Unknown card", "");
      beep(3, 150, 120);
    }
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  delay(700);
  show("Scan a card", "");
}