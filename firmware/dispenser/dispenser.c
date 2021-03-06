#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <avr/eeprom.h>
#include <stdarg.h>
#include <stdlib.h>
#include "defs.h"

#include "packet.h"
#include "serial.h"
#include "led.h"

// TODO
// Add support for different animation speeds

// EEprom data 
uint32_t EEMEM _ee_pump_id;
uint32_t EEMEM _ee_run_time;

#define RESET_DURATION  1
#define SYNC_COUNT      10 // Every SYNC_INIT ms we will change the color animation
#define NUM_ADC_SAMPLES 5
#define NUM_CURRENT_SENSE_SAMPLES 10

// this (non volatile) variable keeps the current liquid level
static uint16_t g_liquid_level = 0;

volatile uint32_t g_time = 0;
static volatile uint32_t g_reset_fe_time = 0;
static volatile uint32_t g_reset = 0;
static volatile uint32_t g_ticks = 0;
static volatile uint32_t g_dispense_target_ticks = 0;
static volatile uint8_t g_is_dispensing = 0;

static volatile uint8_t g_hall0 = 0;
static volatile uint8_t g_hall1 = 0;
static volatile uint8_t g_hall2 = 0;
static volatile uint8_t g_hall3 = 0;
static volatile uint8_t g_sync = 0;
static volatile uint32_t g_sync_count = 0, g_pattern_t = 0;
static void (*g_led_function)(uint32_t, color_t *) = 0;

void check_dispense_complete_isr(void);
void set_motor_speed(uint8_t speed);

/*
   0  - PD0 - RX
   1  - PD1 - TX
   2  - PD2 - RESET
   3  - PD3 - LED clock
   4  - PD4 - LED data
   5  - PD5 - motor PWM out
   6  - PD6 - Hall 0 (pcint 22)
   7  - PD7 - Hall 1 (pcint 23)
   8  - PB0 - Hall 2 (pcint 0)
   9  - PB1 - Hall 3 (pcint 1) 
  10  - PB2 - SYNC (pcint 2)
  A0  - PC0 - CS
  A1  - PC1 - liquid level

*/
void setup(void)
{
    serial_init();

    // Set up LEDs & motor out
    DDRD |= (1<<PD3)|(1<<PD4)|(1<<PD5);

    // pull ups
    sbi(PORTD, 6);
    sbi(PORTD, 7);
    sbi(PORTB, 0);
    sbi(PORTB, 1);

    // Timer setup for reset pulse width measuring
    TCCR1B |= TIMER1_FLAGS;
    TCNT1 = TIMER1_INIT;
    TIMSK1 |= (1<<TOIE1);

    // Set to Phase correct PWM, compare output mode
    TCCR0A |= _BV(WGM00) | _BV(COM0B1);

    // Set the clock source
    TCCR0B |= (0 << CS00) | (1 << CS01);

    // Reset timers and comparators
    OCR0B = 0;
    TCNT0 = 0;

    // INT0 for router reset
    EICRA |= (1 << ISC00);
    EIMSK |= (1 << INT0);

    // PCINT setup
    PCMSK0 |= (1 << PCINT0) | (1 << PCINT1) | (1 << PCINT2);
    PCMSK2 |= (1 << PCINT22) | (1 << PCINT23);
    PCICR |=  (1 << PCIE2) | (1 << PCIE0);
}

// update g_time
ISR (TIMER1_OVF_vect)
{
    g_time++;
    TCNT1 = TIMER1_INIT;
}

// reset pin change
ISR(INT0_vect)
{
    if (PIND & (1<<PIND2))
    {
        g_reset_fe_time = g_time + RESET_DURATION;
    }
    else
    {
        if (g_reset_fe_time > 0 && g_time >= g_reset_fe_time)
            g_reset = 1;
        g_reset_fe_time = 0;
    }
}

ISR(PCINT0_vect)
{
    uint8_t      state;

    state = PINB & (1<<PINB0);
    if (state != g_hall2)
    {
        g_hall2 = state;
        g_ticks++;
    }

    state = PINB & (1<<PINB1);
    if (state != g_hall3)
    {
        g_hall3 = state;
        g_ticks++;
    }
    check_dispense_complete_isr();

    state = PINB & (1<<PINB2);
    if (state != g_sync)
    {
        g_sync_count++;
        g_sync = state;
    }
}

ISR(PCINT2_vect)
{
    uint8_t state;

    state = PIND & (1<<PIND6);
    if (state != g_hall0)
    {
        g_hall0 = state;
        g_ticks++;
    }

    state = PIND & (1<<PIND7);
    if (state != g_hall1)
    {
        g_hall1 = state;
        g_ticks++;
    }
    check_dispense_complete_isr();
}

// this function is called from an ISR, so no need to turn off/on interrupts
void check_dispense_complete_isr(void)
{
    if (g_dispense_target_ticks > 0 && g_ticks >= g_dispense_target_ticks)
    {
         g_dispense_target_ticks = 0;
         g_is_dispensing = 0;
         set_motor_speed(0);
    }
}

uint8_t check_reset(void)
{
    uint8_t reset;

    cli();
    reset = g_reset;
    sei();

    return reset;
}

void idle(void)
{
    color_t c;
    uint8_t animate = 0;
    uint32_t t = 0;

    cli();
    if (g_sync_count >= SYNC_COUNT)
    {
        g_sync_count = 0;
        animate = 1;
    }
    sei();

    if (animate && g_led_function)
    {
        cli();
        t = g_pattern_t++;
        sei();
        // do some animation!
        (*g_led_function)(t, &c);
        set_led_rgb_no_delay(c.red, c.green, c.blue);
    }
}

void set_led_pattern(void (*func)(uint32_t, color_t *))
{
    if (func == NULL)
        set_led_rgb(0, 0, 0);

    cli();
    g_pattern_t = 0;
    sei();
    g_led_function = func;
}

void adc_liquid_level_setup(void)
{
    ADCSRA = (1 << ADPS1);
    ADMUX = (1<<REFS0) | (1 << MUX0);
    ADCSRA |= (1<<ADEN);
}

void adc_current_sense_setup(void)
{
    ADCSRA = (1 << ADPS0) | (1 << ADPS1) | (1 << ADPS2);
    ADMUX = (1<<REFS0) | (0 << REFS1);
    ADCSRA |= (1<<ADEN);
}

void adc_shutdown(void)
{
    ADCSRA &= ~(1<<ADEN);
}

uint16_t adc_read()
{
    uint8_t hi, low;

    ADCSRA |= (1<<ADSC);
    while(ADCSRA & 0b01000000);
    low = ADCL;
    hi = ADCH;
    return (hi << 8) | low;
}

uint16_t read_current_sense(void)
{
    uint8_t  i;
    uint16_t v = 0;

    adc_current_sense_setup();
    for(i = 0; i < NUM_CURRENT_SENSE_SAMPLES; i++)
        v += adc_read();
    adc_shutdown();

    return (uint16_t)(v / NUM_CURRENT_SENSE_SAMPLES);
}

void update_liquid_level(void)
{
    uint8_t  i;
    uint16_t v = 0;

    adc_liquid_level_setup();
    for(i = 0; i < NUM_ADC_SAMPLES; i++)
        v += adc_read();
    adc_shutdown();

    g_liquid_level = (uint16_t)(v / NUM_ADC_SAMPLES);
}

void get_liquid_level(void)
{
    send_packet16(PACKET_LIQUID_LEVEL, g_liquid_level);
}

void set_motor_speed(uint8_t speed)
{
    OCR0B = 255 - speed;
}

void run_motor_timed(uint32_t duration)
{
    uint32_t t;

    set_motor_speed(255);
    for(t = 0; t < duration && !check_reset(); t++)
        _delay_ms(1);
    set_motor_speed(0);
}

void dispense_ticks(uint32_t ticks)
{
    uint8_t dispensing;

    cli();
    dispensing = g_is_dispensing;
    sei();

    if (dispensing)
        return;

    cli();
    g_dispense_target_ticks = ticks;
    g_ticks = 0;
    g_is_dispensing = 1;
    sei();

    set_motor_speed(255);
}

void is_dispensing(void)
{
    uint8_t dispensing;

    cli();
    dispensing = g_is_dispensing;
    sei();

    send_packet8(PACKET_IS_DISPENSING, dispensing);
}

uint8_t read_pump_id_from_eeprom(void)
{
    return eeprom_read_byte((uint8_t *)0);
}

uint8_t address_exchange(void)
{
    uint8_t  ch;
    uint8_t  id;

    set_led_rgb(0, 0, 255);
    id = read_pump_id_from_eeprom();
    if (id == 0 || id == 255)
    {
        // we failed to get a unique number for the pump. just stop.
        set_led_rgb(255, 0, 0);
        for(;;);
    }

    for(;;)
    {
        for(;;)
        {
            if (serial_rx_nb(&ch))
                break;

            if (check_reset())
                return 0xFF;
        }
        if (ch == 0xFF)
            break;
        if (ch == '?')
            serial_tx(id);
    }
    set_led_rgb(0, 255, 0);

    return id;
}

void comm_test(void)
{
    uint8_t ch;

    // disable all interrupts and just echo every character received.
    cli();
    set_led_rgb(0, 255, 255);
    for(; !check_reset();)
        if (serial_rx_nb(&ch))
            for(; !serial_tx_nb(ch) && !check_reset();)
                ;
    sei();
}

void id_conflict(void)
{
    // we failed to get an address. stop and wait for a reset
    set_led_rgb(255, 0, 0);
    for(; !check_reset();)
        ;
}

int main(void)
{
    uint8_t id, rec, i;
    packet_t p;

    setup();
    set_motor_speed(0);
    sei();
    for(i = 0; i < 5; i++)
    {
        set_led_rgb(255, 0, 255);
        _delay_ms(50);
        set_led_rgb(255, 255, 0);
        _delay_ms(50);
    }

    // get the current liquid level 
    update_liquid_level();

    for(;;)
    {
        cli();
        g_reset = 0;
        setup();
        serial_init();
        set_motor_speed(0);
        set_led_rgb(0, 0, 255);

        sei();
        id = address_exchange();

        for(; !check_reset();)
        {
            rec = receive_packet(&p);
            if (rec == COMM_CRC_FAIL)
                continue;

            if (rec == COMM_RESET)
                break;

            if (rec == COMM_OK && (p.dest == DEST_BROADCAST || p.dest == id))
            {
                switch(p.type)
                {
                    case PACKET_PING:
                        break;

                    case PACKET_SET_MOTOR_SPEED:
                        set_motor_speed(p.p.uint8[0]);
                        break;

                    case PACKET_TICK_DISPENSE:
                        dispense_ticks(p.p.uint32);
                        break;

                    case PACKET_TIME_DISPENSE:
                        run_motor_timed(p.p.uint32);
                        break;

                    case PACKET_IS_DISPENSING:
                        is_dispensing();
                        break;

                    case PACKET_LIQUID_LEVEL:
                        get_liquid_level();
                        break;

                    case PACKET_UPDATE_LIQUID_LEVEL:
                        update_liquid_level();
                        break;

                    case PACKET_LED_OFF:
                        set_led_pattern(NULL);
                        break;

                    case PACKET_LED_IDLE:
                        set_led_pattern(led_pattern_idle);
                        break;

                    case PACKET_LED_DISPENSE:
                        set_led_pattern(led_pattern_dispense);
                        break;

                    case PACKET_LED_DRINK_DONE:
                        set_led_pattern(led_pattern_drink_done);
                        break;

                    case PACKET_COMM_TEST:
                        comm_test();
                        break;

                    case PACKET_ID_CONFLICT:
                        id_conflict();
                        break;
                }
            }
        }
    }
    return 0;
}
