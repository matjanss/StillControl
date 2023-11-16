#include <Arduino.h>
#include <EEPROM.h>
#include <ItemProgress.h>
#include <ItemList.h>
#include <ItemCommand.h>
#include <LcdMenu.h>
#include <LiquidCrystal_PCF8574.h>
#include <SimpleRotary.h>
#include <TimerOne.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <DFRobot_BMP3XX.h>
#include <T2ABV.h>

#define USE_ANALOG_POT  // uncomment when using an analog pot for power level setting. rotary encoder will only be used for menu.

#define ROT_PIN_A 4
#define ROT_PIN_B 5
#define ROT_SW 6
#define ONE_WIRE_BUS 2
#define SSR_PIN 10       // timer1 is limited to pins 9 & 10 on UNO and relatives
#define POT_PIN A0       // Analog pot
#define BUZZER_PIN 9
#define ERRORLED_PIN 3

#define LCD_ROWS 4
#define LCD_COLS 20


float tempTop = 0.0;    // temp vapor
float tempStill = 0.0;  // temp Still/wash
float tempProd = 0.0;   // temp of bottom product condensor (= temp of product. -ish)
float tempWater = 0.0;  // temp of cooling water exit
float tempCase = 0.0 ;  // temp of case (from bmp390)
int tempCaseMax = 70;   // max case internal temp (can't change in menu, hardcoded) - could be defined to save ram
float pressure = 0.0;   // atm pressure from bmp390

bool menuActive = false; // keep track of whether the menu is on screen
int dutyCycle = 0;      // holds power level 1-100%
int ssrPeriod = 1;      // ssr PWM period in seconds (NOT Hz)

float vaporABV = 0.0;   //holds vapor abv calc result
float liquidABV = 0.0;  //holds liquid abv calc result


 // function@interval milis timing holders
uint32_t v_UpdateLCD;
uint32_t v_doSetPower;
uint32_t v_doUpdateSensors;
uint32_t v_doClearErrors;

//errors
bool errActive = false;

// Setup rotary encoder with Pin A, Pin B, Button Pin
SimpleRotary rotary(ROT_PIN_A,ROT_PIN_B,ROT_SW);

// eforward declare functions
void callback(uint16_t pos);
void exitMenu();

String action[] = {"None", "Beep", "Shutdown"};

// Define the main menu
MAIN_MENU(
    ITEM_PROGRESS("Warn T Still ", callback),
    ITEM_PROGRESS("Warn T Top   ", callback),
    ITEM_PROGRESS("Warn T Prod  ", callback),
    ITEM_PROGRESS("Warn T Water ", callback),
    ITEM_PROGRESS("Err T Still  ", callback),
    ITEM_PROGRESS("Err T Top    ", callback),
    ITEM_PROGRESS("Err T Prod   ", callback),
    ITEM_PROGRESS("Err T Water  ", callback),
    ITEM_STRING_LIST("Warn Action  ", action, 3, callback),
    ITEM_STRING_LIST("Error Action ", action, 3, callback),
    ITEM_PROGRESS("Still sensor ", callback),
    ITEM_PROGRESS("Top sensor   ", callback),
    ITEM_PROGRESS("Prod sensor  ", callback),
    ITEM_PROGRESS("Water sensor ", callback),
    ITEM_PROGRESS("Calibrt Still", callback),
    ITEM_PROGRESS("Calibrt Top  ", callback)
    //ITEM_COMMAND("Exit / Save", exitMenu) // MAX 16 MENU ITEMS per menu. exit with long click.

);


// Define a struct to save the menu settings to eeprom.
struct mySettings 
{
  int valid, //for checking valid settings
      warnStill,
      warnTop,
      warnProd,
      warnWater,
      errStill,
      errTop,
      errProd,
      errWater,
      warnAction,
      errAction,
      sensorStill,
      sensorTop,
      sensorProd,
      sensorWater,
      calStill,
      calTop;
};
mySettings settings;

LcdMenu menu(LCD_ROWS, LCD_COLS);

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

DFRobot_BMP390L_I2C bmp;

void redrawLCD()
{
  menu.lcd->clear();
  menu.lcd->setCursor(0,0);
  menu.lcd->print(F("Power:"));
  menu.lcd->setCursor(0,2);
  menu.lcd->print(F("Vapor:"));
  menu.lcd->setCursor(0,3);
  menu.lcd->print(F("Wash:"));

  menu.lcd->setCursor(12,0);
  menu.lcd->print(F("WT"));
  menu.lcd->setCursor(12,1);
  menu.lcd->print(F("WB"));
  menu.lcd->setCursor(12,2);
  menu.lcd->print(F(">"));
  menu.lcd->setCursor(12,3);
  menu.lcd->print(F(">"));

}


void exitMenu ()
{
  EEPROM.put(0x0100, settings); // .update won't accept the struct with ints (why??)
  menuActive = false;
  menu.hide();
  redrawLCD();
}

void updateNav()
{
  byte i;

  if(menuActive) // Menu active, use menu controls
  {
      // 0 = not turning, 1 = CW, 2 = CCW
    i = rotary.rotate();

    if ( i == 1 ) {
      //CW = down or right
      if(menu.isInEditMode())
      {
        menu.right();
      }
      else {
        menu.down();
      }
    }

    if ( i == 2 ) {
      //CCW = up or left
      if(menu.isInEditMode())
      {
        menu.left();
      }
      else 
      {
        menu.up();
      }
    }
      // 0 = not pushed, 1 = pushed, 2 = long pushed
    i = rotary.pushType(1000); // number of milliseconds button has to be pushed for it to be considered a long push.

    if ( i == 1 ) {
      if(menu.isInEditMode())
      {
        //send back (confirm input)
        menu.back();
      }
      else 
      {
        //Short click
        menu.enter();
      }
    }
    
    if ( i == 2 ) 
    {
      //long click
      exitMenu();
    }
  } 

  else { // MENU not active, control only power and start menu button

  #ifndef USE_ANALOG_POT  // rotary encoder

    // 0 = not turning, 1 = CW, 2 = CCW
    i = rotary.rotate();

    if ( i == 1 )
    {
      //Clockwise -> powerplus
      if (dutyCycle > 98) 
      {
        dutyCycle = 100;
      } else
      {
        dutyCycle++;
      }
    }

    if ( i == 2 )
    {
      //Counter clockwise -> powermin
      if (dutyCycle < 2) 
      {
        dutyCycle = 0;
      } else 
      {
        dutyCycle--;
      }
    }
  #endif // end rotary encoder power level setting

  #ifdef USE_ANALOG_POT // use analog pot for power level setting only if so instructed

    int potValue = analogRead(POT_PIN);
    dutyCycle = map(potValue, 0, 1023, 0, 100);
    if (dutyCycle < 2) dutyCycle = 0;
    if (dutyCycle > 98 ) dutyCycle = 100;

  #endif

 
    // 0 = not pushed, 1 = pushed, 2 = long pushed
    i = rotary.pushType(1000); // number of milliseconds button has to be pushed for it to be considered a long push.
  
  #ifndef USE_ANALOG_POT
    if (i == 1 ) {
      dutyCycle = 0;
    }
  #endif

    if ( i == 2) 
    { 
      //Button LONG pressed, show menu
      menuActive = true;
      menu.show();
    }

  }
}


void callback(uint16_t value) 
{
  // do something when menu settings are changed
  Serial.print(F("value: "));
  Serial.println(value);
  Serial.print(F("menu: "));
  Serial.println(menu.getCursorPosition());

  switch (menu.getCursorPosition()) 
  {
    case 1:
      settings.warnStill = value;
      break;

    case 2:
      settings.warnTop = value;
      break;
    
    case 3:
      settings.warnProd = value;
      break;

    case 4:
      settings.warnWater = value;
      break;

    case 5:
      settings.errStill = value;
      break;

    case 6:
      settings.errTop = value;
      break;

    case 7:
      settings.errProd = value;
      break;

    case 8:
      settings.errWater = value;
      break;

    case 9:
      settings.warnAction = value;
      break;
    
    case 10:
      settings.errAction = value;
      break;
    
    case 11:
      settings.sensorStill = value;
      break;

    case 12:
      settings.sensorTop = value;
      break;

    case 13:
      settings.sensorProd = value;
      break;

    case 14:
      settings.sensorWater = value;
      break;

    case 15:
      settings.calStill = value;
      break;
    
    case 16:
      settings.calTop = value;
      break;

    default:
      break;
  }

}

//Time the execution of functions
void doFunctionAtInterval(void (*callBackFunction)(), uint32_t &lastEvent, uint32_t Interval) {
  uint32_t now = millis();
  if ((now - lastEvent) >= Interval) 
  {
    lastEvent = now;
    callBackFunction();
  }
}



void doUpdateLCD()
{
  // print power level
  char buf[7];
  if(dutyCycle >0)
  {
    sprintf(buf, "%3d", dutyCycle);
    menu.lcd->setCursor(7,0);
    menu.lcd->print(buf);
    menu.lcd->print(F("%"));
  } else
  {
    menu.lcd->setCursor(7,0);
    menu.lcd->print(F("OFF "));
  }
  
  //print abv TOP
  if (tempTop > 79 )
  {
    dtostrf(vaporABV, 3, 0, buf); 
    menu.lcd->setCursor(7,2);
    menu.lcd->print(buf);
    menu.lcd->print(F("%"));
  } else{
    menu.lcd->setCursor(7,2);
    menu.lcd->print(F("... "));
  }
  // print abv wash
  if (tempStill > 79 )
  {
    dtostrf(liquidABV, 4, 1, buf); 
    menu.lcd->setCursor(6,3);
    menu.lcd->print(buf);
    menu.lcd->print(F("%"));
  } else{
    menu.lcd->setCursor(7,3);
    menu.lcd->print(F("... "));
  }

  // print cooling water exit temp
  dtostrf(tempWater, 6, 1, buf);
  menu.lcd->setCursor(14,0);
  menu.lcd->print(buf);

  // print product/bottom water temp
  dtostrf(tempProd, 6, 1, buf);
  menu.lcd->setCursor(14,1);
  menu.lcd->print(buf);

  // print vapor temp
  dtostrf(tempTop, 6, 1, buf);
  menu.lcd->setCursor(14,2);
  menu.lcd->print(buf);

  // print wash temp
  dtostrf(tempStill, 6, 1, buf);
  menu.lcd->setCursor(14,3);
  menu.lcd->print(buf);

}

void doSetPower()
{
  Timer1.setPwmDuty(SSR_PIN, (float(dutyCycle) / 100) * 1023); // set pwm duty between 0-1023 
}

void doUpdateSensors()
{
  // Tidy this up for multiple sensors
  sensors.requestTemperatures(); // Send the command to get temperatures
  tempTop = sensors.getTempCByIndex(settings.sensorTop) + (float (settings.calTop - 100) / 100);
  tempStill = sensors.getTempCByIndex(settings.sensorStill) + (float (settings.calStill - 100) / 100);
  tempProd = sensors.getTempCByIndex(settings.sensorProd);
  tempWater = sensors.getTempCByIndex(settings.sensorWater);

  //todo - rest of sensors
  tempCase = bmp.readTempC();
  pressure = bmp.readPressPa()/100;

  vaporABV = TtoVaporABV(tempTop, pressure);
  liquidABV = TtoLiquidABV(tempStill, pressure);
}


void setup() 
{
  Serial.begin(9600);
  Serial.println(F("Initializing Menu..."));
  
  menu.setupLcdWithMenu(0x27, mainMenu);
  menu.hide();
  menuActive = false;

  menu.lcd->clear();



  
  // get settings from eeprom
  EEPROM.get(0x0100, settings);
  Serial.println(F("Grabbing settings from EEPROM"));
  if(settings.valid != 2) // if no valid eeprom found, reset defaults. First byte at address 0x0100 should be 2 for validtion reasons
  {
    Serial.println(F("EEPROM not OK, resetting to defaults"));
    settings.valid = 2;
    settings.warnStill = 99;
    settings.warnTop = 99; 
    settings.warnProd = 35;
    settings.warnWater = 65;
    settings.errStill = 102;
    settings.errTop = 101;
    settings.errProd = 45;
    settings.errWater = 80;
    settings.warnAction = 1;
    settings.errAction = 1;
    settings.sensorStill = 1;
    settings.sensorTop = 1;
    settings.sensorProd = 1;
    settings.sensorWater = 1;
    settings.calStill = 100;
    settings.calTop = 100;
    EEPROM.put(0x0100, settings);  //storing struct into EEPROM
  }

  //Serial.println(F("EEPROM seems to be OK"));
  //Serial.println(F("Settings retrieved:"));
  //populate menu defaults from eeprom 
  char buf[5];

  itoa(settings.warnStill, buf, 10 );
  menu[1]->setValue(buf);
  //Serial.print(F("Warn T Still: "));
  //Serial.println(buf);

  itoa(settings.warnTop, buf, 10 );
  menu[2]->setValue(buf);
  //Serial.print(F("Warn T Top: "));
  //Serial.println(buf);

  itoa(settings.warnProd, buf, 10 );
  menu[3]->setValue(buf);
  //Serial.print(F("Warn T Prod: "));
  //Serial.println(buf);

  itoa(settings.warnWater, buf, 10 );
  menu[4]->setValue(buf);
  //Serial.print(F("Warn T Water: "));
  //Serial.println(buf);

  itoa(settings.errStill, buf, 10 );
  menu[5]->setValue(buf);
  //Serial.print(F("Err T Still: "));
  //Serial.println(buf);

  itoa(settings.errTop, buf, 10 );
  menu[6]->setValue(buf);
  //Serial.print(F("Err T Top: "));
  //Serial.println(buf);

  itoa(settings.errProd, buf, 10 );
  menu[7]->setValue(buf);
  //Serial.print(F("Err T Prod: "));
  //Serial.println(buf);

  itoa(settings.errWater, buf, 10 );
  menu[8]->setValue(buf);
  //Serial.print(F("Err T Water: "));
  //Serial.println(buf);

  menu[9]->setItemIndex(settings.warnAction);
  //Serial.print(F("Warn Action: "));
  //Serial.print(settings.warnAction);
  //Serial.print(F(" or "));
  //Serial.println(action[settings.warnAction]);

  menu[10]->setItemIndex(settings.errAction);
  //Serial.print(F("Err Action: "));
  //Serial.print(settings.errAction);
  //Serial.print(F(" or "));
  //Serial.println(action[settings.errAction]);  

  itoa(settings.sensorStill, buf, 10 );
  menu[11]->setValue(buf);
  //Serial.print(F("Sensor Still: "));
  //Serial.println(buf);

  itoa(settings.sensorTop, buf, 10 );
  menu[12]->setValue(buf);
  //Serial.print(F("Sensor Top: "));
  //Serial.println(buf);

  itoa(settings.sensorProd, buf, 10 );
  menu[13]->setValue(buf);
  //Serial.print(F("Sensor Prod: "));
  //Serial.println(buf);

  itoa(settings.sensorWater, buf, 10 );
  menu[14]->setValue(buf);
  //Serial.print(F("Sensor Water: "));
  //Serial.println(buf);

  itoa(settings.calStill, buf, 10 );
  menu[15]->setValue(buf);
  //Serial.print(F("Calibrate still: "));
  //Serial.println(buf);

  itoa(settings.calTop, buf, 10 );
  menu[16]->setValue(buf);
  //Serial.print(F("calibrate Water: "));
  //Serial.println(buf);

  menu.update();
  
  

  // Init SSR pwm timer
  pinMode(SSR_PIN, OUTPUT); 
  Timer1.initialize( ssrPeriod * 1000000);                // period * 1000000 is period in seconds -> microseconds
  Timer1.pwm(SSR_PIN, 0 );  // start flipflopping on pin A with dutycycle ssrDutyCycle (initially 0, so off)
  Serial.println(F("Timer1 is switching, duty cycle set to 0% ..."));

  // start 1wire and DS18B20
  sensors.begin();
  sensors.setWaitForConversion(false);      // make it async, don't wait for the reading (we'll get it the next second)
  sensors.setResolution(12);                // set max resolution
  Serial.println(F("Started DS18B20 sensors ..."));

  // temporary, for the rotary encoder power. need to put connector on pcb
  pinMode(7, OUTPUT);
  digitalWrite(7, HIGH);

  bmp.begin();
  bmp.setSamplingMode(bmp.eUltraPrecision);
  /*bmp.setPWRMode(bmp.ePressEN |        // enable pressure reading
                    bmp.eTempEN |      // enable temp reading
                    bmp.eForcedMode);  // power mode "forced" 
*/
  bmp.setOSRMode(bmp.ePressOSRMode32 | // 32x oversampling pressure reading
                    bmp.eTempOSRMode1);// 2x oversampling temp reading
  bmp.setODRMode(BMP3XX_ODR_1P5_HZ);      // 1.5Hz data rate
  bmp.setIIRMode(BMP3XX_IIR_CONFIG_COEF_127); // set IIR coefficient for accuracy; 

  // Give everything some time to settele
  Serial.println(F("Stabilizing subsystems... :) "));
  delay (5000);

  Serial.println(F("Starting Loop ..."));
  redrawLCD();
}

void doHandleErrors(int errorType, char *errorMessage) //type 1 = warn, type 2 = error
{
  Serial.println(F("Error triggered:"));
  Serial.print(F("Type: "));
  Serial.println(errorType);
  Serial.print(F("Message:"));
  Serial.println(errorMessage);

  int i = 0;
  errActive = true; // set error active = true, prevent from giving an error direct after clearing it.
  v_doClearErrors = millis(); // reset counter to start from NOW

    if ( errorType == 1 ) 
    {
      switch (settings.warnAction)
      {
        case 0: // do nothing
          break;
        
        case 1: // BEEP
          Serial.println(F("Handling Type 1 error - warning"));
          // put notice on LCD
          menu.lcd->clear();
          menu.lcd->setCursor(0,1);
          menu.lcd->print(errorMessage);
          // Beeep
          tone(BUZZER_PIN, 2000, 500);
          delay(1000); //ouch!
          tone(BUZZER_PIN, 2000, 500);
          // Wait for input
          
          while (i == 0)
          {
            i = rotary.push();
          }
          redrawLCD();
          // error was handled:
          break;
        
        case 2: // SHUTDOWN
        Serial.println(F("Handling Type 1 error - Shutdown"));
          // shutdown still
          dutyCycle = 0;
          doSetPower();
          // put notice on LCD
          menu.lcd->clear();
          menu.lcd->setCursor(0,1);
          menu.lcd->print(errorMessage);
          menu.lcd->setCursor(0,2);
          menu.lcd->print(F("  Still Shut Down"));
          // Beeep
          tone(BUZZER_PIN, 2000, 500);
          delay(1000); //ouch!
          tone(BUZZER_PIN, 2000, 500);
          // Wait for input
          
          while (i == 0)
          {
            i = rotary.push();
          }
          redrawLCD();
          // error was handled:
          break;
      }
    }

    if ( errorType == 2 ) 
    {
      switch (settings.errAction)
      {
        case 0: // do nothing
          break;
        
        case 1: // BEEP
          Serial.println(F("Handling Type 2 error - ERROR"));
          // put notice on LCD
          menu.lcd->clear();
          menu.lcd->setCursor(0,1);
          menu.lcd->print(errorMessage);
          // Beeep
          tone(BUZZER_PIN, 2000, 500);
          delay(1000); //ouch!
          tone(BUZZER_PIN, 2000, 500);
          // Wait for input
          
          while (i == 0)
          {
            i = rotary.push();
          }
          redrawLCD();
          // error was handled:
          break;
        
        case 2: // SHUTDOWN
        Serial.println(F("Handling Type 2 error - Shutdown"));
          // shutdown still
          dutyCycle = 0;
          doSetPower();
          // put notice on LCD
          menu.lcd->clear();
          menu.lcd->setCursor(0,1);
          menu.lcd->print(errorMessage);
          menu.lcd->setCursor(0,2);
          menu.lcd->print(F("  Still Shut Down"));
          // Beeep
          tone(BUZZER_PIN, 2000, 500);
          delay(1000); //ouch!
          tone(BUZZER_PIN, 2000, 500);
          // Wait for input
          
          while (i == 0)
          {
            i = rotary.push();
          }
          redrawLCD();
          // error was handled:
          break;
      }
    }
  

}

void doCheckErrors()
{

  if( tempTop > settings.warnTop )    { char msg[20] = "Vapor Temp Warning!";  doHandleErrors(1, msg ); }
  if( tempStill > settings.warnStill ){ char msg[20] = " Wash Temp Warning!";  doHandleErrors(1, msg ); }
  if( tempWater > settings.warnWater ){ char msg[20] = "Water Temp Warning!";  doHandleErrors(1, msg ); }
  if( tempProd > settings.warnProd )  { char msg[20] = "Water Temp Warning!";  doHandleErrors(1, msg ); }

  if( tempTop > settings.errTop )     { char msg[20] = "  Vapor Temp Error!";  doHandleErrors(2, msg ); }
  if( tempStill > settings.errStill ) { char msg[20] = "  Wash Temp Error!";   doHandleErrors(2, msg ); }
  if( tempWater > settings.errWater)  { char msg[20] = " Water Temp Error!";   doHandleErrors(2, msg ); }
  if( tempProd > settings.errProd )   { char msg[20] = "Product Temp Error!";  doHandleErrors(2, msg ); }

  if( tempCase > tempCaseMax )        { char msg[20] = "Control Temp Error!";  doHandleErrors(2, msg); }
}

void doClearErrors()
{
  errActive = false;
}


void loop() 
{
  updateNav(); //check rotary button for actions, read analog pot if any
  
  if(!menuActive) 
  {
    doFunctionAtInterval(doUpdateLCD, v_UpdateLCD, 100);
    doFunctionAtInterval(doSetPower, v_doSetPower, 1000);
    doFunctionAtInterval(doUpdateSensors, v_doUpdateSensors, 1000);
    doFunctionAtInterval(doClearErrors, v_doClearErrors, 120000); // clear errors for 2 minutes
    
    if(!errActive) doCheckErrors(); // only throw an error if erractive has been cleared

  }

}
