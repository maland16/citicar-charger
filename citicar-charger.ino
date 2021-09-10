#include <rpb-1600-commands.h> // Charger commands
#include <rpb-1600.h>          // Charger
#include <daly-bms-uart.h>     // BMS
#include <ILI9341_t3.h>        // Display library
#include <font_Arial.h>        // from ILI9341_t3
#include <XPT2046_Touchscreen.h>
#include <SPI.h> // For talking to the display/touchscreen

// These developer options disable the charger/BMS query operations to make
// it easier to develop without the charger or BMS physically present
#define DEV_MODE_NO_CHARGER true
#define DEV_MODE_NO_BMS true

#define MIN_PACK_TEMP_C 5 // We don't want to try to charge the batteries if the pack is too cold
#define FULLY_CHARGED_VOLTAGE 58.5

// Pin definitions
#define T_IRQ 2
#define CS_PIN 8
#define TFT_DC 9
#define TFT_CS 10

// The data from the touchscreen needs to be scaled to be checked against the positions on the tft.
// These values define the bounds of the touchscreen
#define TS_MINX 150
#define TS_MINY 130
#define TS_MAXX 3800
#define TS_MAXY 4000

// Private types
enum states
{
  STARTUP = 0,
  CHARGING,
  FULLY_CHARGED,
  PACK_TEMP_TOO_LOW,
  BMS_ERR,
  CHARGER_ERR,
  CONFIGURATION
};

enum charger_state
{
  CC = 0,
  CV,
  CHARGED
};

enum charge_speed
{
  SLOW = 0,
  FAST
};

enum buttons
{
  SETTINGS = 0
};

struct button_bounds
{
  uint8_t x;
  uint8_t y;
  uint8_t width;
  uint8_t height;
};

// Helper function prototypes
void initDisplay(void);
void initTouchscreen(void);
bool queryBMS(void);
bool queryCharger(void);

void transitionToBMSError(void);
void transitionToChargerError(void);
void transitionToCharging(void);
void transitionToConfiguration(void);
bool buttonPressed(button_bounds bounds);

// Private member object declarations
XPT2046_Touchscreen ts(CS_PIN, T_IRQ);
ILI9341_t3 tft = ILI9341_t3(TFT_CS, TFT_DC);
RPB_1600 charger;
Daly_BMS_UART bms(Serial1);

// Private member variables
uint8_t my_state = STARTUP;
// Data from BMS
float pack_voltage = 0;
float bms_current = 0;
float SOC = 0;
int8_t pack_temp = 26; // Default pack temp to room temp
// Data from charger
float charger_vout = 0;
uint16_t charger_vin = 0;
uint16_t charger_iout = 0;
uint8_t charger_state = CC;
uint8_t charging_speed = SLOW;
uint8_t charging_limit_percentage = 60;

// Button boundaries. Used to draw buttons and check if touches are within the button bounds
button_bounds settings_button{14, 165, 150, 60};

button_bounds limit_sixty_button{30, 60, 100, 50};
button_bounds limit_eighty_button{30, 110, 100, 50};
button_bounds limit_ninety_button{30, 160, 100, 50};

button_bounds speed_slow_button{170, 60, 100, 50};
button_bounds speed_fast_button{170, 110, 100, 50};
button_bounds exit_config_button{160, 175, 120, 50};

void setup()
{
  // Start up the serial communication
  Serial.begin(115200);
  Serial.printf("<Citicar-charger DEBUG> Starting initialization...\n");

  // Initialize the peripheral devices
  initDisplay(); // Display first, so we can see something
  initTouchscreen();
  charger.Init(0x47); // 0x47 = i2c address of charger, set using pins A0, A1, A2
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
      transitionToBMSError();
      break;
    }

    if (!queryCharger())
    {
      transitionToChargerError();
      break;
    }

    if (pack_temp <= MIN_PACK_TEMP_C)
    {
      // Too cold, we shouldn't start up the charger
      my_state = PACK_TEMP_TOO_LOW;
      break;
    }

    if (pack_voltage >= FULLY_CHARGED_VOLTAGE)
    {
      // We're already charged! No point in starting the charger...
      my_state = FULLY_CHARGED;
      break;
    }

    transitionToCharging();
    break;

    break;
  }
  case (CHARGING):
  {
    // Query the BMS & Charger, jump states if those queries fail
    if (!queryBMS())
    {
      transitionToBMSError();
      break;
    }

    if (!queryCharger())
    {
      transitionToChargerError();
      break;
    }

    if (ts.touched() && buttonPressed(settings_button))
    {
      transitionToConfiguration();
      break;
    }

    // Charger info
    tft.fillRect(0, 80, 49, 14, ILI9341_BLACK);    // Clear old vin
    tft.fillRect(127, 80, 49, 14, ILI9341_BLACK);  // Clear old vout
    tft.fillRect(127, 100, 49, 14, ILI9341_BLACK); // Clear old iout
    tft.setTextColor(ILI9341_WHITE);
    tft.setFont(Arial_12);
    tft.setCursor(5, 80);
    tft.printf("%dV", charger_vin);
    tft.setCursor(127, 80);
    tft.printf("%4.2fV", charger_vout);
    tft.setCursor(127, 103);
    tft.printf("%dA", charger_iout);

    tft.fillRect(55, 90, 30, 25, ILI9341_BLACK); // Clear old mode
    tft.setCursor(55, 90);
    tft.setFont(Arial_20);
    if (charger_state == CC)
    {
      tft.print("CC");
    }
    else if (charger_state == CV)
    {
      tft.print("CV");
    }

    // Battery Info
    tft.fillRect(215, 90, 55, 65, ILI9341_BLACK); // Clear old BMS values
    tft.setFont(Arial_16);
    tft.setCursor(215, 90);
    tft.printf("%4.2fV", pack_voltage);
    tft.setCursor(215, 110);
    tft.printf("%4.2f%", SOC);
    tft.setCursor(215, 130);
    tft.printf("%4.2fC", pack_temp);
    break;
  }
  case (FULLY_CHARGED):
  {
    tft.fillScreen(ILI9341_GREEN);
    break;
  }
  case (PACK_TEMP_TOO_LOW):
  {
    // Spin and wait for the pack to warm up I guess...
    if (!queryBMS())
    {
      transitionToBMSError();
      break;
    }

    if (pack_voltage > MIN_PACK_TEMP_C)
    {
      my_state = STARTUP;
    }
    break;
  }
  case (BMS_ERR):
  {
    Serial.printf("<Citicar-charger DEBUG> Attempting to query BMS...\n");

    if (queryBMS())
    {
      // We were able to communicate!
      Serial.printf("<Citicar-charger DEBUG> Query BMS successful! Restarting...\n");
      my_state = STARTUP;
    }
    break;
  }
  case (CHARGER_ERR):
  {
    Serial.printf("<Citicar-charger DEBUG> Attempting to query charger...\n");

    if (queryCharger())
    {
      // We were able to communicate!
      Serial.printf("<Citicar-charger DEBUG> Query Charger successful! Restarting...\n");
      my_state = STARTUP;
    }
    break;
  }
  case (CONFIGURATION):
  {
    if (ts.touched())
    {
      if (buttonPressed(limit_sixty_button))
      {
        charging_limit_percentage = 60;
      }
      else if (buttonPressed(limit_eighty_button))
      {
        charging_limit_percentage = 80;
      }
      else if (buttonPressed(limit_ninety_button))
      {
        charging_limit_percentage = 90;
      }
      else if (buttonPressed(speed_slow_button))
      {
        charging_speed = SLOW;
      }
      else if (buttonPressed(speed_fast_button))
      {
        charging_speed = FAST;
      }
      else if (buttonPressed(exit_config_button))
      {
        transitionToCharging();
        break;
      }

      transitionToConfiguration();
    }
    break;
  }
  default:
    break;
  }
  delay(250);
}

//-------------------------------------------------
// Helper Function Definitions
//-------------------------------------------------

/**
 * @brief Initialize the tft display, and print an init message
 */
void initDisplay(void)
{
  tft.begin();
  tft.setRotation(1);

  // Clear the screen and print some text about starting up
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_GREEN);
  tft.setFont(Arial_24);
  tft.setCursor(70, 100);
  tft.print("Initializing...");
}

/**
 * @brief Initialize the touchscreen
 */
void initTouchscreen(void)
{
  ts.begin();
  // I'm not sure if this rotation is correct, but I'm in too deep to change it and face the ramifications
  ts.setRotation(1);
}

/**
 * @brief Queries the data from the Battery Management System
 */
bool queryBMS(void)
{
  if (DEV_MODE_NO_BMS)
  {
    return true;
  }

  return bms.getPackMeasurements(pack_voltage, bms_current, SOC) && bms.getPackTemp(pack_temp);
}

/**
 * @brief Queries the data from the charger
 */
bool queryCharger(void)
{
  if (DEV_MODE_NO_CHARGER)
  {
    return true;
  }

  readings charger_readings{};
  charge_status charger_status{};

  if (charger.getReadings(&charger_readings) && charger.getChargeStatus(&charger_status))
  {
    charger_vout = charger_readings.v_out;
    charger_vin = charger_readings.v_in;
    charger_iout = charger_readings.i_out;

    if (charger_status.in_cc_mode)
    {
      charger_state = CC;
    }
    else if (charger_status.in_cv_mode)
    {
      charger_state = CV;
    }
    else if (charger_status.fully_charged)
    {
      charger_state = CHARGED;
    }

    // Data collected successfully!
    return true;
  }
  else
  {
    // At least one of our querys failed, return error
    return false;
  }
}

/**
 * @brief Transitions to the BMS Error state
 * @details Changes states & prints a message about the error
 */
void transitionToBMSError(void)
{
  // Change the state
  my_state = BMS_ERR;

  // Print an error message
  tft.fillRect(30, 50, 260, 80, ILI9341_RED);
  tft.drawRect(30, 50, 260, 80, ILI9341_WHITE);
  tft.setTextColor(ILI9341_WHITE);
  tft.setFont(Arial_18);
  tft.setCursor(40, 65);
  tft.print("Error communicating");
  tft.setCursor(40, 95);
  tft.print("with BMS! Retrying...");
}

/**
 * @brief Transitions to the charger error state
 * @details Changes states & prints a message about the error
 */
void transitionToChargerError(void)
{
  // Change the state
  my_state = CHARGER_ERR;

  // Print an error message
  tft.fillRect(20, 50, 285, 80, ILI9341_RED);
  tft.drawRect(20, 50, 285, 80, ILI9341_WHITE);
  tft.setTextColor(ILI9341_WHITE);
  tft.setFont(Arial_18);
  tft.setCursor(40, 65);
  tft.print("Error communicating");
  tft.setCursor(25, 95);
  tft.print("with Charger! Retrying...");
}

/**
 * @brief Transition to the charing state
 * @details clears the screen and prints all the pretty stuff on the "Charging" display
 */
void transitionToCharging(void)
{
  my_state = CHARGING;

  tft.fillScreen(ILI9341_BLACK);
  tft.setFont(Arial_24);
  tft.setTextColor(ILI9341_GREEN);

  tft.setCursor(70, 10);
  tft.print("CHARGING");

  // Charger diagram
  tft.drawRect(0, 95, 240, 5, ILI9341_WHITE);  // Wire
  tft.fillRect(50, 65, 75, 55, ILI9341_BLACK); // Charger
  tft.drawRect(50, 65, 75, 55, ILI9341_WHITE); // Charger
  tft.drawRect(50, 85, 75, 35, ILI9341_WHITE); // Charger
  tft.setFont(Arial_14);
  tft.setCursor(52, 67);
  tft.setTextColor(ILI9341_WHITE);
  tft.print("Charger");

  // Print battery diagram
  tft.fillRect(210, 60, 100, 100, ILI9341_BLACK);
  tft.drawRect(210, 60, 100, 100, ILI9341_WHITE);
  tft.drawRect(210, 85, 100, 75, ILI9341_WHITE);
  tft.setFont(Arial_16);
  tft.setCursor(215, 63);
  tft.setTextColor(ILI9341_WHITE);
  tft.print("Battery");

  /*
  tft.setCursor(185, 90);
  tft.print("V:");
  tft.setCursor(185, 110);
  tft.print("SOC:");
  tft.setCursor(185, 130);
  tft.print("Temp:"); */

  // Draw settings button
  tft.fillRect(settings_button.x, settings_button.y, settings_button.width, settings_button.height, ILI9341_YELLOW);
  tft.drawRect(settings_button.x, settings_button.y, settings_button.width, settings_button.height, ILI9341_RED);
  tft.setFont(Arial_24);
  tft.setTextColor(ILI9341_RED);
  tft.setCursor(30, 180);
  tft.print("Settings");
}

/**
 * @brief Transition to the configuration state
 */
void transitionToConfiguration(void)
{

  // Pull the charging curve configuration from the charger

  // Wipe the screen if we aren't coming from the configuration state
  if (my_state != CONFIGURATION)
  {
    my_state = CONFIGURATION;
    tft.fillScreen(ILI9341_BLACK);
  }

  tft.setTextColor(ILI9341_WHITE);
  tft.setFont(Arial_18);
  tft.setCursor(60, 20);
  tft.print("limit           speed");

  // *** Charging limit buttons *** //

  uint16_t limit_sixty_color = ILI9341_YELLOW, limit_eighty_color = ILI9341_YELLOW, limit_ninety_color = ILI9341_YELLOW;

  // Color the button that corresponds to the configured charging limit
  if (charging_limit_percentage == 60)
  {
    limit_sixty_color = ILI9341_GREEN;
  }
  else if (charging_limit_percentage == 80)
  {
    limit_eighty_color = ILI9341_GREEN;
  }
  else if (charging_limit_percentage == 90)
  {
    limit_ninety_color = ILI9341_GREEN;
  }

  tft.fillRect(limit_sixty_button.x, limit_sixty_button.y, limit_sixty_button.width, limit_sixty_button.height, limit_sixty_color);
  tft.fillRect(limit_eighty_button.x, limit_eighty_button.y, limit_eighty_button.width, limit_eighty_button.height, limit_eighty_color);
  tft.fillRect(limit_ninety_button.x, limit_ninety_button.y, limit_ninety_button.width, limit_ninety_button.height, limit_ninety_color);

  tft.drawRect(limit_sixty_button.x, limit_sixty_button.y, limit_sixty_button.width, limit_sixty_button.height, ILI9341_BLUE);
  tft.drawRect(limit_eighty_button.x, limit_eighty_button.y, limit_eighty_button.width, limit_eighty_button.height, ILI9341_BLUE);
  tft.drawRect(limit_ninety_button.x, limit_ninety_button.y, limit_ninety_button.width, limit_ninety_button.height, ILI9341_BLUE);

  tft.setTextColor(ILI9341_BLACK);

  tft.setCursor(60, 75);
  tft.print("60%");
  tft.setCursor(60, 75 + 50);
  tft.print("80%");
  tft.setCursor(60, 75 + 50 + 50);
  tft.print("90%");

  // *** Charging speed buttons *** //

  uint16_t speed_fast_color = ILI9341_YELLOW,
           speed_slow_color = ILI9341_YELLOW;

  // Color the button that corresponds to the configured charging speed
  switch (charging_speed)
  {
  case (SLOW):
    speed_slow_color = ILI9341_GREEN;
    break;
  case (FAST):
    speed_fast_color = ILI9341_GREEN;
    break;
  default:
    break;
  }

  tft.fillRect(speed_slow_button.x, speed_slow_button.y, speed_slow_button.width, speed_slow_button.height, speed_slow_color);
  tft.fillRect(speed_fast_button.x, speed_fast_button.y, speed_fast_button.width, speed_fast_button.height, speed_fast_color);

  tft.drawRect(speed_slow_button.x, speed_slow_button.y, speed_slow_button.width, speed_slow_button.height, ILI9341_BLUE);
  tft.drawRect(speed_fast_button.x, speed_fast_button.y, speed_fast_button.width, speed_fast_button.height, ILI9341_BLUE);

  tft.setTextColor(ILI9341_BLACK);

  tft.setCursor(195, 75);
  tft.print("slow");
  tft.setCursor(200, 75 + 50);
  tft.print("fast");

  // *** Exit button *** //

  tft.fillRect(exit_config_button.x, exit_config_button.y, exit_config_button.width, exit_config_button.height, ILI9341_ORANGE);
  tft.drawRect(exit_config_button.x, exit_config_button.y, exit_config_button.width, exit_config_button.height, ILI9341_WHITE);

  tft.setTextColor(ILI9341_BLACK);

  tft.setFont(Arial_20);
  tft.setCursor(185, 185);
  tft.print("EXIT");
}

/**
 * @brief Helper for detecting button presses
 * @returns true if the last touch was within the bounds of the given button, false otherwise
 */
bool buttonPressed(button_bounds bounds)
{
  TS_Point p = ts.getPoint(); // Pull the location of the touch from the touchscreen

  // Map the point given from the range of touchscreen values to the range of tft pixels
  // Note the last two arguments, these are in this order to account for the fact that
  // the touchscreen x & y and tft x & y axis are reversed, for some reason.
  p.x = map(p.x, TS_MINX, TS_MAXX, tft.width(), 0);
  p.y = map(p.y, TS_MINY, TS_MAXY, tft.height(), 0);

  // Check the location of the touch against the bounds of the button
  return (p.x > bounds.x) && (p.x < bounds.x + bounds.width) && (p.y > bounds.y) && (p.y < bounds.y + bounds.height);
}