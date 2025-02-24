/*
 * File:   main.c
 * Author: 
 *
 * Created on 04.04.2022., 16.27
 */


#include <avr/io.h>
#include <avr/interrupt.h>

#define BUTTON_1_SHORT 1
#define BUTTON_1_LONG  2
#define BUTTON_2       3
#define BUTTON_3       4
#define BUTTON_1_HOLD  5
#define TIMEOUT      1000
#define LONG_TIMEOUT 2000U

// MODE FUNCTIONS

void displayTimeMode();
void setTime(unsigned char CA);
void setTimeMode();
void setAlarmMode();

// INPUT FUNCTIONS
unsigned char shortButtonRead();
unsigned char longButtonRead();

// RESTORE FUNCTIONS
void saveState();
void restore();

// ALARM FUNCTIONS
void alarmHandling();
void driveAlarm();
void enablePWMoutput();
void disablePWMoutput();

void eepromWrite(unsigned int addr, unsigned char byte);
volatile unsigned char eepromWriteFlag;

enum CLOCK_STATES
{
    TIME_DISPLAY,
    TIME_SET,
    ALARM_SET,
};

#define LCHCLK_HIGH() PORTD |= 0x10
#define LCHCLK_LOW() PORTD &=0xEF

#define SDI_HIGH() PORTB |= 0x01
#define SDI_LOW() PORTB &= 0xFE


#define SHTCLK_HIGH() PORTD |= 0x80
#define SHTCLK_LOW() PORTD &= 0x7F

#define ALARM_ACTIVE_LED_ON()  PORTB &= 0xF7;
#define ALARM_ACTIVE_LED_OFF() PORTB |= 0x08;

volatile uint8_t displayNum;
volatile unsigned short miliseconds;
volatile unsigned char seconds;
volatile unsigned char minutes;
volatile unsigned char hours;
volatile unsigned char blinkMiddle;
volatile unsigned char pm;
volatile signed char hoursToSet;
volatile signed char minutesToSet;
volatile unsigned char alarmHours;
volatile unsigned char alarmMinutes;
volatile unsigned int ms;
volatile unsigned char nextDay;
unsigned char possibleAlarmSet;
unsigned char alarmActive;
unsigned char alarmSilenced;
unsigned int alarmActiveTime;
unsigned int buzzerPeriod;
unsigned char returnFromSettings;

unsigned char digitMaps[] = {0xC0,0xF9,0xA4,0xB0,0x99,0x92,0x82,0xF8,0X80,0X90};
unsigned char digits[4] = {1, 2, 3, 4};
unsigned char dNum[] = {0xF1, 0xF2, 0xF4, 0xF8};
unsigned char eachDisplay[4];

volatile unsigned char dutyCycle;
volatile unsigned char pwmCounter;
unsigned char ledsOn;
unsigned char alarmSet;


struct dStates
{
    unsigned char hh : 1;
    unsigned char ss : 1;
    unsigned char ts : 1;
    unsigned char as : 1;
    unsigned char al : 1;
};

typedef struct dStates dStates;
volatile dStates displayStates;

struct dSubStates
{
    unsigned char f24 : 1;
    unsigned char hh : 1;
    
};

typedef struct dSubStates dSubStates;
volatile dSubStates displaySubStates;

unsigned char readPot();

void TIMER0setup()
{
    // CTC mode
    TCCR0A = 0x02;  
    // 64 prescaler, 16MHz/64 = 250MHz, 1/250MHz = 0.000004s per tick,
    //0.001/000004=250 ticks for 1Ms, OCR0A = 250
    TCCR0B = 0x03;
    TCNT0 = 0;
    // When TCNT0 reaches 250 interrupt will ocure
    OCR0A = 250;
    OCR0B = 0;
    // Enable Timer0 interrupt
    TIMSK0 = 0x02;
    TIFR0 = 0;
}

void TIMER1setup()
{
    TCCR1A = 0x00;
    TCCR1B = 0x0A; // 8 prescaler CTC mode
    OCR1A = 312;   // 200 -> 0.0001s
    TIMSK1 = 0x02; // enable OCR1A match interrupt
}

void TIMER2setup()
{
    TCCR2A = 0x02;
    TCCR2B = 0x05;
    OCR2A  =  125;     
}

void configureADC()
{
    ADMUX = 0x60;
    ADCSRA = 0x86;
}

void driveSDI(unsigned char x)
{
    if(x)
    {
        SDI_HIGH();
    }
    else
    {
        SDI_LOW();
    }
}

void clearShiftRegisters()
{
    LCHCLK_LOW();
    for(int i = 0; i < 16; i++)
    {
        
        for(i = 0; i < 8; i++)
        {
            driveSDI(!!(dNum[displayNum] & (1 << (7 - i))));
            SHTCLK_LOW();
        
             SHTCLK_HIGH();
        }
    }
    LCHCLK_HIGH();
}

void clockDataIn(unsigned char data)
{
    int i;
    LCHCLK_LOW();
    for(i = 0; i < 8; i++)
    {
        driveSDI(!!(data & (1 << (7 - i))));
        
        SHTCLK_LOW();
        
        SHTCLK_HIGH();
    }
    for(i = 0; i < 8; i++)
    {
        driveSDI(!!(~dNum[displayNum] & (1 << (7 - i))));
        SHTCLK_LOW();
        
        SHTCLK_HIGH();
    }
    LCHCLK_HIGH();
}



void pinSetup()
{
    // SET PB0 as output, DATA INPUT SDI
    DDRB |= 0x01;
    // SET PD7 and PD4 as outputs, SHTCLK LCHCLK, SHIFT CLOCK LATCH CLOCK
    DDRD |= 0x98; // PD3 output, beeper drive
    PORTD |= 0x08; // beeper OFF
    
    DDRC &= 0xF0;
    
    DDRB |= 0x0C;   // D3 D4 pins
    PORTB |= 0x0C;  // turn D3 D4 off 
}

void clockOrAlarmSet()
{
    if(displaySubStates.hh)
        {
            eachDisplay[2] = 0xFF; // Turn display minutes first digit OFF
            eachDisplay[3] = 0xFF; // Turn display minutes second digit OFF
            eachDisplay[0] = digitMaps[hoursToSet / 10]; // Used in transition between time display and set TIme set Alarm
            eachDisplay[1] = digitMaps[hoursToSet % 10];
        }
        else
        {
            eachDisplay[0] = 0xFF; // Turn display minutes first digit OFF
            eachDisplay[1] = 0xFF; // Turn display minutes second digit OFF
            eachDisplay[2] = digitMaps[minutesToSet / 10]; // Used in transition between time display and set TIme set Alarm
            eachDisplay[3] = digitMaps[minutesToSet % 10];
        }
}

void readTimeForDisplay(unsigned char hm, unsigned char ms)
{
    // Display state hh shows hours and minutes
    // Display state ss shows minutes and seconds
    // Display state ts shows hours or minutes to be set with other half of the display turned off
    // Display state as shows hours or minutes for the alarm to be set with other half turned off
    // Display state al shows Alarm time
    // 
    // If display is not in 24 hours fomat and display in HH MM state, transform digits to be displayed
    if(displayStates.hh)
    {
        pm = 0;
        digits[0] = hours/10;
        digits[1] = hours%10;
        digits[2] = minutes/10;
        digits[3] = minutes%10;
        // check if it is 24 hour format, if it is the values inside digits will change
        // values of hours and minutes will stay the same
        if(!displaySubStates.f24)
        {
            pm = 0;
            // Midnight
            if(hours == 0)
            {
                digits[0] = 1;
                digits[1] = 2;
                
            }
            // Noon
            else if(hours == 12)
            {
                pm = 1;
            }
            else if(hours > 12)
            {
                digits[0] = (hours - 12) / 10;
                digits[1] = (hours - 12) % 10;
                pm = 1;
            }
        }
        eachDisplay[0] = digitMaps[digits[0]];
        eachDisplay[1] = digitMaps[digits[1]];
        eachDisplay[2] = digitMaps[digits[2]];
        eachDisplay[3] = digitMaps[digits[3]];
        if(pm)
        {
            eachDisplay[3] &= 0x7F;
        }
        if(blinkMiddle)
        {
            eachDisplay[1] &= 0x7F;
        }
    }
    // display shows MM SS
    else if(displayStates.ss)
    {
        eachDisplay[0] = digitMaps[minutes/10];
        eachDisplay[1] = digitMaps[minutes%10] & 0x7F; // Middle dot always ON when MM SS
        eachDisplay[2] = digitMaps[seconds/10];
        eachDisplay[3] = digitMaps[seconds%10];
    }
    // Set Time state
    else if(displayStates.ts)
    {
        clockOrAlarmSet();
    }
    else if(displayStates.as)
    {
        clockOrAlarmSet();
    }
    else if(displayStates.al)
    {
        eachDisplay[0] = digitMaps[alarmHours/10];
        eachDisplay[1] = digitMaps[alarmHours%10] & 0x7F; // Middle dot always ON when MM SS
        eachDisplay[2] = digitMaps[alarmMinutes/10];
        eachDisplay[3] = digitMaps[alarmMinutes%10];
    }
}

void resetDisplayState()
{
    displayStates.hh = 1;
    displayStates.ss = 0;
    displayStates.ts = 0;
    displayStates.as = 0;
    displayStates.al = 0;
    
    displaySubStates.f24 = 1;
    displaySubStates.hh = 1;
   
 /*   seconds = 0;
    minutes = 0;
    hours = 0;
  */  
    pm = 0;
    blinkMiddle = 0;
}

int main(void) {
    /* Replace with your application code */
     eepromWrite(20, 0xFF);
 //   restore();
   
    pinSetup();
    TIMER0setup();
    TIMER1setup();
 //   TIMER2setup();
    configureADC();
    displayNum = 1;
    miliseconds = 0;
   
    dutyCycle = 16;
    ledsOn = 1;
    resetDisplayState();
    int i;
    
    
    sei();
    
    while(1)
    {

        dutyCycle = readPot()/4;
        readTimeForDisplay(minutes, seconds);
        if(dutyCycle)
        {    
            if(ledsOn)
            {
                clockDataIn(~eachDisplay[displayNum]);
                displayNum++;
                if(displayNum >= 4)
                {
                    displayNum = 0;
                }    
            }
            else
            {
                clockDataIn(~0xFF);
            }
        }
        else
        {
             // IF dutycycle is 0 Copletely turn off display
            clockDataIn(~0xFF);
        }
        if(displayStates.hh || displayStates.ss || displayStates.al)
        {
            displayTimeMode();
            // toggle alarm here 
        }
        else if(displayStates.ts)
        {
            setTimeMode();
        }
        else if(displayStates.as)
        {
            setAlarmMode();
        }
        if(eepromWriteFlag)
        {
            saveState();
            eepromWriteFlag = 0;
        }
        alarmHandling();
    }
}

ISR(TIMER0_COMPA_vect)
{
    ms++;
    miliseconds++;
    if(alarmActive)
    {
        if(shortButtonRead() == BUTTON_2)
        {
        //    PORTD &= 0xF7;
            disablePWMoutput();
  //          alarmActive = 0;
            alarmSilenced = 1;
        }
    }
    if(miliseconds == 1000)
    {
        if(displayStates.hh)
        {
            blinkMiddle = !blinkMiddle;
        }
        if(displayStates.as)
        {
            PORTB ^= 0x08;
        }
        seconds++;
        miliseconds = 0;
        
        if(seconds == 60)
        {
            eepromWriteFlag = 1;
            seconds = 0;
            minutes++;
            if(minutes == 60)
            {
                minutes = 0;
                hours++;
                if(hours == 24)
                {
                    hours = 0;
                    nextDay = 1;
                }
            }
        }
        
    }
    
}

ISR(TIMER1_COMPA_vect)
{
    pwmCounter++;
    if(pwmCounter >= dutyCycle)
    {
        ledsOn = 0;         
    }
    if(pwmCounter >= 64)
    {
        ledsOn = 1;
        pwmCounter = 0;
    }
}



void displayTimeMode()
{
    // constantly read buttons
    // read long button1
    // read short buttons 2 and 3
    
    // if button 1 short switch to mm ss, use toggle variable, state stays
    
    // if button 1 long state changes
    // if button 1 and button 2 long state changes
    
    // if button 2 short state stays alarm off, if alarm active alarm off
    static unsigned int heldTime;
    static unsigned char firstDetection;
    static unsigned char heldConfirmed;
    unsigned char buttonPress;
/*    if((PINC & 0x0E) == 0x0E)
    {
        returnFromSettings = 0;
    }
    if(returnFromSettings)
    {
        return;
    }*/
    buttonPress = longButtonRead();
    if(!buttonPress)
    {
        returnFromSettings = 0;
    }
    if(returnFromSettings)
    {
        return;
    }
    if(buttonPress == BUTTON_1_HOLD && displayStates.hh)
    {
        
        if(!firstDetection)
        {
            firstDetection = 1;
            heldTime = ms;
        }
        if((ms - heldTime) > LONG_TIMEOUT)
        {
            heldTime = ms;
            firstDetection = 0;
            heldConfirmed = 1;
            if(possibleAlarmSet)
            {
                displayStates.hh = 0;
                displayStates.ss = 0;
                displayStates.as = 1;
                displayStates.al = 0;
                displayStates.ts = 0;
                displaySubStates.hh = 1;
                returnFromSettings = 1;
                possibleAlarmSet = 0;
                return;
            }
            displaySubStates.f24 = !displaySubStates.f24;
            
        }
        //reset TIMER
    }
    else
    {
        // If button press detected then released in less than 2s go to timeSetState
        if(firstDetection && !heldConfirmed)
        {
            buttonPress = BUTTON_1_SHORT;
        }
        heldConfirmed = 0;
        firstDetection = 0;
        heldTime = ms;
    }
    if(buttonPress == BUTTON_1_SHORT)
    {
        displayStates.hh = 0;
        displayStates.ss = 0;
        displayStates.ts = 1;
        displaySubStates.hh = 1;
        returnFromSettings = 1;
    
    }
    buttonPress = shortButtonRead();
    if(buttonPress == BUTTON_2)
    {
        if(!alarmActive)
        {
            alarmSet = !alarmSet;
            nextDay = !nextDay;
            PORTB ^= 0x08;
        }
        else
        {
        //    PORTD &= 0xF7;
            disablePWMoutput();
        //    alarmActive = 0;
            alarmSilenced = 1;
        }
    }
    if(buttonPress == BUTTON_3)
    {
        if(displayStates.hh)
        {
            displayStates.hh = 0;
            displayStates.ss = 1;
            displayStates.al = 0;
            PORTB |= 0x04;
        }
        else if(alarmSet && displayStates.ss)
        {
            displayStates.hh = 0;
            displayStates.ss = 0;
            displayStates.al = 1;
            // light alarm led
            PORTB &= 0xFB; 
        }
        else
        {
            displayStates.hh = 1;
            displayStates.ss = 0;
            displayStates.al = 0;
            // turn off alarm time led
            PORTB |= 0x04;
        }
       
    }
    

    
}

void setTimeMode()
{
    //read button 1 short, switch between HH MM and back to displayTimeMode
    // read button 2 short for icrement
    // read button 3 for decrement
    setTime(1);
    
}


void setTime(unsigned char CA)
{
    signed char incDec = 0;
    unsigned char buttonPressed;
    buttonPressed = shortButtonRead();
    if((PINC & 0x0E) == 0x0E)
    {
        returnFromSettings = 0;
    }
    if(returnFromSettings)
    {
        return;
    }
    if(buttonPressed == BUTTON_1_SHORT)
    {
        if(displaySubStates.hh)
        {
            displaySubStates.hh = 0;
        }
        else
        {
            if(CA)
            {    
                hours = hoursToSet;
                minutes = minutesToSet;
                seconds = 0;
                displayStates.ts = 0;
            }
            else
            {
                alarmHours = hoursToSet;
                alarmMinutes = minutesToSet;
                displayStates.as = 0;
                returnFromSettings = 1;
                alarmSet = 1;
                eepromWriteFlag = 1;
                nextDay = 1;
                PORTB &= 0x7F;
            }
            displayStates.hh = 1;
            returnFromSettings = 1;
        }
    }
    if(buttonPressed == BUTTON_2)
    {
        if(alarmActive)
        {
        //    PORTD &= 0xF7;
            disablePWMoutput();
        //    alarmActive = 0;
            alarmSilenced = 1;
        }
        else
        {
            incDec = 1;
        }
    }
    else if(buttonPressed == BUTTON_3)
    {
        
        incDec = -1;
    }
    if(incDec != 0)
    {
        if(displaySubStates.hh)
        {
            hoursToSet += incDec;
            if(hoursToSet == 24)
            {
                hoursToSet = 0;
            }
            if(hoursToSet == -1)
            {
                hoursToSet = 23;
            }
        }
        else
        {
            minutesToSet += incDec;
            if(minutesToSet == 60)
            {
                minutesToSet = 0;
            }
            if(minutesToSet == -1)
            {
                minutesToSet = 59;
            }
        }
    }
}

void setAlarmMode()
{
    setTime(0);
}

unsigned char readPot()
{
    unsigned char result;
    ADCSRA |= 0x40;
    while(ADCSRA & 0x40);
    result = ADCH;
    return result;
}

unsigned char shortButtonRead()
{
    static unsigned char buttonOld;
    static unsigned int timeout;
    unsigned char buttonNow;

    buttonNow = 0;

    if(!(PINC & 0x04))
    {
       if(!(PINC & 0x04))
       buttonNow = BUTTON_2;        
    }
     if(!(PINC & 0x02))
    {
        if(!(PINC & 0x02))
        buttonNow = BUTTON_1_SHORT;
    }
    if(!(PINC & 0x08))
    {
        if(!(PINC & 0x08))
        buttonNow = BUTTON_3;
    }
    
    // wait for all buttons released to become stable
    if(buttonOld && !buttonNow)
    {
       if(((PINC & 0x0E) >> 1) == 7)
       {
           // 
           while(((PINC & 0x0E) >> 1) != 7);
       }
    }
    
    if((buttonNow != buttonOld) || (ms - timeout > TIMEOUT))
    {
        if((displayStates.hh || displayStates.ss || displayStates.al) && (buttonOld == buttonNow))
            {
                return 0;
            }
            buttonOld = buttonNow;
            timeout = ms;
            
            return buttonNow;
    }
    else
    {
        return 0;
    }
}

unsigned char longButtonRead()
{
    static unsigned char together;
    if(!(PINC & 0x02))
    {
        if(!(PINC & 0x04))
        {
            if(!together)
            possibleAlarmSet = 1;
        }
        else
        {
            possibleAlarmSet = 0;
            together = 1;
        }
        return BUTTON_1_HOLD;
    }
    together = 0;
    return 0;
}

/*******************************************************************************
 * EEPROM SAVE TIME FUNCTIONS
 ******************************************************************************/
unsigned char eepromRead(unsigned int addr)
{
    while(EECR &(1 << EEPE));
    EEAR = addr;
    EECR |= (1 << EERE);
    return EEDR;
}

void eepromWrite(unsigned int addr, unsigned char byte)
{
    while(EECR &(1 << EEPE));
    EEAR = addr;
    EEDR = byte;
    EECR |= (1 << EEMPE);
    EECR |= (1 << EEPE);        
}

void restore()
{
    unsigned char states;
    hours = eepromRead(8);
    minutes = eepromRead(9);
    alarmHours = eepromRead(10);
    alarmMinutes = eepromRead(11);
    alarmSet = eepromRead(12);

    //is it first ever
    if(eepromRead(20) == 0xFF)
    {
        eepromWrite(20, 1);
        hours = 0;
        minutes = 0;
        alarmHours = 0;
        alarmMinutes = 0;
        alarmSet = 0;
    }
}

void saveState()
{
    eepromWrite(8, hours);
    eepromWrite(9, minutes);
    eepromWrite(10, alarmHours);
    eepromWrite(11, alarmMinutes);
    eepromWrite(12, alarmSet);
}

/*******************************************************************************
 * ALARM FUNCTIONS
 ******************************************************************************/

void alarmHandling()
{
    if(!displayStates.as)
    {
        if(alarmSet && !alarmActive && nextDay)
        {
            ALARM_ACTIVE_LED_ON();
            // compare times
            if(hours == alarmHours && minutes == alarmMinutes)
            {
                // START BUZZER PWM HERE, DRIVE ALARM FUNCTION THEN TOGGLES PWM OUTPUT ON AND OFF
                enablePWMoutput();
                alarmActive = 1;
            //    alarmSet = 0;
                alarmActiveTime = ms;
                buzzerPeriod = ms;
                nextDay = 0;
            }
            
        }
        else
        {
 //           PORTB |= 0x08;
        }
    }
    if(alarmActive)
    { 
        driveAlarm();
    }
    
}
    
void driveAlarm()
{
    if((ms - alarmActiveTime) > 5000)
    {
     
        disablePWMoutput();
        alarmActive = 0;
        alarmSilenced = 0;
    }
    else
    {
         if((ms - buzzerPeriod) > 500)
        {
//            ALARM_ACTIVE_LED_OFF();
            buzzerPeriod = ms;
            if(!alarmSilenced)
//                driveBeeper();
            togglePWMoutput();
            else
                disablePWMoutput();
        }   
    }
   
}

void enablePWMoutput()
{
 //   TCCR2A |= 0b00010000;
    PORTD &= 0xF7;
}

void disablePWMoutput()
{
//    TCCR2A &= 0xEF;
    PORTD |= 0x08;
}

void togglePWMoutput()
{
//    TCCR2A ^= 0b00010000;
    PORTD ^= 0x08;
}
