//
// K1FM-WSPR-TX
//
// Code based on JTEncode by Jason Milldrum as well as
// on TinyGPSPlus by Mikal Hart
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject
// to the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
// CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//


#include <TinyGPS++.h>
#include <SoftwareSerial.h>
#include <si5351.h>
#include <JTEncode.h>

#define WSPR_TONE_SPACING       146           // ~1.46 Hz
#define WSPR_DELAY              683          // Delay value for WSPR
#define WSPR_DEFAULT_FREQ       14095600UL

#define BUTTON_PIN              6
#define RED_LED_PIN             8
#define YELLOW_LED_PIN          9
#define GREEN_LED_PIN           10

#define DEBUG                   true

Si5351 si5351;
JTEncode jtencode;

static const int RXPin = 4, TXPin = 3;
static const uint32_t GPSBaud = 9600;

// The TinyGPS++ object
TinyGPSPlus gps;

// The serial connection to the GPS device
SoftwareSerial ss(RXPin, TXPin);

// Pre-programmed WSPR Frequencies
uint32_t frequencies[5] = { 14095600, 10138700, 7038600, 18104600, 21094600  };
int current_frequency = 0;

// Callsign and locator (Eg. K1FM - FN30)
char call[] = "MYCALL";
char loc[] = "AA00";

// Calibration value (use https://github.com/etherkit/Si5351Arduino/tree/master/examples/si5351_calibration )
int calvalue = -3850;

// Power output (21 dBm ~ 120 mW) 
uint8_t dbm = 21;

// Audio frequency to use
int32_t freq_audio = 1550;

uint8_t tx_buffer[255];
uint8_t symbol_count;
uint16_t tone_delay, tone_spacing;

void setup()
{
  if (DEBUG) Serial.println(F("Starting"));
  si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, calvalue);

  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);

  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(YELLOW_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  symbol_count = WSPR_SYMBOL_COUNT;
  tone_spacing = WSPR_TONE_SPACING;
  tone_delay = WSPR_DELAY;

  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_8MA); // Set for max power if desired
  si5351.output_enable(SI5351_CLK0, 0); // Disable the clock initially

  set_tx_buffer();

  Serial.begin(115200);
  ss.begin(GPSBaud);

  all_leds_on();
  delay(2000);
  all_leds_off();
}

void nextFrequency() {
  current_frequency = ++current_frequency % 5;
  if (DEBUG) Serial.print(F("Now working on "));
  if (DEBUG) Serial.println(frequencies[current_frequency]);
  int i;
  for (i = 0; i <= current_frequency; i++) {
    all_leds_on();
    smartDelay(300);
    all_leds_off();
    smartDelay(300);
  }
}

unsigned short measureButtonTime() {
  int buttonState = digitalRead(BUTTON_PIN);
  unsigned long start = millis();
  while (buttonState == LOW) {
    buttonState = digitalRead(BUTTON_PIN);
  }
  unsigned long duration = millis() - start;
  return duration;
}

void loop()
{
  all_leds_off();
  digitalWrite(GREEN_LED_PIN, HIGH);

  int buttonState = digitalRead(BUTTON_PIN);
  bool forceTransmit = false;

  if (buttonState == LOW) {
    unsigned long duration = measureButtonTime();
    if (DEBUG) Serial.println(F("Button pressed!"));
    if (DEBUG) Serial.println(duration);
    if (duration < 500) {
      forceTransmit = true;
      digitalWrite(YELLOW_LED_PIN, LOW);
    } else {
      nextFrequency();
    }
  }

  if (gps.location.isValid()) {
    digitalWrite(YELLOW_LED_PIN, HIGH);
    calcLocator(loc, gps.location.lat(), gps.location.lng());
    if (DEBUG) {
      Serial.print(F("calculated locator: "));
      Serial.println(loc);
    }
  }

  if (forceTransmit) { // !gps.location.isValid()
    transmit_loop();
  } else { // Invalid Fix
    if (DEBUG) {
      printFloat(gps.location.lat(), gps.location.isValid(), 11, 6);
      printFloat(gps.location.lng(), gps.location.isValid(), 12, 6);
      printDateTime(gps.date, gps.time);
      Serial.print(gps.satellites.value());
      digitalWrite(YELLOW_LED_PIN, LOW);
      Serial.println(F(" Invalid fix"));
    }

    TinyGPSTime t = gps.time;
    TinyGPSDate d = gps.date;
    uint8_t seconds = t.second();
    uint8_t minutes = t.minute();
    uint16_t year   = d.year();

    if (year > 2000 and year < 2080) { // The time is correct
      digitalWrite(GREEN_LED_PIN, HIGH);
      if ((minutes % 2 == 0) and seconds <= 1) {
        transmit();
        smartDelay(1000);
      }
    } else { // No valid time
      digitalWrite(GREEN_LED_PIN, !digitalRead(GREEN_LED_PIN)); // Blink
    }
  }

  smartDelay(200);

  if (millis() > 5000 && gps.charsProcessed() < 10)
    Serial.println("No GPS data received: check wiring");
}

void transmit()
{
  Serial.println(F("Transmit"));
  ss.end();
  si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, calvalue);
  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_8MA); // Set for max power if desired
  si5351.output_enable(SI5351_CLK0, 0); // Disable the clock initially
  set_tx_buffer();
  encode();
  ss.begin(GPSBaud);
}

void transmit_loop()
{
  while (true) {
    transmit();
    smartDelay(8200);
  }
}

void encode()
{
  uint8_t i;

  // Reset the tone to the base frequency and turn on the output
  si5351.output_enable(SI5351_CLK0, 0);
  // si5351.output_enable(SI5351_CLK0, 1);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, HIGH);

  for (i = 0; i < symbol_count; i++)
  {
    si5351.set_freq( (frequencies[current_frequency] * 100) + (freq_audio * 100) + (tx_buffer[i] * tone_spacing), SI5351_CLK0);
    delay(tone_delay);
  }

  // Turn off the output
  si5351.output_enable(SI5351_CLK0, 0);
  digitalWrite(RED_LED_PIN, LOW);
}

void set_tx_buffer()
{
  // Clear out the transmit buffer
  memset(tx_buffer, 0, 255);
  jtencode.wspr_encode(call, loc, dbm, tx_buffer);
}

void calcLocator(char *dst, double lat, double lon) {
  int o1, o2, o3;
  int a1, a2, a3;
  double remainder;
  // longitude
  remainder = lon + 180.0;
  o1 = (int)(remainder / 20.0);
  remainder = remainder - (double)o1 * 20.0;
  o2 = (int)(remainder / 2.0);
  remainder = remainder - 2.0 * (double)o2;
  o3 = (int)(12.0 * remainder);

  // latitude
  remainder = lat + 90.0;
  a1 = (int)(remainder / 10.0);
  remainder = remainder - (double)a1 * 10.0;
  a2 = (int)(remainder);
  remainder = remainder - (double)a2;
  a3 = (int)(24.0 * remainder);
  dst[0] = (char)o1 + 'A';
  dst[1] = (char)a1 + 'A';
  dst[2] = (char)o2 + '0';
  dst[3] = (char)a2 + '0';
  dst[4] = (char)0;
  //dst[4] = (char)o3 + 'A';
  //dst[5] = (char)a3 + 'A';
  //dst[6] = (char)0;
}


// This custom version of delay() ensures that the gps object
// is being "fed".
static void smartDelay(unsigned long ms)
{
  unsigned long start = millis();
  do
  {
    while (ss.available())
      gps.encode(ss.read());
  } while (millis() - start < ms);
}

static void printFloat(float val, bool valid, int len, int prec)
{
  if (!valid)
  {
    while (len-- > 1)
      Serial.print('*');
    Serial.print(' ');
  }
  else
  {
    Serial.print(val, prec);
    int vi = abs((int)val);
    int flen = prec + (val < 0.0 ? 2 : 1); // . and -
    flen += vi >= 1000 ? 4 : vi >= 100 ? 3 : vi >= 10 ? 2 : 1;
    for (int i = flen; i < len; ++i)
      Serial.print(' ');
  }
  smartDelay(0);
}

static void printInt(unsigned long val, bool valid, int len)
{
  char sz[32] = "*****************";
  if (valid)
    sprintf(sz, "%ld", val);
  sz[len] = 0;
  for (int i = strlen(sz); i < len; ++i)
    sz[i] = ' ';
  if (len > 0)
    sz[len - 1] = ' ';
  Serial.print(sz);
  smartDelay(0);
}

static void printDateTime(TinyGPSDate &d, TinyGPSTime &t)
{
  if (!d.isValid())
  {
    Serial.print(F("********** "));
  }
  else
  {
    char sz[32];
    sprintf(sz, "%02d/%02d/%02d ", d.month(), d.day(), d.year());
    Serial.print(sz);
  }

  if (!t.isValid())
  {
    Serial.print(F("******** "));
  }
  else
  {
    char sz[32];
    sprintf(sz, "%02d:%02d:%02d ", t.hour(), t.minute(), t.second());
    Serial.print(sz);
  }

  printInt(d.age(), d.isValid(), 5);
  smartDelay(0);
}

static void printStr(const char *str, int len)
{
  int slen = strlen(str);
  for (int i = 0; i < len; ++i)
    Serial.print(i < slen ? str[i] : ' ');
  smartDelay(0);
}

static void all_leds_off()
{
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(YELLOW_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);
}

static void all_leds_on()
{
  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(YELLOW_LED_PIN, HIGH);
  digitalWrite(GREEN_LED_PIN, HIGH);
}

