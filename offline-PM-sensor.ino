#include <Sds011.h>
#include "WiFi.h"
#include "FS.h"
#include "SPIFFS.h"
#include "Wire.h"

/* 
 *  Portable offline low-power particle sensor device. This device will wake up every 5 
 *  minutes, measure PM10 and PM2.5, write data to a file and go back to deep sleep. 
 *  A pushbutton starts a wifi accesspoint (called "LUFTDATA") and a small web server 
 *  from which data can be fetched (available at http://192.168.1.1). 
 *  
 *  The DS3231 Real time clock makes sure the data file logs correct timestamps.
 *  
 *  For more information see https://github.com/luftdata/offline-PM-sensor
 *  
 *  Copyright 2019 Peter Krantz
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *  
 *  http://www.apache.org/licenses/LICENSE-2.0
 *  
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *  
*/

#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP 300   // Measure interval in secs (300 = 5 mins)
#define FORMAT_SPIFFS_IF_FAILED true

// Store boot count after deep sleep
RTC_DATA_ATTR int bootCount = 0;

// Only tested with Adafruit feather ESP32
HardwareSerial& serialSDS(Serial2);
Sds011Async< HardwareSerial > sds011(serialSDS);
#define SDS_PIN_RX 16
#define SDS_PIN_TX 17

// Store PM data
constexpr int pm_tablesize = 20;
int pm25_table[pm_tablesize];
int pm10_table[pm_tablesize];
bool is_SDS_running = true;

// file in flash memory to store data in
const char datafile[] = "/data.csv";

// Real time clock
#define DS3231_I2C_ADDRESS 0x68
char timestamp[16];

// Web server setup. A sesnor should be accessible for everyone. TODO: passwordless?
const char* ssid = "LUFTDATA";  
const char* password = "12345678";  

IPAddress local_ip(192,168,1,1);
IPAddress gateway(192,168,1,1);
IPAddress subnet(255,255,255,0);
WiFiServer server(80);
String header;

// Button setup
const int buttonPin = 33;  
const int ledPin =  12; 
#define BUTTON33_PIN_BITMASK 0x200000000 // 2^33 in hex


byte decToBcd(byte val)
{
  return( (val/10*16) + (val%10) );
}

byte bcdToDec(byte val)
{
  return( (val/16*10) + (val%16) );
}


/* Code for setting the DS3231 clock the first time it is used.
 *  
void setDS3231time(byte second, byte minute, byte hour, byte dayOfWeek, byte dayOfMonth, byte month, byte year)
{
 
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(0);
  Wire.write(decToBcd(second)); 
  Wire.write(decToBcd(minute));
  Wire.write(decToBcd(hour)); 
  Wire.write(decToBcd(dayOfWeek)); 
  Wire.write(decToBcd(dayOfMonth)); 
  Wire.write(decToBcd(month)); 
  Wire.write(decToBcd(year)); 
  Wire.endTransmission();
}

*/


// Read time from the D3231 real time clock.
void readDS3231time(byte *second,
byte *minute,
byte *hour,
byte *dayOfWeek,
byte *dayOfMonth,
byte *month,
byte *year)
{
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(0); 
  Wire.endTransmission();
  Wire.requestFrom(DS3231_I2C_ADDRESS, 7);
  
  *second = bcdToDec(Wire.read() & 0x7f);
  *minute = bcdToDec(Wire.read());
  *hour = bcdToDec(Wire.read() & 0x3f);
  *dayOfWeek = bcdToDec(Wire.read());
  *dayOfMonth = bcdToDec(Wire.read());
  *month = bcdToDec(Wire.read());
  *year = bcdToDec(Wire.read());
}

//
// Get a properly formatted timestamp string from the real time clock.
//
void getTimestamp(char *timestamp) {
  byte second, minute, hour, dayOfWeek, dayOfMonth, month, year;
  
  readDS3231time(&second, &minute, &hour, &dayOfWeek, &dayOfMonth, &month,
  &year);

  sprintf(timestamp, "20%d-%02d-%02dT%02d:%02d:%02d", year, month, dayOfMonth, hour, minute, second);
}



// Start particle sensor
void start_SDS() {
    Serial.println("Start wakeup SDS011");

    if (sds011.set_sleep(false)) { is_SDS_running = true; }

    Serial.println("End wakeup SDS011");
}

//Put particle sensor to sleep
void stop_SDS() {
    Serial.println("Start sleep SDS011");

    if (sds011.set_sleep(true)) { is_SDS_running = false; }

    Serial.println("End sleep SDS011");
}


// List files in "directory".
void listDir(fs::FS &fs, const char * dirname, uint8_t levels){
    Serial.printf("Listing directory: %s\r\n", dirname);

    File root = fs.open(dirname);
    if(!root){
        Serial.println("- failed to open directory");
        return;
    }
    if(!root.isDirectory()){
        Serial.println(" - not a directory");
        return;
    }

    File file = root.openNextFile();
    while(file){
        if(file.isDirectory()){
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if(levels){
                listDir(fs, file.name(), levels -1);
            }
        } else {
            Serial.print("  FILE: ");
            Serial.print(file.name());
            Serial.print("\tSIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }
}


// Read a file and print file contents to serial console.
void readFile(fs::FS &fs, const char * path){
    Serial.printf("Reading file: %s\r\n", path);

    File file = fs.open(path);
    if(!file || file.isDirectory()){
        Serial.println("- failed to open file for reading");
        return;
    }

    Serial.println("- read from file:");
    while(file.available()){
        Serial.write(file.read());
    }
}


void writeFile(fs::FS &fs, const char * path, const char * message){
    Serial.printf("Writing file: %s\r\n", path);

    File file = fs.open(path, FILE_WRITE);
    if(!file){
        Serial.println("- failed to open file for writing");
        return;
    }
    if(file.print(message)){
        Serial.println("- file written");
    } else {
        Serial.println("- frite failed");
    }
}


// Append data to text file
void appendFile(fs::FS &fs, const char * path, const char * message){
    Serial.printf("Appending to file: %s\r\n", path);

    File file = fs.open(path, FILE_APPEND);
    if(!file){
        Serial.println("- failed to open file for appending");
        return;
    }
    if(file.print(message)){
        Serial.println("- message appended");
    } else {
        Serial.println("- append failed");
    }
}


// Blink LED. 
void blink(int count) {
  Serial.print("Blinking: ");
  Serial.println(count);
  
  for(int i=0; i <= count; i++) {
    digitalWrite(LED_BUILTIN, HIGH); 
    delay(500);                       
    digitalWrite(LED_BUILTIN, LOW);
    delay(100);
  }

}



void measure()
{
    // Per manufacturer specification, place the sensor in standby to prolong service life.
    // At an user-determined interval (here 30s down plus 30s duty = 1m), run the sensor for 30s.
    // Quick response time is given as 10s by the manufacturer, thus omit the measurements
    // obtained during the first 10s of each run.
    
    constexpr uint32_t down_s = 10;

    stop_SDS();
    Serial.print("stopped SDS011 (is running = ");
    Serial.print(is_SDS_running);
    Serial.println(")");

    uint32_t deadline = millis() + down_s * 1000;
    while (static_cast<int32_t>(deadline - millis()) > 0) {
        delay(1000);
        Serial.println(static_cast<int32_t>(deadline - millis()) / 1000);
        sds011.perform_work();
    }

    constexpr uint32_t duty_s = 30;

    start_SDS();
    Serial.print("started SDS011 (is running = ");
    Serial.print(is_SDS_running);
    Serial.println(")");

    sds011.on_query_data_auto_completed([](int n) {
        Serial.println("Begin Handling SDS011 query data");
        int pm25;
        int pm10;
        Serial.print("n = "); Serial.println(n);
        if (sds011.filter_data(n, pm25_table, pm10_table, pm25, pm10) &&
            !isnan(pm10) && !isnan(pm25)) {
            Serial.print("PM10: ");
            Serial.println(float(pm10) / 10);
            Serial.print("PM2.5: ");
            Serial.println(float(pm25) / 10);

            float valpm10 = float(pm10) / 10;
            float valpm25 = float(pm25) / 10;

            //get current time from Real time clock
            
            getTimestamp(timestamp);
            String message = (String)timestamp + ", " + bootCount + ", " + valpm25 + ", " + valpm10 + "\r\n";
            Serial.print(message);
            int str_len = message.length() + 1;
            char message_array[str_len];
            message.toCharArray(message_array, str_len);

            // append data to file in flash memory
            appendFile(SPIFFS, datafile, message_array);            

        }
        Serial.println("End Handling SDS011 query data");
        });

    if (!sds011.query_data_auto_async(pm_tablesize, pm25_table, pm10_table)) {
        Serial.println("measurement capture start failed");
    }

    deadline = millis() + duty_s * 1000;
    while (static_cast<int32_t>(deadline - millis()) > 0) {
        delay(1000);
        Serial.println(static_cast<int32_t>(deadline - millis()) / 1000);
        sds011.perform_work();
    }

    // make time to collect data
    delay(1000);
    sds011.perform_work();
    delay(1000);
}



// Start AP and web server
void runWebServer() {
  WiFi.softAP(ssid, password);

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  
  server.begin(); 
}




void gotoDeepSleep() {
  Serial.println("Going to sleep...");
  stop_SDS();
  digitalWrite(15, LOW); // Turn power off for SDS011 and clock (clock will run on built in backup battery).
  delay(200);

  // Make ESP32 remember power pin state in deep sleep
  gpio_hold_en(GPIO_NUM_15);
  gpio_deep_sleep_hold_en();
  Serial.println("Remembering pin 15 state. In deep sleep.");

  esp_sleep_enable_ext1_wakeup(BUTTON33_PIN_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}



void setup(){
  
  // remember boot count
  bootCount++;  

  // This pin controls power to the SDS011 and the clock.
  pinMode(15, OUTPUT);  

  // Built in led.
  pinMode(LED_BUILTIN, OUTPUT);
  
  // Disable WiFi
  WiFi.mode(WIFI_OFF);

  // Assume real time clock is correct.
  Wire.begin();  

  // Make sure flash disk is available
  Serial.begin(115200);

  // Check wakeup reason - button or timer?
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();
  
  switch(wakeup_reason){
    case ESP_SLEEP_WAKEUP_EXT0 : Serial.println("Wakeup caused by external signal using RTC_IO"); break;
    case ESP_SLEEP_WAKEUP_EXT1 : 
      Serial.println("Wakeup caused by external signal using RTC_CNTL. Starting web server."); 
      
      break;
    case ESP_SLEEP_WAKEUP_TIMER : Serial.println("Wakeup caused by timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD : Serial.println("Wakeup caused by touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP : Serial.println("Wakeup caused by ULP program"); break;
    default : Serial.printf("Wakeup was not caused by deep sleep: %d\n",wakeup_reason); break;
  }
  
  if(!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED)){
    Serial.println("SPIFFS Mount Failed");
    return;
  }    

  // Sample and go to sleep unless wakup was from button.
  if(wakeup_reason != ESP_SLEEP_WAKEUP_EXT1) {
    blink(2);

    // Power up sensor and clock
    gpio_hold_dis(GPIO_NUM_15);
    digitalWrite(15, HIGH);
    delay(1000);

    // Get data from SDS011 
    Serial.println("Power on. Sampling data...");
    serialSDS.begin(9600, SERIAL_8N1, SDS_PIN_RX, SDS_PIN_TX);
    delay(100);
    measure();
    Serial.println("Sampling data done...");
    
    gotoDeepSleep();
    
  } else {

    // Wakeup was from button - go into AP mode with a simple web server.
    runWebServer(); 
    
    // blink
    blink(3);
    
    // List files on serial console
    listDir(SPIFFS, "/", 0);

    // Dump data file
    readFile(SPIFFS, datafile);

    // next up web server in loop()
  }

}


// Loop only handles web server and AP  
void loop(){
  WiFiClient client = server.available();   // Listen for incoming clients

  if (client) {                             // If a new client connects,
    Serial.println("New Client.");          // print a message out in the serial port
    String currentLine = "";                // make a String to hold incoming data from the client
    while (client.connected()) {            // loop while the client's connected
      if (client.available()) {             // if there's bytes to read from the client,
        char c = client.read();             // read a byte, then
        Serial.write(c);                    // print it out the serial monitor
        header += c;
        if (c == '\n') {                    // if the byte is a newline character
          
          // if the current line is blank, you got two newline characters in a row.
          // that's the end of the client HTTP request, so send a response:
          if (currentLine.length() == 0) {
            // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
            // and a content-type so the client knows what's coming, then a blank line:
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println("Connection: close");
            client.println();

            // Was sleep button clicked?
            if (header.indexOf("GET /sleep") >= 0) {
              Serial.println("Sleep requested");
              WiFi.mode(WIFI_OFF);
              gotoDeepSleep();
            }
            
            // Display the HTML web page
            client.println("<!DOCTYPE html><html lang='en'>");
            client.println("<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
            client.println("<link rel=\"icon\" href=\"data:,\">");
            // CSS to style the on/off buttons 
            // Feel free to change the background-color and font-size attributes to fit your preferences
            client.println("<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}");
            client.println(".button { background-color: #4CAF50; border: none; color: white; padding: 16px 40px;");
            client.println("text-decoration: none; font-size: 30px; margin: 2px; cursor: pointer;}");
            client.println(".button2 {background-color: #555555;}</style></head>");
            
            client.println("<body><h1>LUFTDATA Sensor</h1>");
            getTimestamp(timestamp);
            client.println("<p>Current time:" + (String)timestamp + "</p>");
            
            //page html
            
            client.println("<form><textarea style='width:90%;min-height:300px'>");
            Serial.printf("Reading data file: %s\r\n", datafile);

            File file = SPIFFS.open(datafile);

            uint8_t file_buffer[16];
            int avail;
            while (avail = file.available()) {
              int to_read = min(avail, 16);
              if (to_read != file.read(file_buffer, to_read)) {
                break;
              }
              client.write(file_buffer, to_read);
            }
            
            file.close();            
                      
            client.println("</textarea></form>");  

            // Sleep button
            client.println("<p><a href=\"/sleep\"><button class=\"button\">Go to sleep</button></a></p>");
            
            // end page html
            client.println("</body></html>");
            
            // The HTTP response ends with another blank line
            client.println();
            // Break out of the while loop
            break;
          } else { // if you got a newline, then clear currentLine
            currentLine = "";
          }
        } else if (c != '\r') {  // if you got anything else but a carriage return character,
          currentLine += c;      // add it to the end of the currentLine
        }
      }
    }
    // Clear the header variable
    header = "";
    
    // Close the connection
    client.stop();
    Serial.println("Client disconnected.");

  }
}
