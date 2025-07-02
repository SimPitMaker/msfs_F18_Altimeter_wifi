/*
  Credit:
  A DCS (Digital Combat Simulator) user named Tanarg
  provided the initial idea for using the below hardware
  in this way for a radar altimeter on the DCS forums.
  I just adapted it for use as an a standard altimeter 
  and worked out a connection with Microsoft Flight 
  Simulator using Axis and Ohs.

  Expected Software (tested with):
    Microsoft Flight Simulator 2024 (SU2)
    Axis and Ohs (v4.53 b19)

  Expected Hardware:
    Adafruit Qualia ESP32-S3
    https://www.adafruit.com/product/5800

    Round RGB TTL TFT Display
    - 2.1" 480x480
    - No Touchscreen
    - TL021WVC02-B1323B
    https://www.adafruit.com/product/5806

  This instrument is an altimeter with an optional movable
  altitude bug to help with procedures. In order for the bug
  to work, you must create a numberic user L variable in Axis
  and Ohs called AAO_Altimeter_Bug. Then you would bind physical
  hardware to increase and decrease that bug. The examples below
  create a wrapping bug that increments in 25ft amounts, so when
  it gets to max, it wraps back to 0 and vise versa. So create a
  script in AAO for increase:

  (L:AAO_Altimeter_Bug) 975 >= if{
    0 (>L:AAO_Altimeter_Bug)
  } els{
    (L:AAO_Altimeter_Bug) 25 + (>L:AAO_Altimeter_Bug)
  }

  And a script for decrease:

  (L:AAO_Altimeter_Bug) 0 <= if{
    975 (>L:AAO_Altimeter_Bug)
  } els{
    (L:AAO_Altimeter_Bug) 25 - (>L:AAO_Altimeter_Bug)
  }

  ERROR HANDLING: The four digit pressure value is used to
  indicate an error condition. As the record air pressure 
  in inHg was 32.01 (1968, Siberia) we have a lot of numbers
  to play with, here's the current setup:

  3333: Connection to AAO works. Connection to sim works.
        No plane name. Likely because AAO is just at the
        "waiting for aircraft" state.

  4444: Connection to AAO works but the return data cannot
        be processed. Uncomment the "Show Raw Response" 
        portion of the getData() function to see what is
        being sent back from AAO to troubleshoot.

  5555: Connection to AAO works but the return data does not
        have a plane name. Something is wrong with the sim as
        we are getting all data but plane name.

  6666: Connection to AAO works, connection to sim works, but
        no data is being sent back. So not only is something
        wrong in the sim (not flying?) the AAO variable for 
        the bug also isn't setup and returning a null value.

  7777: Connection to AAO does not work. Verify it is running
        and that it is remotely (within your network)
        accessible. Also make sure the sim is running, if it
        isn't, then this error will also show.

  8888: Connection to AAO works. Connection to sim works.
        Getting altitude bug values ouside of the sensible
        range of 0ft to 999ft. Check the AAO script code.

  9999: No Wifi. Power cycle the ESP32 after verifying wifi
        credentials and access.
*/

// Wifi Libraries for connection to AAO
#include <WiFi.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
// Used for connection to the sim using wifi
const char* ssid = "YOUR_SSID_HERE";
const char* password = "YOUR_WIFI_PASS";
const char* serverUrl = "http://YOUR_PC_IP:43380/webapi";
// Example can be viewed at:
// http://YOUR_PC_IP:43380/webapi/buttonexample/index.html

// Setup some global variables for use elsewhere as needed
String aircraftTitle = "";
String prevAircraftTitle = "";
int aircraftMatchIndex = -1;
int indicatedAltitude = 0;
double rawPressure = 0.0;
int barPressure = 0;
int bugAngle = 0;

// Import the graphics library for sending the images out to the display
#include <Arduino_GFX_Library.h>
#include "needle.h"         // needleImg
#include "lgVertNum.h"      // lgVertNumImg
#include "smVertNum.h"      // smVertNumImg
#include "gaugeFace.h"      // gaugeFaceImg
#include "bug.h"            // altimeter bug

// Now setup the display for the GFX Library
const byte colorDepth = 16;

Arduino_XCA9554SWSPI *expander = new Arduino_XCA9554SWSPI(
    PCA_TFT_RESET, PCA_TFT_CS, PCA_TFT_SCK, PCA_TFT_MOSI, &Wire, 0x3F);

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    TFT_DE, TFT_VSYNC, TFT_HSYNC, TFT_PCLK, TFT_R1, TFT_R2, TFT_R3, TFT_R4, TFT_R5, 
    TFT_G0, TFT_G1, TFT_G2, TFT_G3, TFT_G4, TFT_G5, TFT_B1, TFT_B2, TFT_B3, TFT_B4, TFT_B5,
    1 /* hync_polarity */, 50 /* hsync_front_porch */, 2 /* hsync_pulse_width */, 44 /* hsync_back_porch */,
    1 /* vsync_polarity */, 16 /* vsync_front_porch */, 2 /* vsync_pulse_width */, 18 /* vsync_back_porch */
    // ,1, 30000000
    );

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    // 2.1" 480x480 round display
    480 /* width */, 480 /* height */, rgbpanel, 0 /* rotation */, true /* auto_flush */, 
    expander, GFX_NOT_DEFINED /* RST */, TL021WVC02_init_operations, sizeof(TL021WVC02_init_operations));

// Just include the TFT_eSPI as we need the image manipulation functions
// So don't worry about the User_Config.h for that package.
#include <TFT_eSPI.h>
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite background = TFT_eSprite(&tft);
TFT_eSprite lgVertNumTenT = TFT_eSprite(&tft);
TFT_eSprite lgVertNumThou = TFT_eSprite(&tft);
TFT_eSprite smVertNumTenT = TFT_eSprite(&tft);
TFT_eSprite smVertNumThou = TFT_eSprite(&tft);
TFT_eSprite smVertNumHund = TFT_eSprite(&tft);
TFT_eSprite smVertNumTens = TFT_eSprite(&tft);
TFT_eSprite gaugeFace = TFT_eSprite(&tft);
TFT_eSprite needle = TFT_eSprite(&tft);
TFT_eSprite bug = TFT_eSprite(&tft);

// This will return the value of the digit
// at the chosen digit location for a given num.
// We count from the right so the hundreds digit
// is always the 3rd from the right. The arrays
// count from the left though with the first
// position as the 0th element.
int getDigit(int num, int digit) {
  // String manipulation is a lot faster than
  // doing modulus math or division
  String strNum = String(num);
  if (strNum.length() < digit) {
    // asking for a digit but num isn't big enough
    return 0;
  }else {
    // adjust so we give correct positional digit
    // e.g. 23645, want 3rd digit (hundreds)
    // so 5 - 3 = 2, the element in the 2 position
    // of the array will be the hundreds (6)
    int adjDigit = strNum.length() - digit;
    // get the digit
    char charDigit = strNum.charAt(adjDigit);
    // convert from char to int
    int intDigit = charDigit - '0';
    return intDigit;
  }
}

// Get the angle of the needle based on the hundreds
// value of the current altitude
int getNeedleAngle(int altitude) {
  // Goes 0->999 map to 0->359
  // Only care about up to tens place
  int currHund = getDigit(altitude, 3);
  int currTens = getDigit(altitude, 2);
  int hundTen = (currHund * 100) + (currTens * 10);
  int currAngle = map(hundTen, 0, 999, 0, 359);
  return currAngle;
}

// Move the ten-thousand and thousand vertical number
// image strips so the right number is visible in the window
void drawLgVertNums(int tenThou, int thou) {
  // large digits top of each number in the list in pixels (y-position)
  int topPos[] = {0, 47, 94, 140, 187, 233, 279, 326, 372, 419};
  // Work on the ten thousands position
  int yPosTenT = topPos[tenThou];
  /*
    The top of the box for the image is 130,166. So to
    view the 0, you position the image at 166. To view
    the 1, it is positioned at 166-47 and so forth.
  */
  yPosTenT = 166 - yPosTenT;
  lgVertNumTenT.pushToSprite(&background, 130, yPosTenT);
  // Work on the thousands position, x is now 161
  int yPosThou = topPos[thou];
  yPosThou = 166 - yPosThou;
  lgVertNumThou.pushToSprite(&background, 161, yPosThou);
}

// Move the four digits for the barometric pressure
// display. Expecting a four digit number.
void drawSmVertNums(int tenThou, int thou, int hund, int tens) {
  // large digits top of each number in the list in pixels (y-position)
  int topPos[] = {0, 36, 73, 109, 145, 181, 217, 253, 289, 325};
  // Work on the ten thousands position
  int yPosTenT = topPos[tenThou];
  /*
    The top of the box for the image is 192,353. So to
    view the 0, you position the image at 353. To view
    the 1, it is positioned at 353-36 and so forth.
  */
  yPosTenT = 353 - yPosTenT;
  smVertNumTenT.pushToSprite(&background, 191, yPosTenT);
  // Work on the thousands position, x is now 219
  int yPosThou = topPos[thou];
  yPosThou = 353 - yPosThou;
  smVertNumThou.pushToSprite(&background, 218, yPosThou);
  int yPosHund = topPos[hund];
  yPosHund = 353 - yPosHund;
  smVertNumThou.pushToSprite(&background, 244, yPosHund);
  int yPosTens = topPos[tens];
  yPosTens = 353 - yPosTens;
  smVertNumThou.pushToSprite(&background, 270, yPosTens);
}

// Draw the screen using an image stack of sprites.
// Sprites are manipulated with TFT_eSPI and then 
// pushed to the screen using the GFX library.
void drawAltimeter(int altitude, int barometric, int angleBug) {
  // Start the image stack with the background
  background.setColorDepth(colorDepth);
  background.createSprite(480, 480);
  // This pivot point sets where the needle will be centered
  background.setPivot(242, 240);
  background.fillSprite(TFT_BLACK);
  // Now place the larget altitude digits
  drawLgVertNums(getDigit(altitude, 5), getDigit(altitude, 4));
  // Now place the small barometric digits
  drawSmVertNums(getDigit(barometric, 4), getDigit(barometric, 3), getDigit(barometric, 2), getDigit(barometric, 1));
  // Place the gauge face, it has transparency
  gaugeFace.pushToSprite(&background, 0, 0, 0xffff);
  // Get needle angle
  int currNeedleAngle = getNeedleAngle(altitude);
  // Now push the rotated needle image
  needle.pushRotated(&background, currNeedleAngle, 0xffff);
  // Push the rotated altimeter bug
  bug.pushRotated(&background, angleBug, 0xffff);  // push the rotate bug
  // Write the stacked image to the screen
  gfx->draw16bitBeRGBBitmap (0, 0, (uint16_t *)background.getPointer(), 480, 480);
  // Clean up the sprite as we will need to make a new one on the next loop
  background.deleteSprite();
}

// Query Axis and Ohs for the data required for this instrument.
// We will pull plane name, altitude, instrument pressure, and bug value.
// For the altimeter there is no plane-specific code being run since
// none of the variables being queried are unique to a plane. The code
// is there for such manipulation as an example.
void getData() {
  // Send JSON request
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverUrl); // Specify the server URL
    http.addHeader("Content-Type", "application/json"); // Set content type to JSON

    // Create JSON object
    JsonDocument jsonDoc;
    // Create the 'getvars' array
    JsonArray getvars = jsonDoc.createNestedArray("getvars");
    // Ask for indicated altitude - not plane specific
    JsonObject altitudeObj = getvars.createNestedObject();
    altitudeObj["var"] = "(A:INDICATED ALTITUDE, feet)";
    altitudeObj["value"] = 0.0;
    // Ask for current barometer pressure setting - not plane specific
    JsonObject pressureObj = getvars.createNestedObject();
    pressureObj["var"] = "(A:KOHLSMAN SETTING HG, inHg)";
    pressureObj["value"] = 0.0;
    // Ask for current altimeter bug value - not plane specific
    JsonObject bugObj = getvars.createNestedObject();
    // This is a local AAO var, so no comma and variable type ", Number"
    bugObj["var"] = "(L:AAO_Altitude_Bug)";
    bugObj["value"] = 0.0;

    // Create the 'getstringvars' array
    JsonArray getstringvars = jsonDoc.createNestedArray("getstringvars");
    // Ask for the plane name for plane specific code management
    JsonObject titleObj = getstringvars.createNestedObject();
    titleObj["var"] = "(A:TITLE, String)";
    titleObj["value"] = "";

    /*
      Below not needed for this, a pulllvars will set the variable
      to zero to reduce redundant data transmission and processing.

    // Create "pulllvars" array and add a variable object
    JsonArray pulllvars = jsonDoc.createNestedArray("pulllvars");
    JsonObject bugObj = pulllvars.createNestedObject();
    // This is a local AAO var, so no comma and variable type ", Number"
    bugObj["var"] = "(L:AAO_Altitude_Bug)";
    bugObj["value"] = 0.0;
    */

    // Serialize JSON to string
    String jsonString;
    serializeJson(jsonDoc, jsonString);
    //Serial.println(jsonString);

    // Send POST request
    int httpResponseCode = http.POST(jsonString);

    // Handle response
    if (httpResponseCode > 0) {
      // No error
      String response = http.getString();
      // Show raw response
      //Serial.println("Response:");
      //Serial.println(response);

      // Now deserialize it
      JsonDocument jsonResponse; // Adjust size as needed
      DeserializationError error = deserializeJson(jsonResponse, response);
      if (error) {
        // This is a fatal error. Set error condition to 4444
        // Error deserializing
        //Serial.print("JSON Deserialization failed: ");
        //Serial.println(error.c_str());
        indicatedAltitude = 0;
        barPressure = 4444;
        bugAngle = 0;
        return;
      } else {
        // Extract the values from getvars (altitude, pressure, alt bug)
        JsonArray getvarsResponse = jsonResponse["getvars"];
        // Extract the name of the plane from getstringvars
        // This should be unique to livery, e.g. Pilatus PC-12/47 Fly7 OH-DEN
        JsonArray getstringvarsResponse = jsonResponse["getstringvars"];
        // Extract and reset any custom Lvars, not used in this instrument
        //JsonArray pullLvarsResponse = jsonResponse["pulllvars"];
        
        // Only makes sense to show data if we can see we are in a plane
        // and the plane name is the only string variable we request so it
        // needs to not be null and be bigger than nothing.
        if (!getstringvarsResponse.isNull() && getstringvarsResponse.size() > 0) {
          // All planes use these values for altitude and the main barometric pressure
          if (!getvarsResponse.isNull() && getvarsResponse.size() > 0) {
            // Convert to an int and it'll truncate the unecessary decimals from the sim
            indicatedAltitude = getvarsResponse[0]["value"].as<int>();
            //Serial.print("Indicated Altitude: ");
            //Serial.println(indicatedAltitude);
            rawPressure = getvarsResponse[1]["value"];
            //Serial.print("Raw Pressure: ");
            //Serial.println(rawPressure);
            // Convert to a four digit value for use, comes as XX.XXXXXX
            barPressure = round(rawPressure * 100);
            //Serial.print("inHg Pressure: ");
            //Serial.println(barPressure);
            int currBugAlt = getvarsResponse[2]["value"].as<int>();
            //Serial.print("Bug Altitude: ");
            //Serial.println(currBugAlt);
            // Convert to angle, maps value from 0<= x <=999 to 0<= y <=359
            bugAngle = map(currBugAlt, 0, 999, 0, 359);
            /*
              The map function does not constrain so if an out of range
              value is sent, it will just extrapolate. While the AAO
              scripts should constrain it, we need to do sanity checking
              since the rotation value won't make sense for such out of
              range values.
            */
            if (bugAngle < 0 || bugAngle > 999) {
              bugAngle = 0;
              // This is a non-fatal error but we should be told we are
              // getting altitude values beyond what makes sense.
              barPressure = 8888;
            }
          } else {
            /*
              We didn't get any data so set default and a pressure 
              value of 6666 to notify as an error code.
              This is a fatal error for this instrument.
            */
            //Serial.println("getvars is null or empty");
            indicatedAltitude = 0;
            barPressure = 6666;
            bugAngle = 0;
            // Free resources
            http.end();
            return;
          }
          
          /*
            Plane specific code: I don't know what plane specific
            features there could be for an altimeter, but below
            provides the idea on how you can process planes differently.
            The Pilatus has an altimeter bug so one could sync that
            bug with this one bug doing so here means you have to http
            json send it back and that makes no sense when you can do
            that update locally in AAO using PC horsepower.
          */
          aircraftTitle = getstringvarsResponse[0]["value"].as<String>();
          //Serial.println("Aircraft Title: " + aircraftTitle);
          if (aircraftTitle.length() == 0) {
            // We somehow got here where our string variable isn't empty
            // but the aircraft title has zero length. Non-fatal error so
            // let's just notify with a 3333.
            barPressure = 3333;
          }else {
            if (aircraftTitle.startsWith("Pilatus PC-12/47")) {
              // Do stuff for the pilatus
            }
          }
        } else {
          /*
            In this situation, we have not gotten an airplane name
            from the sim. So to notify us of that state we will draw
            the altimeter with barometric values of 5555 as an error code.
            This is considered to be a fatal error for the instrument.
          */
          //Serial.println("getstringvars is null or empty");
          indicatedAltitude = 0;
          barPressure = 5555;
          bugAngle = 0;
          // Free resources
          http.end();
          return;
        }
      }
    } else {
      // Error on http request, set error 7777
      // This is a fatal error for this instrument.
      //Serial.print("Error on sending POST: ");
      //Serial.println(httpResponseCode);
      indicatedAltitude = 0;
      barPressure = 7777;
      bugAngle = 0;
      // Free resources
      http.end();
      return;
    }

    // Free resources
    http.end();
  }else {
    // Wi-Fi not connected fix wifi and reboot/cycle power on ESP32
    // Provide error code 9999
    indicatedAltitude = 0;
    barPressure = 9999;
    bugAngle = 0;
  }
}

void setup() {
  Serial.begin(115200);

  // Set WiFi mode to Station
  WiFi.mode(WIFI_STA);
  // Start connecting to the WiFi network
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi...");

  // Wait until connected
  while (WiFi.status() != WL_CONNECTED) {
  delay(1000);
    Serial.print(".");
  }

  // Print the local IP address once connected
  Serial.println("\nConnected!");
  Serial.print("ESP32 IP Address: ");
  Serial.println(WiFi.localIP());

  // Initialize graphics
  #ifdef GFX_EXTRA_PRE_INIT
  GFX_EXTRA_PRE_INIT();
  #endif
  
  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }

  // Below is for transparency and color
  lgVertNumTenT.setSwapBytes(true);
  lgVertNumThou.setSwapBytes(true);
  smVertNumTenT.setSwapBytes(true);
  smVertNumThou.setSwapBytes(true);
  smVertNumHund.setSwapBytes(true);
  smVertNumTens.setSwapBytes(true);
  gaugeFace.setSwapBytes(true);
  needle.setSwapBytes(true);
  bug.setSwapBytes(true);

  // Setup properties that don't change
  // Gauge Face
  gaugeFace.setColorDepth(colorDepth);
  gaugeFace.createSprite(480, 480);
  gaugeFace.pushImage(0, 0, 480, 480, gaugeFaceImg);
  // Ten-thousandths altitude
  lgVertNumTenT.setColorDepth(colorDepth);
  lgVertNumTenT.createSprite(32, 462); 
  lgVertNumTenT.pushImage(0, 0, 32, 462, lgVertNumImg);
  // Thousandths altitude
  lgVertNumThou.setColorDepth(colorDepth);
  lgVertNumThou.createSprite(32, 462);
  lgVertNumThou.pushImage(0, 0, 32, 462, lgVertNumImg);
  // Ten-thousandths barometric
  smVertNumTenT.setColorDepth(colorDepth);
  smVertNumTenT.createSprite(25, 360); 
  smVertNumTenT.pushImage(0, 0, 25, 360, smVertNumImg);
  // Ten-thousandths barometric
  smVertNumThou.setColorDepth(colorDepth);
  smVertNumThou.createSprite(25, 360); 
  smVertNumThou.pushImage(0, 0, 25, 360, smVertNumImg);
  // Ten-thousandths barometric
  smVertNumHund.setColorDepth(colorDepth);
  smVertNumHund.createSprite(25, 360); 
  smVertNumHund.pushImage(0, 0, 25, 360, smVertNumImg);
  // Ten-thousandths barometric
  smVertNumTens.setColorDepth(colorDepth);
  smVertNumTens.createSprite(25, 360); 
  smVertNumTens.pushImage(0, 0, 25, 360, smVertNumImg);
  // Needle
  needle.setColorDepth(colorDepth);
  needle.createSprite(32, 377);
  needle.setPivot(16, 188);
  needle.pushImage(0, 0, 32, 377, needleImg);
  // Bug
  bug.setColorDepth (colorDepth);
  bug.createSprite (25, 480);
  bug.setPivot (12, 240);
  bug.pushImage (0, 0, 25, 480, altBug);
}

/*
  Performance metric variables used for determining
  the fastest speed the loop can run. In theory, the
  display can do 30fps with the custom Qualia ESP32.
  So any value less than 33ms is not useful. When I
  run the test where no data is pulled and it is just
  drawing the screen, I get 179-ish milliseconds. That
  means the ESP32 processing of the image is the
  bottleneck, likely due to stacking and moving the
  images.
*/
//unsigned long prevLoopTime = 0;
//unsigned long lastAverageUpdate = 0;
//unsigned long loopDurationSum = 0;
//unsigned int loopCount = 0;

// Main loop
void loop() {
  // Performance metric code
  /*
  unsigned long currentLoopTime = millis();  // Current loop time
  unsigned long loopDuration = currentLoopTime - prevLoopTime;  // How long it took to loop
  prevLoopTime = currentLoopTime; // Update the previous loop time for next loop
  loopDurationSum += loopDuration; // Accumulate the total time
  loopCount++; // Accumulate the number of loops run
  // Do a 5 second average of loop time
  if (currentLoopTime - lastAverageUpdate >= 5000) {
    if (loopCount > 0) {
      barPressure = loopDurationSum / loopCount; // Compute average
      barPressure = barPressure % 10000; // Force 0–9999 range
    } else {
      barPressure = 0; // Safety fallback
    }
    // Reset counters for the next interval
    loopDurationSum = 0;
    loopCount = 0;
    lastAverageUpdate = currentLoopTime;
  }
  */

  // Get and process data from AAO and the Sim
  // Comment this line out if testing raw speed/output
  getData();
  // Draw the screen
  //Serial.println("Calling drawAltimeter with --- Alt: " + String(indicatedAltitude) + "  Press: " + String(barPressure) + "  Bug: " + String(bugAngle));
  drawAltimeter(indicatedAltitude, barPressure, bugAngle);
  /*
    This is a 5fps framerate and from testing the ESP32 can't
    go faster than about 180ms. The example from the AAO
    documentation also has a 200ms as a reference, so we'll
    just stick with that since we aren't doing better anyway.
    At some level it doesn't matter so much on a lagging
    instrument anyway, except for how smooth the framerate is.
  */
  delay(200);
}
