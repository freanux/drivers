# HD44780/ST7066U display driver
This kernel modules drives an Hitachi HD44780/Sitronix ST7066U LCD controller with 4 rows and 20 characters per line.
It drives the controller in 4 bit mode. Once the driver is probed, the device `/dev/lcd` will appear. Just write into it to send characters to the LCD.

## GPIO pin configuration
```
RS -> GPIO pin 4
RW -> GPIO pin 17
EN -> GPIO pin 27
D4 -> GPIO pin 22
D5 -> GPIO pin 23
D6 -> GPIO pin 24
D7 -> GPIO pin 25
```
or you can change the pin configuration in the code:
```
#define LCD_RS 4
#define LCD_RW 17
#define LCD_E 27
#define LCD_D4 22
#define LCD_D5 23
#define LCD_D6 24
#define LCD_D7 25
```

## Driver commands
This driver can handle a bunch of control codes:

### Clear display
Send a 0x01 to the device. 

### Cursor management
Send a 0x02 to the device with two following bytes. The first byte enables/disables the cursor, the second sets the blinking mode.

### Set cursor position
Send a 0x03 with to following bytes for x and y position, beginning with zero (0, 0).

### Draw mode
Send a 0x04 with one following byte, if set, each printing to the device, the whole screen will be cleared.

## Examples
```
$ echo "Hello world" > /dev/lcd
$ printf "\x04\x01" > /dev/lcd
$ printf "\x02\x01\x01" > /dev/lcd
$ printf "\x02\x00\x00" > /dev/lcd
```

## Tested on
This driver is tested on a Raspberry Pi.
