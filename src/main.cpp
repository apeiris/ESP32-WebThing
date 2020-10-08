// current device mac 2462ABCEBDBC
// * Tested on ESP8266, ESP32, Arduino boards with WINC1500 modules (shields or
// t11

// -*- mode: c++;  c-basic-offset: 2 -*-
/**
 * Simple server compliant with Mozilla's proposed WoT API
 * Originally based on the HelloServer example
 * MKR1000)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 * 
 * this code is based on
 * https://github.com/WebThingsIO/webthing-arduino/blob/master/examples/LEDLamp/LEDLamp.ino
 */
#define LARGE_JSON_BUFFERS 1

#include <Arduino.h>
#include "Thing.h"
#include "WebThingAdapter.h"
#include <SPI.h>
#include "LedControl.h"
#include <Adafruit_AHTX0.h>
#include <analogWrite.h>
#include <MySQL_Connection.h>
#include <MySQL_Cursor.h>
#include <ArduinoOTA.h>
#include <map>
#include "password.h"
#include "myUtils.h"
#include "esp_system.h"
#include "esp_log.h"

using namespace std;
static const char * TAG = "main.cpp";


IPAddress sqlIP(192, 168, 0, 10);
/// Only used for monitoring, can be removed it's not part of our "thing"

#define pushInterval (60000 / 1)
#if defined(LED_BUILTIN)
const int ledPin = LED_BUILTIN;
#else
const int ledPin = 2; // manually configure LED pin was 13 2 is blue led built in
const int lampPin = 2;
#endif
// AHT10 pins
const int pinSDA = 5;  // white wire
const int pinSCL = 17; // the purple

// pin definitions for MAX7129
const int pinCS = 15;  // Chip Select
const int pinCLK = 18; // Clock pin
const int pinDIN = 23; // Data
const int anzMAX = 1;  //Anzahl der kaskadierten  Module = Number of Cascaded modules

long mmap(long x, long in_min, long in_max, long out_min, long out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
WebThingAdapter *adapter; // adapter could then be used as as device
ThingActionObject *action_generator(DynamicJsonDocument *);
const char *lampTypes[] = {"OnOffSwitch", "Light", nullptr};
const char *sensorTypes[] = {"Sensor", "Sensor", nullptr};
const char *asyncProperties[] = {"asyncProperty", nullptr};

StaticJsonDocument<256> fadeInput;
JsonObject fadeInputObj = fadeInput.to<JsonObject>();
ThingAction fade("fade", "Fade", "Fade the lamp to a given level", "FadeAction", &fadeInputObj, action_generator);
ThingEvent overheated("overheated", "The lamp has exceeded its safe operating temperature", NUMBER, "OverheatedEvent");

ThingDevice lamp("ABC", "ABC", lampTypes);
ThingDevice AHT10Device("AHT10", "AHT10", sensorTypes);
ThingDevice textDisplay("asyncProperty", "Async Property Test", asyncProperties);

ThingProperty lampOn("on", "Whether the lamp is turned on", BOOLEAN, "OnOffProperty");
ThingProperty lampLevel("brightness", "The level of light from 0-100", INTEGER, "BrightnessProperty");
ThingProperty AHT10TemperatureProperty("Temperature", "Temperature in C", NUMBER, "Centigrades");
ThingProperty AHT10HumidityProperty("Humidity", "Humidity (RH) %", NUMBER, "%");

// Forward declaration
void textDisplayTextChanged(ThingPropertyValue newVal);
ThingProperty textDisplayText("text", "", STRING, nullptr, textDisplayTextChanged);
//ThingProperty textDisplayToggle("toggle","",STRING,nullptr,textDisplayToggled);
//ThingProperty textDisplayNumber("number","",STRING,nullptr,textDisplayNumbenewrChanged);
String message = "message";
String lastMessage = message;
void textDisplayTextChanged(ThingPropertyValue newVal)
{
    String x = *newVal.string;
    ESP_LOGI(TAG, "text=>%s\n", x.c_str());
}

String lastColor = "#ffffff";

const unsigned char redPin = 12;
const unsigned char greenPin = 13;
const unsigned char bluePin = 14;
//---------------GPIO33 pulled low and connected to button to pull high to 3.3v
#define BUTTON_WAKEUP_BITMASK 0x200000000 // 2^33 in hex / GPIO33
//------------------------------------------------------------------

//------------Adafruit Lib -----------------------------------------
// the AHT must be connected to Bords SDA->GPIO21 and SCL ->GPIO22
//  to detect the sensor board
Adafruit_AHTX0 aht;
//------------------------------------------------------------------
LedControl lc = LedControl(pinDIN, pinCLK, pinCS, 1);
//------------------------------------------------------------------
WiFiClient client;
MySQL_Connection sqlConn(&client);


const int baseid = 0;
String baseKey = ""; // this is the Wifi Mac strip of :
std::map<int, string> deviceMap;
char sqlStmt[] = "CALL IOT.insertDevice('%s','%s','%s');";
char sQuery[300];

void registerDevice(ThingDevice *d, ThingProperty *p)
{
    // printf("Registering device = %s, property=%s description=%s\n", d->id.c_str(), p->id.c_str(),p->description.c_str());
    sprintf(sQuery, sqlStmt, d->id.c_str(), p->id.c_str(), p->description.c_str());

    if (sqlConn.connect(sqlIP, sqlPort, sqlUser, sqlPassword))
    {
       // ESP_LOGI(TAG, "Connected in registerDevice for query \n\t delaying for 500\n->%s \n", sQuery);
        delay(200);
        MySQL_Cursor *curm=new MySQL_Cursor(&sqlConn);
        curm->execute(sQuery);
        char * x=  curm->getLastScalar();
        ESP_LOGI(TAG,"Query=%s\n\t rv=%s\n",sQuery,x);
        curm->close();
        delete curm;
        
        
       
    }
    else
    { ESP_LOGE(TAG,"Sql connect failed\n" );}
    
}
void setup(void)
{
    uint8_t macAddr[6] = {0};
    esp_err_t ret = ESP_OK;
    // setup pins init MAX7219
    {

        pinMode(ledPin, OUTPUT);
        digitalWrite(ledPin, HIGH);
        Serial.begin(115200);
        ESP_LOGI(TAG, "Connecting to %s \n", ssid);
        //--- RTC wakeup on gpio 33 (wired to button)
        esp_sleep_enable_ext1_wakeup(BUTTON_WAKEUP_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
        //------------------Initialize MAX7219-------------
        ESP_LOGI(TAG, "Initializing MAX7219..\n");
        lc.shutdown(0, false);
        lc.setIntensity(0, 10);
        lc.clearDisplay(0);
        lc.printF(0, (char *)"%0.2f");
    }
    //-----------Detect AHT ----------------------------
    if (!aht.begin())
    {
        ESP_LOGI(TAG, "Could not fine AHT ! \n");
    }
    else
        ESP_LOGI(TAG, "AHT10 or AHT20 found \n");
    //---------connect Wifi(STA); blink while connecting--------------
    {
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, password);

        // Wait for connection
        bool blink = true;
        while (WiFi.status() != WL_CONNECTED)
        {
            printf(".");
            digitalWrite(ledPin, blink ? LOW : HIGH); // active low led
            digitalWrite(ledPin, blink);
            blink = !blink;
            delay(80);
        }
        baseKey = WiFi.macAddress();
        baseKey.replace(":", "");
        ESP_LOGI(TAG, "Connected to-> %s : localIP=%s: mac=%s \n", ssid, (char *)WiFi.localIP().toString().c_str(), baseKey.c_str());
        ret = esp_efuse_mac_get_default(macAddr);
  
    }
    // connect MySQL on given SQLIP (in password.h)
    {
        if (sqlConn.connect(sqlIP, sqlPort, sqlUser, sqlPassword))
        {
            ESP_LOGI(TAG, "Connected to SQL On ->%s \n", (char *)sqlIP.toString().c_str());
            // exit test/wakeup (press button(on gpio33) to wakeup)
            digitalWrite(ledPin, LOW); // turn off active low led
        }
        else
        {
            ESP_LOGI(TAG, "Oh.. could not connect to SQL %s \n ", (char *)sqlIP.toString().c_str());
            ESP_LOGI(TAG, "Placing this station to Deep Sleep!. Press the Button(GPIO33) to wakeup\n");
            esp_deep_sleep_start();
        };
        // now get the registered device list;

        // digitalWrite(ledPin, HIGH); // turn off active low led
    }
    // Init Mozilla thing and begin the adapter
    {

        String x = WiFi.macAddress();
        x.replace(":", "");
        adapter = new WebThingAdapter(x, WiFi.localIP());
        //   registerDevice(x,"base");
        {
            lamp.description = "A web conneced lamp";
            lamp.title = "On/Off";

            lampLevel.title = "Brightness";
            lampLevel.minimum = 0;
            lampLevel.maximum = 100;
            lampLevel.unit = "%";
            lamp.addProperty(&lampLevel);
        }
        // create fade Input object and json
        {
            fadeInputObj["type"] = "object";
            JsonObject fadeInputProperties = fadeInputObj.createNestedObject("properties");

            JsonObject brightnessInput = fadeInputProperties.createNestedObject("brightness");
            brightnessInput["type"] = "integer";
            brightnessInput["minimum"] = 0;
            brightnessInput["maximum"] = 100;
            brightnessInput["unit"] = "percent";

            JsonObject durationInput = fadeInputProperties.createNestedObject("duration");
            durationInput["type"] = "integer";
            durationInput["minimum"] = 1;
            durationInput["unit"] = "milliseconds";
        }
        // add device(lamp) actions and events and add the Device to adapter then begin the adapter

        lamp.addAction(&fade);
        overheated.unit = "degree C";
        lamp.addEvent(&overheated);

        lamp.addProperty(&lampOn);
        AHT10Device.addProperty(&AHT10TemperatureProperty);
        AHT10Device.addProperty(&AHT10HumidityProperty);
        // ThingPropertyValue v;

        //  textDisplayText.setValue(v);
        textDisplay.addProperty(&textDisplayText);

        adapter->addDevice(&lamp);
        adapter->addDevice(&AHT10Device);
        adapter->addDevice(&textDisplay);
        adapter->begin();
    }
    ESP_LOGE(TAG, "Adapter initialized \n");

    ThingDevice *d = adapter->getFirstDevice();
    while (d)
    {
        ThingProperty *p = d->firstProperty;
        while (p)
        {
            // registerDevice(d->id, p->id);
            registerDevice(d, p);
            p = (ThingProperty *)p->next;
        }
        d = d->next;
    }
}
sensors_event_t humidity, temperature;
static int i = 0;

ThingPropertyValue toPvalueNumber(double n)
{
    ThingPropertyValue pv;
    pv.number = n;
    return pv;
}
void readAHT10()
{
    aht.getEvent(&humidity, &temperature);

    AHT10HumidityProperty.setValue(toPvalueNumber(humidity.relative_humidity));
    AHT10TemperatureProperty.readOnly = true;
    AHT10TemperatureProperty.setValue(toPvalueNumber(temperature.temperature));
    ESP_LOGI(TAG, "%05d Humidity=%.2lf%% : Tempurature=%.2lf\n", ++i, humidity.relative_humidity, temperature.temperature);
}
void loop(void)
{

    digitalWrite(23, HIGH);

    readAHT10();
    adapter->update(); // pushit to the iot gateway

    delay(pushInterval);
}
void do_fade(const JsonVariant &input)
{
    JsonObject inputObj = input.as<JsonObject>();
    long long int duration = inputObj["duration"];
    long long int brightness = inputObj["brightness"];

    delay(duration);

    ThingDataValue value = {.integer = brightness};
    lampLevel.setValue(value);

    int level = (int)Arduino_h::map(brightness, 0, 100, 255, 0);
    ESP_LOGI(TAG, "value =%i , level(mapped)=%d \n", (int)value.integer, level);
    analogWrite(lampPin, level, 255);
    lc.clearDisplay(0);
    lc.printF((float)brightness, (char *)"%.2f");
    ThingDataValue val;

    ThingEventObject *ev = new ThingEventObject("overheated", NUMBER, val);
    ESP_LOGI(TAG, " Queu event(overheated) %2.2f\n", val.number);
    lamp.queueEventObject(ev);
}
ThingActionObject *action_generator(DynamicJsonDocument *input)
{
    String output;
    serializeJson(*input, output);

    ESP_LOGI(TAG, "printing ->input %s\n", output.c_str());
    return new ThingActionObject("fade", input, do_fade, nullptr);
}