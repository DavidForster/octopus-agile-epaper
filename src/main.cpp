#include <Arduino.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>

// Pin definitions for ESP32 SPI connection
#define EPD_CS    5
#define EPD_DC    17
#define EPD_RST   16
#define EPD_BUSY  4
#define BUTTON_PIN 0  // BOOT button on most ESP32 boards

// Initialize display (296x128, 2.9" Waveshare)
// Try base driver (works for some V2 displays)
GxEPD2_BW<GxEPD2_290, GxEPD2_290::HEIGHT> display(GxEPD2_290(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

int updateCount = 0;
bool lastButtonState = HIGH;

void updateDisplay() {
  Serial.println("Updating display...");

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);

    // Draw test content
    display.setCursor(10, 30);
    display.println("Waveshare 2.9\"");
    display.setCursor(10, 50);
    display.println("e-Paper V2");
    display.setCursor(10, 70);
    display.print("Updates: ");
    display.println(updateCount);

    // Draw a rectangle
    display.drawRect(10, 80, 276, 40, GxEPD_BLACK);
    display.setCursor(20, 105);
    display.println("Press BOOT!");
  } while (display.nextPage());

  Serial.println("Display update complete!");
  display.hibernate();
}

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 e-Paper V2 Test Started!");
  Serial.println("Press BOOT button to update display");

  // Setup button pin
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Initialize the display
  display.init(115200);
  display.setRotation(1);

  // Initial display update
  updateDisplay();
  updateCount++;
}

void loop() {
  bool buttonState = digitalRead(BUTTON_PIN);

  // Detect button press (LOW = pressed on BOOT button)
  if (buttonState == LOW && lastButtonState == HIGH) {
    Serial.println("Button pressed!");
    delay(50); // Debounce
    updateDisplay();
    updateCount++;
    delay(200); // Prevent multiple triggers
  }

  lastButtonState = buttonState;
  delay(10);
}