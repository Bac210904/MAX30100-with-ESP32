#include <Wire.h>
#include <MAX30100_PulseOximeter.h> // Đảm bảo bạn đã cài đặt thư viện này
#include <esp32-hal.h>   
#include <Adafruit_GFX.h>          // Thư viện đồ họa cho OLED
#include <Adafruit_SSD1306.h>      // Thư viện điều khiển OLED SSD1306

// --- Định nghĩa các hằng số ---
#define REPORTING_PERIOD_MS 2000    // Chu kỳ cập nhật dữ liệu và hiển thị (2 giây)
#define SDA_PIN 21                  // Chân SDA cho giao tiếp I2C trên ESP32
#define SCL_PIN 22                  // Chân SCL cho giao tiếp I2C trên ESP32
#define SCREEN_WIDTH 128            // Chiều rộng màn hình OLED
#define SCREEN_HEIGHT 64            // Chiều cao màn hình OLED
#define OLED_RESET -1               // Pin reset cho OLED (hoặc -1 nếu không sử dụng)
#define OLED_ADDRESS 0x3C           // Địa chỉ I2C mặc định của OLED SSD1306

// Thêm các thông số cho bộ lọc và kiểm tra độ tin cậy của dữ liệu
#define MIN_RELIABLE_HR 40          // Nhịp tim tối thiểu hợp lệ (BPM)
#define MAX_RELIABLE_HR 120         // Nhịp tim tối đa hợp lệ (BPM) - Điều chỉnh nếu cần cho người vận động
#define MIN_RELIABLE_SPO2 70        // SpO2 tối thiểu hợp lệ (%)

// --- Khởi tạo đối tượng ---
PulseOximeter pox; // Đối tượng cảm biến MAX30100
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET); // Đối tượng màn hình OLED

#define buzzer 25 // Chân GPIO kết nối với còi báo

// --- Biến toàn cục để điều khiển trạng thái ---
uint32_t tLastReport = 0;           
bool beatShown = false;             // Cờ báo hiệu nhịp đập đã được hiển thị/báo còi
unsigned long beatTime = 0;       
const unsigned long beatDuration = 100; // Thời gian còi kêu khi phát hiện nhịp (ms)
bool isBeeping = false;            

// Biến kiểm tra ánh sáng mạnh
bool brightLightDetected = false;       // Cờ báo hiệu có khả năng nhiễu ánh sáng mạnh
unsigned long brightLightStartTime = 0; 
const unsigned long brightLightTimeout = 3000; // Thời gian tối đa cho phép nhiễu trước khi báo lỗi (3 giây)

// --- Hàm scan I2C (Hữu ích để debug kết nối) ---
void scanI2C() {
  byte error, address;
  int nDevices = 0;
  Serial.println("Scanning I2C addresses...");
  for (address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
      nDevices++;
    } else if (error == 4) {
      Serial.print("Unknown error at address 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
    }
  }
  if (nDevices == 0) Serial.println("No I2C devices found!");
  else Serial.println("Scan completed.");
}
// --- Hàm gọi lại khi phát hiện nhịp đập ---
void onBeatDetected() {
  beatShown = true;
  tone(buzzer, 1000); 
  beatTime = millis();
  isBeeping = true;
}
//Hàm kiểm tra độ tin cậy của dữ liệu
bool isDataReliable(float heartRate, float spO2) {
  if (heartRate < MIN_RELIABLE_HR || heartRate > MAX_RELIABLE_HR) return false;
  if (spO2 < MIN_RELIABLE_SPO2) return false; 
  // Nếu cả hai đều trong phạm vi thì dữ liệu đáng tin cậy
  return true;
}
// Hàm kiểm tra ánh sáng mạnh
void checkBrightLight(float heartRate, float spO2) {
  if ((heartRate > 0 || spO2 > 0) && !isDataReliable(heartRate, spO2)) {
    if (!brightLightDetected) { // Nếu đây là lần đầu phát hiện nhiễu
      brightLightDetected = true;
      brightLightStartTime = millis(); // Ghi lại thời điểm bắt đầu
      Serial.println("Warning: Possible bright light interference!");
    }
  } 
  // Nếu dữ liệu hợp lệ, reset cờ cảnh báo
  else if (isDataReliable(heartRate, spO2)) {
    brightLightDetected = false;
  }
  
  // Kiểm tra nếu tình trạng nhiễu ánh sáng kéo dài quá thời gian cho phép
  if (brightLightDetected && (millis() - brightLightStartTime > brightLightTimeout)) {
    Serial.println("Error: Prolonged bright light exposure! Please cover sensor.");
    // Có thể thêm cảnh báo liên tục trên màn hình hoặc còi báo động ở đây
  }
}

// --- Hàm Setup (Chạy một lần khi khởi động) ---
void setup() {
  Serial.begin(115200); // Khởi tạo Serial Monitor
  while (!Serial) delay(100);
  // Khởi tạo giao tiếp I2C cho ESP32
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000); 
  scanI2C();
  pinMode(buzzer, OUTPUT);

  // --- Khởi tạo màn hình OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println(F("OLED initialization failed!"));
    while (1) {
      delay(500);
      Serial.println("OLED error, check hardware connections and address!");
    }
  }
  display.clearDisplay();
  display.setTextSize(1); // Cỡ chữ 
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0); 
  display.println(F("Initializing...")); 
  display.display(); // Hiển thị nội dung lên màn hình
  disableCore0WDT(); 
  if (!pox.begin()) { // Khởi tạo cảm biến
    Serial.println("MAX30100 sensor failed!");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F("MAX30100 ERROR!"));
    display.display();
    while (1) {
      delay(500);
      Serial.println("MAX30100 error, check sensor connections!");
    }
  }
  enableCore0WDT();
  pox.setIRLedCurrent(MAX30100_LED_CURR_24MA);// Dòng điện của LED là 24mA
  pox.setOnBeatDetectedCallback(onBeatDetected);

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(F("MAX30100 Ready"));
  display.display();
  Serial.println("Setup Complete. Waiting for finger...");
}

void loop() {
  pox.update(); // Cập nhật dữ liệu từ cảm biến

  if (isBeeping && (millis() - beatTime > beatDuration)) {
    noTone(buzzer);
    isBeeping = false;
  }
  if ((millis() - tLastReport) > REPORTING_PERIOD_MS) {
    tLastReport = millis(); // Cập nhật thời điểm báo cáo cuối cùng

    float heartRate = pox.getHeartRate(); // Lấy nhịp tim
    float spO2 = pox.getSpO2();           // Lấy SpO2
    checkBrightLight(heartRate, spO2);
    display.clearDisplay(); // Xóa màn hình để vẽ lại
    display.setCursor(0, 0); // Đặt con trỏ về góc trên bên trái
    
    if (brightLightDetected) {
      // Hiển thị cảnh báo ánh sáng mạnh trên OLED
      display.setTextSize(1);
      display.println(F("WARNING:"));
      display.println(F("Bright light detected!"));
      display.println(F("Cover sensor properly"));
      Serial.println("Bright light interference detected! Cover sensor.");
    }
    else if (spO2 > 0 && heartRate > 0 && isDataReliable(heartRate, spO2)) {
      // Nếu dữ liệu hợp lệ, hiển thị lên Serial Monitor
      Serial.print("Heart rate: ");
      Serial.print(heartRate);
      Serial.println(" bpm");
      Serial.print("SpO2: ");
      Serial.print(spO2);
      Serial.println(" %");
      // Hiển thị dữ liệu lên màn hình OLED
      display.setTextSize(1);
      display.println(F("Heart Rate Monitor"));
      display.setTextSize(2); // Cỡ chữ lớn hơn cho số
      display.setCursor(0, 16); // Đặt con trỏ ở hàng thứ hai
      display.print(heartRate, 0); // Hiển thị nhịp tim
      display.println(F(" bpm"));
      display.print(spO2, 0); // Hiển thị SpO2
      display.println(F(" % SpO2"));
    } else {
      // Nếu không có ngón tay hoặc dữ liệu không hợp lệ, hiển thị hướng dẫn
      Serial.println("No finger or unreliable data. Please place finger on sensor.");
      display.setTextSize(1);
      int16_t x1, y1;
      uint16_t w, h;
      display.getTextBounds(F("Place finger"), 0, 0, &x1, &y1, &w, &h);
      display.setCursor((SCREEN_WIDTH - w) / 2, 15);
      display.println(F("Place finger"));
      display.getTextBounds(F("on sensor"), 0, 0, &x1, &y1, &w, &h);
      display.setCursor((SCREEN_WIDTH - w) / 2, 40);
      display.println(F("on sensor"));
    }
    display.display(); // Cập nhật màn hình OLED
  }
}
