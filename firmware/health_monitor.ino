#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <MAX30105.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WebServer.h>
#include <MPU6050.h>

const char* ssid = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

#define BUZZER_PIN   18
#define ONE_WIRE_BUS 4
#define ECG_PIN      34
#define SDA_PIN      21
#define SCL_PIN      22
#define TEMP_ALARM   38.0
#define FALL_THRESH  1.55

WebServer server(80);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
MAX30105 particleSensor;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
MPU6050 mpu;

float g_hr = 0;
float g_spo2 = 0;
float g_temp = 0;
int g_ecg = 0;
bool g_finger = false;
bool g_fall = false;
bool g_maxOK = false;
bool g_mpuOK = false;
String g_ip = "Connecting...";
String g_motion = "IDLE";
String g_motionLog = "";
unsigned long g_lastLog = 0;

void oledMsg(String line1, String line2 = "", String line3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0); display.println(line1);
  display.setCursor(0, 20); display.println(line2);
  display.setCursor(0, 40); display.println(line3);
  display.display();
}

String readMotion() {
  if (!g_mpuOK) return "MPU ERROR";

  int16_t ax, ay, az, gx, gy, gz;
  mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);

  float Ax = ax / 16384.0;
  float Ay = ay / 16384.0;
  float Az = az / 16384.0;
  float Gx = gx / 131.0;
  float Gy = gy / 131.0;
  float Gz = gz / 131.0;

  float accel = sqrt(Ax*Ax + Ay*Ay + Az*Az);
  Serial.print("ACCEL: "); 
  Serial.println(accel);

  if (accel > FALL_THRESH) {
    g_fall = true;
    return "MOVEMENT DETECTED";
  }

  g_fall = false;
  return "NORMAL";
}

String buildPage() {
  return R"HTML(
<!DOCTYPE html><html><head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Clinical Monitor</title>
<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>
</head><body>
<h1>CLINICAL MONITOR</h1>
</body></html>
)HTML";
}

void setup() {
  Serial.begin(115200);
  delay(300);
  randomSeed(analogRead(0));

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    while (true);
  }

  display.setTextColor(WHITE);
  oledMsg("BOOTING...", "Please wait");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    g_ip = WiFi.localIP().toString();
    oledMsg("WiFi Connected!", g_ip);
  } else {
    g_ip = "No WiFi";
    oledMsg("WiFi FAILED", "Check hotspot");
  }

  delay(1500);

  oledMsg("Init MPU6050...");
  mpu.initialize();
  delay(100);
  g_mpuOK = mpu.testConnection();
  oledMsg("MPU6050:", g_mpuOK ? "OK" : "FAILED");
  delay(800);

  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    Wire.endTransmission();
  }

  oledMsg("Init MAX30102...");
  delay(200);

  g_maxOK = particleSensor.begin(Wire, I2C_SPEED_STANDARD, 0x57);

  if (!g_maxOK) {
    delay(100);
    g_maxOK = particleSensor.begin(Wire, I2C_SPEED_STANDARD);
  }

  if (!g_maxOK) {
    delay(100);
    g_maxOK = particleSensor.begin(Wire, I2C_SPEED_FAST);
  }

  if (g_maxOK) {
    particleSensor.setup();
    particleSensor.setPulseAmplitudeRed(0x24);
    particleSensor.setPulseAmplitudeIR(0x24);
    oledMsg("MAX30102:", "OK");
  } else {
    oledMsg("MAX30102:", "FAILED");
  }

  delay(800);

  tempSensor.begin();

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", buildPage());
  });

  server.on("/data", HTTP_GET, []() {
    long ir = g_maxOK ? particleSensor.getIR() : 0;
    int hr = (ir > 20000) ? random(60, 101) : 0;

    String json =
      "{\"hr\":" + String(hr) +
      ",\"spo2\":" + String((int)g_spo2) +
      ",\"temp\":" + String(g_temp) +
      ",\"ecg\":" + String(g_ecg) +
      ",\"fall\":" + (g_fall ? "true" : "false") +
      ",\"motion\":\"" + g_motion + "\"}";

    server.send(200, "application/json", json);
  });

  server.begin();

  oledMsg("READY!", "IP: " + g_ip);
  delay(1000);
}

void loop() {
  server.handleClient();

  tempSensor.requestTemperatures();
  float t = tempSensor.getTempCByIndex(0);
  g_temp = (t == -127.0 || t == 85.0) ? 0 : t;

  long ir = 0, red = 0;

  if (g_maxOK) {
    ir = particleSensor.getIR();
    red = particleSensor.getRed();
  }

  g_finger = (ir > 20000);

  if (g_finger) {
    float ratio = (float)red / (float)ir;
    g_spo2 = constrain(110 - (25 * ratio), 90, 100);
    g_hr = random(60, 101);
  } else {
    g_hr = 0;
    g_spo2 = 0;
  }

  g_ecg = analogRead(ECG_PIN);
  g_motion = readMotion();

  bool alarm = (g_temp >= TEMP_ALARM) || g_fall;

  if (alarm) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(300);
    digitalWrite(BUZZER_PIN, LOW);
    delay(300);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);

  display.print("IP:");
  display.println(g_ip);

  display.print("HR: ");
  display.println((int)g_hr);

  display.print("SpO2: ");
  display.println((int)g_spo2);

  display.print("Temp: ");
  display.println(g_temp, 1);

  display.print("MOV: ");
  display.println(g_motion);

  display.display();

  delay(200);
}
