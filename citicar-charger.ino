#include <rpb-1600-commands.h> // Charger commands
#include <rpb-1600.h>          // Charger
#include <daly-bms-uart.h>     // BMS
#include <ILI9341_t3.h>        // Display library
#include <font_Arial.h>        // from ILI9341_t3
#include <XPT2046_Touchscreen.h>
#include <SPI.h> // For talking to the display/touchscreen

#define MIN_PACK_TEMP_C 5 // We don't want to try to charge the batteries if the pack is too cold
#define FULLY_CHARGED_VOLTAGE 58.5

// Pin definitions
#define CS_PIN 8
#define TFT_DC 9
#define TFT_CS 10

// Helper function prototypes
void initDisplay(void);
void initTouchscreen(void);
void queryBMS(void);
void queryCharger(void);

// Private types
enum states
{
  STARTUP = 0,
  CHARGING,
  FULLY_CHARGED,
  PACK_TEMP_TOO_LOW,
  ERR
};

// Private member object declarations
XPT2046_Touchscreen ts(CS_PIN);
ILI9341_t3 tft = ILI9341_t3(TFT_CS, TFT_DC);
RPB_1600 charger;
Daly_BMS_UART bms(Serial1);

// Private member variables
uint8_t my_state = STARTUP;
// Data from BMS
float pack_voltage = 0;
float bms_current = 0;
float SOC = 0;
int8_t pack_temp = 0;
// Data from charger
float charger_vout = 0;
uint16_t charger_vin = 0;
uint16_t charger_current = 0;

void setup()
{
  // Start up the serial communication
  Serial.begin(115200);
  Serial.printf("<Citicar-charger DEBUG> Starting initialization...\n");

  // Initialize the peripheral devices
  initDisplay(); // Display first, so we can see something
  initTouchscreen();
  charger.Init(0x47); // 0x47 = i2c address of charger, set using pins A0,A1,A2
  bms.Init();

  Serial.printf("<Citicar-charger DEBUG> Initialization complete!\n");
}

void loop()
{
  switch (my_state)
  {
  case (STARTUP):
  {
    // Pull values from the charger & bms. Make sure we can talk to them
    if (!queryBMS())
    {
      state = ERR;
      break;
    }

    if (!queryCharger())
    {
      state = ERR;
      break;
    }

    if (pack_temp <= MIN_PACK_TEMP_C)
    {
      // Too cold, we shouldn't start up the charger
      state = PACK_TEMP_TOO_LOW;
      break;
    }

    if (pack_voltage >= FULLY_CHARGED_VOLTAGE)
    {
      // We're already charged! No point in starting the charger...
      state = FULLY_CHARGED;
      break;
    }

    break;
  }
  case (CHARGING):
  {
    break;
  }
  case (FULLY_CHARGED):
  {
    break;
  }
  case (PACK_TEMP_TOO_LOW):
  {
    // Spin and wait for the pack to warm up I guess...
    if (!queryBMS())
    {
      state = ERR;
    }

    if (pack_voltage > MIN_PACK_TEMP_C)
    {
      state = STARTUP;
    }
    break;
  }
  case (ERR):
  {
    // Display an error message
    break;
  }
  }
}

void initDisplay(void)
{
  tft.begin();
  tft.setRotation(1);

  // Clear the screen and print some text about starting up
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_GREEN);
  tft.setFont(Arial_24);
  tft.setCursor(100, 150);
  tft.print("Initializing...");
}

void initTouchscreen(void)
{
  ts.begin();
  ts.setRotation(1);
}

// Query the BMS and update all BMS-related variables
bool queryBMS(void)
{
  return (bms.getPackMeasurements(pack_voltage, bms_current, SOC) && bms.getPackTemp(pack_temp));
}

bool queryCharger(void)
{
  return true;
}
