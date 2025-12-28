/*
 * Teensy 4.1 High-Precision Frequency Generator
 * * Hardware:
 * - ILI9341 2.4" TFT (SPI)
 * - 2x Rotary Encoders
 * - 3x Pushbuttons
 * * Features:
 * - 1 Hz to ~20 MHz Output Range (Square Wave)
 * - Variable Duty Cycle
 * - Dynamic Step Sizing (1Hz, 10Hz, 100Hz... 1MHz)
 * - Output Enable/Disable toggle
 * - Non-blocking UI updates
 */

#include <SPI.h>
#include <ILI9341_t3.h> // Optimized library for Teensy
#include <Encoder.h>
#include <Bounce2.h>
#include <font_ArialBold14.h> // Built-in Teensy fonts
#include <font_Arial24.h>

// --- Pin Definitions ---
#define TFT_CS      10
#define TFT_DC      9
#define TFT_RST     8
#define PIN_ENC1_A  2
#define PIN_ENC1_B  3
#define PIN_ENC2_A  4
#define PIN_ENC2_B  5
#define PIN_BTN1    6   // Toggle Output
#define PIN_BTN2    7   // Reset / Preset
#define PIN_BTN3    14  // Toggle Mode (Step Size vs Duty Cycle)
#define PIN_OUT     22  // PWM Output

// --- Objects ---
ILI9341_t3 tft = ILI9341_t3(TFT_CS, TFT_DC, TFT_RST);
Encoder encFreq(PIN_ENC1_A, PIN_ENC1_B);
Encoder encParam(PIN_ENC2_A, PIN_ENC2_B);
Bounce btnToggle = Bounce();
Bounce btnReset = Bounce();
Bounce btnMode = Bounce();

// --- System State ---
volatile double frequency = 1000.0; // Starting at 1kHz
int dutyCycle = 128;       // 0-255 (128 = 50%)
long stepSizes[] = {1, 10, 100, 1000, 10000, 100000, 1000000};
int stepIndex = 2;         // Start at 100Hz steps
bool outputEnabled = true;
bool paramModeDuty = false; // False = Step Control, True = Duty Control

// Tracking variables for loop efficiency
long oldPosFreq = -999;
long oldPosParam = -999;
double lastDrawFreq = 0;
int lastDrawDuty = 0;
long lastDrawStep = 0;
bool lastDrawState = false;

void setup() {
  Serial.begin(9600);

  // -- Input Setup --
  pinMode(PIN_BTN1, INPUT_PULLUP);
  pinMode(PIN_BTN2, INPUT_PULLUP);
  pinMode(PIN_BTN3, INPUT_PULLUP);
  
  btnToggle.attach(PIN_BTN1); btnToggle.interval(15);
  btnReset.attach(PIN_BTN2);  btnReset.interval(15);
  btnMode.attach(PIN_BTN3);   btnMode.interval(15);

  // -- Output Setup --
  pinMode(PIN_OUT, OUTPUT);
  // Set initial signal
  analogWriteFrequency(PIN_OUT, frequency);
  analogWrite(PIN_OUT, dutyCycle);

  // -- Display Setup --
  tft.begin();
  tft.setRotation(3); // Landscape
  tft.fillScreen(ILI9341_BLACK);
  
  // -- Draw Static UI Elements --
  drawInterface();
}

void loop() {
  // 1. Handle Inputs
  btnToggle.update();
  btnReset.update();
  btnMode.update();
  
  long newPosFreq = encFreq.read();
  long newPosParam = encParam.read();

  // 2. Logic: Frequency Encoder (Enc 1)
  // Div by 4 handles standard encoder detents
  if (newPosFreq / 4 != oldPosFreq / 4) {
    long diff = (newPosFreq / 4) - (oldPosFreq / 4);
    oldPosFreq = newPosFreq;
    
    frequency += diff * stepSizes[stepIndex];
    
    // Limits
    if (frequency < 1.0) frequency = 1.0;
    if (frequency > 20000000.0) frequency = 20000000.0; // 20MHz Cap
    
    updateHardware();
  }

  // 3. Logic: Parameter Encoder (Enc 2)
  if (newPosParam / 4 != oldPosParam / 4) {
    long diff = (newPosParam / 4) - (oldPosParam / 4);
    oldPosParam = newPosParam;

    if (paramModeDuty) {
      // Change Duty Cycle
      dutyCycle += diff * 5; // 5 steps at a time
      if (dutyCycle < 0) dutyCycle = 0;
      if (dutyCycle > 255) dutyCycle = 255;
      updateHardware();
    } else {
      // Change Step Size
      stepIndex += diff;
      if (stepIndex < 0) stepIndex = 0;
      if (stepIndex > 6) stepIndex = 6;
    }
  }

  // 4. Logic: Buttons
  if (btnToggle.fell()) {
    outputEnabled = !outputEnabled;
    updateHardware();
  }
  
  if (btnReset.fell()) {
    frequency = 1000.0;
    dutyCycle = 128;
    stepIndex = 2;
    updateHardware();
  }
  
  if (btnMode.fell()) {
    paramModeDuty = !paramModeDuty; // Toggle between Duty and Step control
    // Redraw mode label immediately
    drawModeLabel(); 
  }

  // 5. Update Screen (Only if values changed)
  refreshValues();
}

// --- Hardware Control Wrapper ---
void updateHardware() {
  if (outputEnabled) {
    analogWriteFrequency(PIN_OUT, frequency);
    analogWrite(PIN_OUT, dutyCycle);
  } else {
    digitalWrite(PIN_OUT, LOW);
  }
}

// --- UI Drawing Functions ---

void drawInterface() {
  // Static Frames
  tft.drawRect(10, 10, 300, 80, ILI9341_WHITE); // Freq Box
  tft.drawRect(10, 100, 145, 60, ILI9341_WHITE); // Step/Duty Box
  tft.drawRect(165, 100, 145, 60, ILI9341_WHITE); // State Box
  
  tft.setTextColor(ILI9341_GREEN);
  tft.setFont(Arial_14_Bold);
  tft.setCursor(20, 20); tft.print("FREQUENCY (Hz)");
  
  tft.setCursor(175, 110); tft.print("OUTPUT");
  
  drawModeLabel();
}

void drawModeLabel() {
  // Clears the label area before writing new label
  tft.fillRect(15, 105, 130, 20, ILI9341_BLACK); 
  tft.setTextColor(ILI9341_CYAN);
  tft.setFont(Arial_14_Bold);
  tft.setCursor(20, 110);
  if (paramModeDuty) tft.print("DUTY CYCLE");
  else tft.print("STEP SIZE");
  
  // Force value refresh to redraw the number below the label
  lastDrawStep = -1; 
  lastDrawDuty = -1;
}

void refreshValues() {
  // Redraw Frequency
  if (frequency != lastDrawFreq) {
    tft.fillRect(20, 45, 280, 40, ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE);
    tft.setFont(Arial_24);
    tft.setCursor(25, 50);
    tft.printf("%.1f", frequency);
    lastDrawFreq = frequency;
  }

  // Redraw Parameter (Step or Duty)
  // We check both conditions because the label might have changed
  if ((!paramModeDuty && stepSizes[stepIndex] != lastDrawStep) || 
      (paramModeDuty && dutyCycle != lastDrawDuty)) {
        
    tft.fillRect(20, 135, 125, 20, ILI9341_BLACK);
    tft.setTextColor(ILI9341_YELLOW);
    tft.setFont(Arial_14_Bold);
    tft.setCursor(25, 135);
    
    if (paramModeDuty) {
      int percent = map(dutyCycle, 0, 255, 0, 100);
      tft.printf("%d %%", percent);
    } else {
      tft.printf("%ld Hz", stepSizes[stepIndex]);
    }
    
    lastDrawStep = stepSizes[stepIndex];
    lastDrawDuty = dutyCycle;
  }

  // Redraw Output State
  if (outputEnabled != lastDrawState) {
    tft.fillRect(175, 135, 120, 20, ILI9341_BLACK);
    if (outputEnabled) {
      tft.setTextColor(ILI9341_GREEN);
      tft.setCursor(185, 135); tft.print("ON");
    } else {
      tft.setTextColor(ILI9341_RED);
      tft.setCursor(185, 135); tft.print("OFF");
    }
    lastDrawState = outputEnabled;
  }
}