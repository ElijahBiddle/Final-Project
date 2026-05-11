//ELIJAH BIDDLE
#include <Wire.h>
#include "RTClib.h"
#include <Keypad.h>
#include <LiquidCrystal.h>

RTC_DS1307 rtc;

// 0 = OFF  1 = IDLE  2 = PASSWORD PROMPT  3 = ERROR
int state     = 0;
int errorType = 0;
// errorType 1 = lockout, errorType 2 = sensor fail

bool systemReady = false;
unsigned long bootTime = 0;
#define BOOT_DELAY_MS 60000UL   // 1-minute millis-based


//volatile modified inside an ISR, must not be cached by the compiler
volatile bool onBtnPressed = false;

unsigned long lastOffPress   = 0;
unsigned long lastResetPress = 0;
#define DEBOUNCE_MS 200


#define BUZZER 26

unsigned long lastMotion = 0;
unsigned long idleStart  = 0;
const int threshold = 80; // cm


LiquidCrystal lcd(12, 11, 5, 4, 3, 2);


const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {27, 29, 31, 33};
byte colPins[COLS] = {35, 37, 39, 41};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

String input    = "";
String password = "1234";
int failCount   = 0;
#define MAX_FAILS 3

bool feedbackActive  = false;
bool feedbackGranted = false;
bool forceLCDRedraw  = false;
unsigned long feedbackStart   = 0;
#define FEEDBACK_MS 1500


void serialBegin(unsigned long baud)
{
    // UBRR = (F_CPU / (16 * baud)) - 1
    unsigned int ubrr = F_CPU / 16 / baud - 1;
    UBRR0H = (unsigned char)(ubrr >> 8);
    UBRR0L = (unsigned char)(ubrr);
    UCSR0B = (1 << TXEN0);                      // enable transmitter only
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);    // 8-bit data, 1 stop bit
}

void serialWriteByte(uint8_t data)
{
    while (!(UCSR0A & (1 << UDRE0)));  // wait for transmit buffer to empty
    UDR0 = data;
}

void serialPrint(const char* str)
{
    while (*str) serialWriteByte((uint8_t)*str++);
}

void serialPrintInt(int num)
{
    if (num < 0) { serialWriteByte('-'); num = -num; }
    if (num >= 10) serialPrintInt(num / 10);
    serialWriteByte('0' + (num % 10));
}

void serialPrintln(const char* str)
{
    serialPrint(str);
    serialWriteByte('\r');
    serialWriteByte('\n');
}


void onBtnISR()
{
    onBtnPressed = true;
}

void setup()
{
    serialBegin(9600);

    Wire.begin();
    rtc.begin();
    //Input pins clear DDR bit (input) set PORT bit (pullup)

    DDRD  &= ~(1 << 3);
    PORTD |=  (1 << 3);

    DDRB  &= ~(1 << 4);
    PORTB |=  (1 << 4);

    DDRB  &= ~(1 << 3);
    PORTB |=  (1 << 3);
    DDRA  &= ~(1 << 0);

    DDRA  |=  (1 << 1);
    PORTA &= ~(1 << 1);

    DDRA  |=  (1 << 4);
    PORTA &= ~(1 << 4);

    DDRH  |=  (1<<3)|(1<<4)|(1<<5)|(1<<6);
    PORTH &= ~((1<<3)|(1<<4)|(1<<5)|(1<<6));

    attachInterrupt(digitalPinToInterrupt(18), onBtnISR, FALLING);

    lcd.begin(16, 2);
    lcd.clear();
    lcd.print("SYSTEM BOOT");

    state     = 0;
    errorType = 0;
    failCount = 0;

    bootTime    = millis();
    systemReady = false;
}

// RTC STATE LOG 
void logState(int newState, int newErrorType)
{
    DateTime now = rtc.now();

    serialWriteByte('[');
    if (now.hour()   < 10) serialWriteByte('0');
    serialPrintInt(now.hour());
    serialWriteByte(':');
    if (now.minute() < 10) serialWriteByte('0');
    serialPrintInt(now.minute());
    serialWriteByte(':');
    if (now.second() < 10) serialWriteByte('0');
    serialPrintInt(now.second());
    serialPrint("]  STATE -> ");

    if      (newState == 0) serialPrint("OFF");
    else if (newState == 1) serialPrint("IDLE");
    else if (newState == 2) serialPrint("PASSWORD PROMPT");
    else if (newState == 3)
    {
        if (newErrorType == 1) serialPrint("ERROR - LOCKOUT");
        else                   serialPrint("ERROR - SENSOR FAIL");
    }

    serialWriteByte('\r');
    serialWriteByte('\n');
}

// Replaces all digitalWrite() calls on LED pins with direct PORTH writes
void updateLEDs()
{
    PORTH &= ~((1<<3)|(1<<4)|(1<<5)|(1<<6)); // all LEDs off

    if      (state == 0) PORTH |= (1<<3); // RED
    else if (state == 1) PORTH |= (1<<4); // BLUE
    else if (state == 2) PORTH |= (1<<5); // GREEN
    else if (state == 3) PORTH |= (1<<6); // YELLOW
}

//lcd
void updateLCD()
{
    static int lastState    = -1;
    static int lastError    = -1;
    static int lastInputLen = -1;

    int currentInputLen = input.length();

    if (state == lastState && errorType == lastError && currentInputLen == lastInputLen && !forceLCDRedraw) return;

    lastState    = state;
    lastError    = errorType;
    lastInputLen = currentInputLen;
    forceLCDRedraw = false;

    lcd.clear();

    if (state == 0)
    {
        lcd.setCursor(0, 0);
        lcd.print("OFF");
    }
    else if (state == 1)
    {
        lcd.setCursor(0, 0);
        lcd.print("BIDDLE");
        lcd.setCursor(6, 1);
        lcd.print("INDUSTRIES");
    }
    else if (state == 2)
    {
        lcd.setCursor(0, 0);
        lcd.print("ENTER PASSWORD:");
        lcd.setCursor(0, 1);
        for (int i = 0; i < currentInputLen; i++) lcd.print("*");
    }
    else if (state == 3)
    {
        if (errorType == 1)
        {
            lcd.setCursor(0, 0);
            lcd.print("ACCESS DENIED");
            lcd.setCursor(0, 1);
            lcd.print("LOCKOUT");
        }
        else
        {
            lcd.setCursor(0, 0);
            lcd.print("SENSOR ERROR");
            lcd.setCursor(0, 1);
            lcd.print("SYSTEM FAIL");
        }
    }
}

// loop() watches the timer and does the state transition once time is up.
void checkPassword()
{
    if (input == password)
    {
        tone(BUZZER, 2000, 500);
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("ACCESS GRANTED");

        feedbackGranted = true;
    }
    else
    {
        failCount++;
        tone(BUZZER, 300, 500);
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("WRONG PASSWORD");
        lcd.setCursor(0, 1);
        lcd.print("Attempt ");
        lcd.print(failCount);
        lcd.print("/");
        lcd.print(MAX_FAILS);

        feedbackGranted = false;
    }

    input          = "";
    feedbackActive = true;
    feedbackStart  = millis();
}

void loop()
{
    if (!systemReady)
    {
        updateLEDs();
        updateLCD();

        if (millis() - bootTime >= BOOT_DELAY_MS)
        {
            systemReady = true;
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("OFF");
        }
        return;
    }
    if (feedbackActive)
    {
        if (millis() - feedbackStart >= FEEDBACK_MS)
        {
            feedbackActive  = false;
            forceLCDRedraw  = true; // screen was written by checkPassword(), not updateLCD()

            if (feedbackGranted)
            {
                failCount = 0;
                state     = 1;
                idleStart = millis();
                logState(state, errorType);
            }
            else if (failCount >= MAX_FAILS)
            {
                state     = 3;
                errorType = 1;
                logState(state, errorType);
            }
        }
        return;
    }

    long distance = readDistance();

    if (state == 2)
    {
        char key = keypad.getKey();

        if (key)
        {
            lastMotion = millis(); // any keypress resets idle timeout

            if (key != '*' && key != '#')
            {
                tone(BUZZER, 1200, 80);
                input += key;
            }
            else if (key == '*')
            {
                tone(BUZZER, 400, 200);
                input = "";
            }
            else if (key == '#')
            {
                checkPassword();
                if (feedbackActive) { updateLEDs(); return; }
            }
        }
    }

    if (onBtnPressed)
    {
        onBtnPressed = false;
        if (state == 0)
        {
            state     = 1;
            idleStart = millis();
            logState(state, errorType);
        }
    }

    unsigned long now = millis();

    if (!(PINB & (1 << 4)) && now - lastOffPress > DEBOUNCE_MS)
    {
        lastOffPress = now;
        if (state == 1 || state == 2)
        {
            state     = 0;
            input     = "";
            failCount = 0;
            logState(state, errorType);
        }
    }

    if (state == 3 && !(PINB & (1 << 3)) && now - lastResetPress > DEBOUNCE_MS)
    {
        lastResetPress = now;
        state     = 1;
        errorType = 0;
        failCount = 0;
        input     = "";
        idleStart = millis();
        logState(state, errorType);
    }
    static int badReads = 0;

    if (distance == -1) badReads++;
    else badReads = 0;

    if (badReads > 5 && state != 3)
    {
        state     = 3;
        errorType = 2;
        logState(state, errorType);
    }

    if (state == 1)
    {
        if (distance > 0 && distance < threshold)
        {
            state      = 2;
            lastMotion = millis();
            input      = "";
            logState(state, errorType);
        }
    }

    if (state == 2)
    {
        if (distance > 0 && distance < threshold)
        {
            lastMotion = millis();
        }

        if (millis() - lastMotion > 5000)
        {
            state     = 1;
            idleStart = millis();
            input     = "";
            logState(state, errorType);
        }
    }

    //buzzer
    if (state == 3)
    {
        tone(BUZZER, 1000);
    }
    else if (state == 0 || state == 1)
    {
        noTone(BUZZER);
    }

    updateLEDs();
    updateLCD();
}

long readDistance()
{
    // TRIG = PA1
    PORTA &= ~(1 << 1);      // TRIG LOW
    delayMicroseconds(2);
    PORTA |=  (1 << 1);      // TRIG HIGH
    delayMicroseconds(10);
    PORTA &= ~(1 << 1);      // TRIG LOW

    // ECHO = pin 22 — pulseIn still needs the Arduino pin number
    long duration = pulseIn(22, HIGH, 30000);

    if (duration == 0) return -1;
    return duration * 0.034 / 2;
}
