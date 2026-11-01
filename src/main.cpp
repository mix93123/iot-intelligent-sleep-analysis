#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>

// --- Wi-Fi Configuration ---
const char* ssid     = "Jon_2.4G";     
const char* password = "42413418"; 

// --- InfluxDB v2 Configuration (Docker Localhost IP) ---
// ⚠️ เปลี่ยน 192.168.X.X ให้เป็น IP เครื่องคอมพิวเตอร์ของคุณภาณุ (ดูผ่าน cmd -> ipconfig)
const char* INFLUX_IP = "192.168.1.144"; // ตัวอย่าง IP เครื่องคอมพิวเตอร์
const int   INFLUX_PORT = 8086;
const char* INFLUX_ORG = "buu_iot";
const char* INFLUX_BUCKET = "sleep_monitoring";
const char* INFLUX_TOKEN = "SWPFueN0jN2TwkVoZGKxXgnZ1itHJY6QpNWDBpra-h5GAPBAhxrhPZUHu8iCwoi3tp_Sj4-JE0Kni5Y5ynBpEg==";

// --- Hardware Pin Config ---
#define DHTPIN    4
#define DHTTYPE   DHT11
#define PIR_PIN   13
#define LIGHT_PIN 34
#define SOUND_PIN 35  // ขา AO ของ KY-038

DHT dht(DHTPIN, DHTTYPE);

volatile bool motionDetected = false;
void IRAM_ATTR onMotionTrigger() { motionDetected = true; }

// --- Downsampling & Aggregation Variables ---
const unsigned long SEND_INTERVAL = 120000; // ส่งข้อมูลขึ้น InfluxDB ทุกๆ 2 นาที (120,000 ms)
const unsigned long SAMPLE_INTERVAL = 5000;  // อ่านค่าจากเซ็นเซอร์ย่อยๆ ทุก 5 วินาที

unsigned long lastSendTime = 0;
unsigned long lastSampleTime = 0;

float tempSum = 0, humSum = 0, lightSum = 0;
int sampleCount = 0;

bool peakSoundNoisy = false;
bool peakMotionDetected = false;

void connectWiFi() {
  Serial.println("\nConnecting to Wi-Fi (2.4GHz)...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) { 
    delay(500); 
    Serial.print("."); 
  }
  Serial.println("\nWi-Fi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  dht.begin();
  pinMode(PIR_PIN,   INPUT);
  pinMode(LIGHT_PIN, INPUT);
  pinMode(SOUND_PIN, INPUT);

  attachInterrupt(digitalPinToInterrupt(PIR_PIN), onMotionTrigger, RISING);

  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected! Reconnecting...");
    connectWiFi();
    delay(2000);
    return;
  }

  unsigned long currentMillis = millis();

  // 1. อ่านค่าเซ็นเซอร์เพื่อเก็บสถิติย่อย (ทุกๆ 5 วินาที)
  if (currentMillis - lastSampleTime >= SAMPLE_INTERVAL) {
    lastSampleTime = currentMillis;

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    int l = analogRead(LIGHT_PIN);

    if (!isnan(t) && !isnan(h)) {
      tempSum += t;
      humSum += h;
      lightSum += l;
      sampleCount++;
    }

    // ตรวจสอบระดับเสียง (Peak Detection)
    int soundMin = 4095, soundMax = 0;
    for (int i = 0; i < 50; i++) {
      int s = analogRead(SOUND_PIN);
      if (s > soundMax) soundMax = s;
      if (s < soundMin) soundMin = s;
      delay(2);
    }
    if ((soundMax - soundMin) > 220) {
      peakSoundNoisy = true; // บันทึก Peak หากเจอเสียงดังชั่วขณะ
    }

    // ตรวจสอบ Motion จาก PIR Interrupt
    if (motionDetected) {
      peakMotionDetected = true;
      motionDetected = false; // รีเซ็ต Flag
    }
  }

  // 2. เมื่อครบระยะเวลา 2 นาที (SEND_INTERVAL) ให้เฉลี่ยและส่งข้อมูลเข้า InfluxDB
  if (currentMillis - lastSendTime >= SEND_INTERVAL) {
    lastSendTime = currentMillis;

    if (sampleCount > 0) {
      detachInterrupt(digitalPinToInterrupt(PIR_PIN));

      // คำนวณค่าเฉลี่ย (Downsampled Values)
      float avgTemp  = tempSum / sampleCount;
      float avgHum   = humSum / sampleCount;
      int avgLight   = lightSum / sampleCount;

      int soundVal = peakSoundNoisy ? 1 : 0;
      int motionVal = peakMotionDetected ? 1 : 0;

      // แสดงผลผ่าน Serial Monitor
      Serial.println("\n--- [Sending Aggregated Data to InfluxDB (Every 2 Mins)] ---");
      Serial.print("Avg Temp: ");     Serial.print(avgTemp, 1);    Serial.println(" C");
      Serial.print("Avg Humidity: "); Serial.print(avgHum, 1);     Serial.println(" %");
      Serial.print("Avg Light: ");    Serial.print(avgLight);      Serial.println();
      Serial.print("Sound Flag: ");   Serial.println(soundVal);
      Serial.print("Motion Flag: ");  Serial.println(motionVal);

      // สร้าง InfluxDB Line Protocol Body
      // Format: measurement,tag_key=tag_val field1=val1,field2=val2
      String lineProtocol = "sleep_metrics,device=esp32_node ";
      lineProtocol += "temp=" + String(avgTemp, 1) + ",";
      lineProtocol += "hum=" + String(avgHum, 1) + ",";
      lineProtocol += "light=" + String(avgLight) + ",";
      lineProtocol += "sound=" + String(soundVal) + ",";
      lineProtocol += "motion=" + String(motionVal);

      // ส่ง HTTP POST ไปที่ InfluxDB v2 Write API
      HTTPClient http;
      String influxUrl = "http://" + String(INFLUX_IP) + ":" + String(INFLUX_PORT) + 
                         "/api/v2/write?org=" + String(INFLUX_ORG) + 
                         "&bucket=" + String(INFLUX_BUCKET) + "&precision=s";

      http.begin(influxUrl);
      http.addHeader("Authorization", "Token " + String(INFLUX_TOKEN));
      http.addHeader("Content-Type", "text/plain; charset=utf-8");

      int httpCode = http.POST(lineProtocol);
      
      if (httpCode == 204) {
        Serial.println("✅ InfluxDB Write Success (HTTP 204)");
      } else {
        Serial.print("❌ InfluxDB Write Failed! HTTP Code: ");
        Serial.println(httpCode);
      }
      http.end();

      // รีเซ็ตค่าตัวแปรสะสมเพื่อรอรอบถัดไป
      tempSum = 0;
      humSum = 0;
      lightSum = 0;
      sampleCount = 0;
      peakSoundNoisy = false;
      peakMotionDetected = false;

      attachInterrupt(digitalPinToInterrupt(PIR_PIN), onMotionTrigger, RISING);
    }
  }
}