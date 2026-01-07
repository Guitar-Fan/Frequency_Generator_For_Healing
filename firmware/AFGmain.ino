#include <Wire.h>
#include <SPI.h>
#include <Audio.h>
#include <SD.h>
#include <SerialFlash.h>
#include <VL53L1X.h>
#include <ILI9341_t3.h>
#include <XPT2046_Touchscreen.h>

// ==========================================
//              PIN DEFINITIONS
// ==========================================
// Display Pins (Check your specific wiring!)
#define TFT_DC      9
#define TFT_CS      10
#define TFT_RST     8   // Or 255 if connected to 3.3V
#define TFT_MOSI    11
#define TFT_SCK     13
#define TFT_MISO    12
#define TOUCH_CS    6   // Chip select for Touch
#define TOUCH_IRQ   255 // We will use polling, not IRQ

// Button Pins
#define BTN_MODE    4
#define BTN_ACTION  5

// ==========================================
//           AUDIO SYSTEM SETUP
// ==========================================
// Audio Design Tool Graph:
// Waveform -> Mixer -> I2S Output
AudioSynthWaveform       waveform1; 
AudioMixer4              mixer1; 
AudioOutputI2S           i2s1; 
AudioConnection          patchCord1(waveform1, 0, mixer1, 0);
AudioConnection          patchCord2(mixer1, 0, i2s1, 0);
AudioConnection          patchCord3(mixer1, 0, i2s1, 1); // Stereo output

// Array for "Custom Drawn" Waveform (256 samples is standard)
short waveTable[257]; 

// ==========================================
//           SENSOR & HARDWARE SETUP
// ==========================================
VL53L1X sensorPitch;
VL53L1X sensorVol;

// Display & Touch Objects
ILI9341_t3 tft = ILI9341_t3(TFT_CS, TFT_DC, TFT_RST, TFT_MOSI, TFT_SCK, TFT_MISO);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// Logic Variables
int currentMode = 0; // 0 = Theremin, 1 = Draw Wave
float smoothedPitch = 440.0;
float smoothedVol = 0.0;
unsigned long lastVizUpdate = 0;

// ==========================================
//                SETUP
// ==========================================
void setup() {
  Serial.begin(9600);
  
  // 1. Setup Audio Memory
  AudioMemory(20);
  
  // 2. Setup Audio Waveform Defaults
  waveform1.begin(WAVEFORM_SINE);
  waveform1.frequency(440);
  waveform1.amplitude(1.0); // We control volume via Mixer, not Waveform amp
  
  // 3. Setup Buttons
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BTN_ACTION, INPUT_PULLUP);

  // 4. Setup Display
  tft.begin();
  tft.setRotation(3); // Landscape
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.print("System Booting...");
  
  ts.begin();
  ts.setRotation(3);

  // 5. Setup Sensors (THE DUAL I2C TRICK)
  
  // --- Pitch Sensor on Wire (Pins 18/19) ---
  Wire.begin();
  Wire.setClock(400000); // 400kHz I2C
  sensorPitch.setBus(&Wire);
  sensorPitch.setTimeout(500);
  if (!sensorPitch.init()) {
    tft.setCursor(10, 30); tft.print("Pitch Sensor Fail!");
  }
  sensorPitch.setDistanceMode(VL53L1X::Medium);
  sensorPitch.setMeasurementTimingBudget(20000); // 20ms fast response
  sensorPitch.startContinuous(20);

  // --- Volume Sensor on Wire1 (Pins 16/17) ---
  Wire1.begin();
  Wire1.setClock(400000);
  sensorVol.setBus(&Wire1);
  sensorVol.setTimeout(500);
  if (!sensorVol.init()) {
    tft.setCursor(10, 50); tft.print("Vol Sensor Fail!");
  }
  sensorVol.setDistanceMode(VL53L1X::Medium);
  sensorVol.setMeasurementTimingBudget(20000);
  sensorVol.startContinuous(20);

  // Initialize Custom Waveform array to a flat line
  for (int i=0; i<257; i++) {
    waveTable[i] = 0;
  }
  
  delay(1000);
  drawInterface();
}


void loop() {
  
  // --- Button Handling ---
  if (digitalRead(BTN_MODE) == LOW) {
    currentMode++;
    if (currentMode > 1) currentMode = 0;
    drawInterface();
    delay(300); // Debounce
  }

  // --- Mode 0: THEREMIN PERFORMANCE ---
  if (currentMode == 0) {
    runTheremin();
  }
  // --- Mode 1: DRAW WAVEFORM ---
  else if (currentMode == 1) {
    runDrawer();
  }
}

// ==========================================
//           THEREMIN LOGIC
// ==========================================
void runTheremin() {
  // 1. Read Sensors
  sensorPitch.read();
  sensorVol.read();
  
  int dPitch = sensorPitch.ranging_data.range_mm;
  int dVol = sensorVol.ranging_data.range_mm;

  // 2. Map Pitch (Distance -> Hz)
  // Logic: 50mm = 100Hz, 500mm = 1000Hz. 
  // If > 600mm, hold last note or ignore.
  float targetFreq = 0;
  if (dPitch < 600 && dPitch > 30) {
    targetFreq = map(dPitch, 30, 500, 100, 1000);
  } else {
    targetFreq = smoothedPitch; // Hold steady
  }

  // 3. Map Volume (Distance -> Gain 0.0 to 1.0)
  // Logic: Close = Quiet, Far = Loud (Traditional Theremin style)
  // OR: Close = Loud, Far = Quiet (Often easier for beginners) -> Let's do Close=Loud
  float targetVol = 0;
  if (dVol < 400 && dVol > 30) {
    targetVol = map((float)dVol, 30.0, 400.0, 1.0, 0.0);
    // Clamp values
    if (targetVol < 0) targetVol = 0;
    if (targetVol > 1) targetVol = 1;
  } else {
    targetVol = 0; // Silence if hand is gone
  }

  // 4. Smoothing (Low Pass Filter)
  // This removes the "jitter" from the sensors
  smoothedPitch = 0.8 * smoothedPitch + 0.2 * targetFreq;
  smoothedVol = 0.8 * smoothedVol + 0.2 * targetVol;

  // 5. Apply to Audio Engine
  waveform1.frequency(smoothedPitch);
  mixer1.gain(0, smoothedVol);

  // 6. Update Screen (Slowly, 10fps)
  if (millis() - lastVizUpdate > 100) {
    tft.fillRect(60, 100, 200, 40, ILI9341_BLACK); // Clear numbers
    tft.setCursor(60, 100);
    tft.print((int)smoothedPitch); tft.print(" Hz");
    tft.setCursor(60, 120);
    tft.print((int)(smoothedVol*100)); tft.print(" %");
    
    // Draw Bar Graph
    int barH = smoothedVol * 100;
    tft.fillRect(280, 200 - barH, 20, barH, ILI9341_GREEN);
    tft.fillRect(280, 50, 20, 150 - barH, ILI9341_BLACK); // Clear top
    
    lastVizUpdate = millis();
  }
}

// ==========================================
//           WAVE DRAW LOGIC
// ==========================================
void runDrawer() {
  // If "Action" button is pressed, play the drawn wave
  if (digitalRead(BTN_ACTION) == LOW) {
    waveform1.begin(WAVEFORM_ARBITRARY);
    waveform1.arbitraryWaveform(waveTable, 10000); // Apply array
    // Play a test tone
    waveform1.frequency(220);
    mixer1.gain(0, 0.8);
    return; // Skip drawing while playing
  } else {
    // Silence while drawing
    mixer1.gain(0, 0); 
  }

  // Handle Touch
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    
    // Map Touch Coordinates to Screen/Array
    // Note: Touch mapping often needs calibration numbers (min/max)
    // Adjust these 200/3800 values based on your specific screen calibration!
    int x = map(p.x, 200, 3800, 0, 320); 
    int y = map(p.y, 200, 3800, 0, 240);

    // If touch is inside the drawing box
    if (x > 10 && x < 266 && y > 50 && y < 200) {
      int index = x - 10; // 0 to 255
      
      // Map Y (height) to Audio Sample (-32000 to +32000)
      // Screen Y 50 is Top (+32000), Y 200 is Bottom (-32000)
      int sampleVal = map(y, 50, 200, 30000, -30000);
      
      waveTable[index] = sampleVal;
      
      // Draw pixel
      tft.drawPixel(x, y, ILI9341_YELLOW);
      // Erase pixels above/below to clean up
      tft.drawFastVLine(x, 50, y-50, ILI9341_BLACK);
      tft.drawFastVLine(x, y+1, 200-y, ILI9341_BLACK);
    }
  }
}

// ==========================================
//               UI HELPER
// ==========================================
void drawInterface() {
  tft.fillScreen(ILI9341_BLACK);
  
  if (currentMode == 0) {
    tft.setCursor(10, 10);
    tft.setTextColor(ILI9341_GREEN);
    tft.print("MODE: THEREMIN");
    
    tft.drawRect(50, 80, 220, 80, ILI9341_WHITE);
    tft.setCursor(60, 200);
    tft.setTextColor(ILI9341_WHITE);
    tft.print("Pitch: Left | Vol: Right");
  } 
  else {
    tft.setCursor(10, 10);
    tft.setTextColor(ILI9341_YELLOW);
    tft.print("MODE: DRAW WAVE");
    
    // Draw Box
    tft.drawRect(9, 49, 258, 152, ILI9341_BLUE);
    tft.setCursor(10, 210);
    tft.print("Touch to draw.");
    tft.setCursor(10, 225);
    tft.print("Hold BTN2 to play.");
    
    // Set standard wave to arbitrary so it's ready
    waveform1.begin(WAVEFORM_ARBITRARY);
  }
}