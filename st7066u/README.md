# HD44780/ST7066U display driver
This kernel module drives an Hitachi HD44780/Sitronix ST7066U LCD controller with 4 rows and 20 characters per line.
It drives the controller in 4 bit mode. Once the driver is probed, the device `/dev/lcd` will appear. Just write into it to send characters to the LCD.
The board I used is a [NHD-0420DZ-FL-YBW-33V3](http://www.newhavendisplay.com/nhd0420dzflybw33v3-p-5168.html).

![alt tag](https://raw.githubusercontent.com/freanux/drivers/master/st7066u/pictures/st7066u.jpg)

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

### Clear display (0x10)
To clear the LCD display, send a 0x10 to the device. 

### Cursor management (0x11)
Send a 0x11 to the device with two following bytes. The first byte enables/disables the cursor, the second sets the blinking mode.

### Set cursor position (0x12)
Send a 0x12 with to following bytes for x and y position, beginning with zero (0, 0).

### Draw mode (0x13)
Send a 0x13 with one following byte, if set, each printing to the device, the whole screen will be cleared.

### Define character (0x14)
To define a character send a 0x14, then your character code (0 - 7), then eight bytes for each row.

## Examples
```
$ echo "Hello world" > /dev/lcd
$ printf "\x13\x01" > /dev/lcd
$ printf "\x11\x01\x01" > /dev/lcd
$ printf "\x12\x03\x03Hello world" > /dev/lcd
$ printf "\x14\x01\x00\x0a\x1f\x1f\x1f\x0e\x04\x00" > /dev/lcd
```

## Tested on
This driver is tested on a Raspberry Pi.
