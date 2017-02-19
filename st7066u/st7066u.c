#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/delay.h>

#define DRIVER_NAME "st7066u"
#define CLASS_NAME "lcd"
#define DEVICE_NAME "lcd"
#define DRIVER_AUTHOR "Philippe Widmer <pw@earthwave.ch>"
#define DRIVER_DESCRIPTION "ST7066U display driver"
#define DRIVER_VERSION "0.1"

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);

#define LCD_RS 4
#define LCD_RW 17
#define LCD_E 27
#define LCD_D4 22
#define LCD_D5 23
#define LCD_D6 24
#define LCD_D7 25

#define GPIO_OUT 0
#define GPIO_IN 1

#define CMD_WAKE_UP       0x30

#define CMD_CLEAR_DISPLAY 0x01

#define CMD_RETURN_HOME   0x02

#define CMD_DISPLAY       0x08
#define CMD_DSP_ON        0x04
#define CMD_DSP_CUR_ON    0x02
#define CMD_DSP_CUR_BLINK 0x01

#define CMD_FUNCTION_SET  0x20
#define CMD_FS_8BIT       0x10
#define CMD_FS_2LINE      0x08
#define CMD_FS_FONT_5X11  0x04

#define CMD_ENTRY_MODE    0x04
#define CMD_EM_RIGHT      0x02
#define CMD_EM_DSP_SH_LFT 0x01

#define CMD_SET_CGRAM     0x40

#define LCD_WIDTH         20
#define LCD_HEIGHT        4

#define LCD_LINE1         0x80
#define LCD_LINE2        (0x80 + 0x40)
#define LCD_LINE3        (0x80 + 0x14)
#define LCD_LINE4        (0x80 + 0x54)

static int lcd_rows[] = {
    LCD_LINE1,
    LCD_LINE2,
    LCD_LINE3,
    LCD_LINE4,
};

#define lcd_xy(x, y)     send_command((x % LCD_WIDTH) + \
                         (lcd_rows[y % LCD_HEIGHT]))

static struct pin_configuration {
    unsigned int pin;
    int direction;
} gpio_pins[] = {
    { LCD_RS, GPIO_OUT },
    { LCD_RW, GPIO_OUT },
    { LCD_E, GPIO_OUT },
    { LCD_D4, GPIO_OUT },
    { LCD_D5, GPIO_OUT },
    { LCD_D6, GPIO_OUT },
    { LCD_D7, GPIO_OUT },
};
#define NUM_GPIO_PINS (sizeof(gpio_pins) / sizeof(struct pin_configuration))

static unsigned int used_pins[NUM_GPIO_PINS];
static int major_nr;
static struct class *lcd_class;
static struct device *lcd_device;
static DEFINE_MUTEX(dev_mutex);
static u8 lcd_buffer[LCD_HEIGHT][LCD_WIDTH];
static int cursor_x;
static int cursor_y;
static bool draw_mode_reset = false;

#define STROBE_LEN 1
#define COMMAND_LEN 2

static void send_nibble(u8 c) {
    gpio_set_value(LCD_D4, (c & 0x10) >> 4);
    gpio_set_value(LCD_D5, (c & 0x20) >> 5);
    gpio_set_value(LCD_D6, (c & 0x40) >> 6);
    gpio_set_value(LCD_D7, (c & 0x80) >> 7);
    gpio_set_value(LCD_E, 1);
    udelay(STROBE_LEN);
    gpio_set_value(LCD_E, 0);
}

static void send_byte(u8 c) {
    send_nibble(c);
    send_nibble(c << 4);
    udelay(50);
}

static void send_command(u8 c) {
    gpio_set_value(LCD_RS, 0);
    send_byte(c);
    if (c < 4) {
        mdelay(COMMAND_LEN);
    }
}

static void send_character(u8 c) {
    gpio_set_value(LCD_RS, 1);
    send_byte(c);
}

static void clear_buffer(void) {
    int by, bx;
    for (by = 0; by < LCD_HEIGHT; ++by) {
        for (bx = 0; bx < LCD_WIDTH; ++bx) {
            lcd_buffer[by][bx] = ' ';
        }
    }
}

static void print_buffer(void) {
    int by, bx;
    for (by = 0; by < LCD_HEIGHT; ++by) {
        lcd_xy(0, by);
        for (bx = 0; bx < LCD_WIDTH; ++bx) {
            send_character(lcd_buffer[by][bx]);
        }
    }
}

static void new_line(void) {
    int by, bx;

    cursor_x = 0;
    cursor_y++;
    if (cursor_y > LCD_HEIGHT - 1) {
        cursor_y = LCD_HEIGHT - 1;
        for (by = 0; by < LCD_HEIGHT - 1; ++by) {
            for (bx = 0; bx < LCD_WIDTH; ++bx) {
                lcd_buffer[by][bx] = lcd_buffer[by + 1][bx];
            }
        }
        for (bx = 0; bx < LCD_WIDTH; ++bx) {
            lcd_buffer[LCD_HEIGHT - 1][bx] = ' ';
        }
        print_buffer();
    }
    lcd_xy(cursor_x, cursor_y);
}

static void clear_screen(void) {
    clear_buffer();
    send_command(CMD_CLEAR_DISPLAY);
    cursor_x = cursor_y = 0;
}

static void dsp_cmd_clr(const char __user *buffer) { 
    clear_screen();
}

static void dsp_cmd_cursor(const char __user *buffer) {
    u8 cmd = CMD_DISPLAY | CMD_DSP_ON;
    cmd |= (buffer[0] ? CMD_DSP_CUR_ON : 0);
    cmd |= (buffer[1] ? CMD_DSP_CUR_BLINK : 0);
    send_command(cmd);
}

static void dsp_cmd_setxy(const char __user *buffer) {
    lcd_xy(buffer[0] % LCD_WIDTH, buffer[1] % LCD_HEIGHT);
}

static void dsp_cmd_draw_mode(const char __user *buffer) {
    draw_mode_reset = (buffer[0] > 0);
}

static void dsp_cmd_define_char(const char __user *buffer) {
    int i;

    send_command(CMD_SET_CGRAM | ((buffer[0] % 8) << 3));
    for (i = 1; i < 9; ++i) {
        send_character(buffer[i]);
    }
    send_command(CMD_RETURN_HOME);
}

static void dsp_cmd_newline(const char __user *buffer) {
    new_line();
}

static struct dsp_command {
    u8 command;
    int params;
    void (*handler)(const char __user *buffer);
} dsp_commands[] = {
    { 0x10, 0, dsp_cmd_clr },
    { 0x11, 2, dsp_cmd_cursor },
    { 0x12, 2, dsp_cmd_setxy },
    { 0x13, 1, dsp_cmd_draw_mode },
    { 0x14, 9, dsp_cmd_define_char },
    { 0x0a, 0, dsp_cmd_newline },
};
#define NUM_DSP_CMDS (sizeof(dsp_commands) / sizeof(struct dsp_command))

static ssize_t lcd_write(struct file *f, const char __user *buffer, size_t len, loff_t *off) {
    ssize_t sz = (ssize_t)len;
    int i;

    if (mutex_lock_interruptible(&dev_mutex)) {
        return -ERESTARTSYS;
    }
    
    if (draw_mode_reset) {
        clear_screen();
    }

    if (len) {
        while (len--) {
            char c = *buffer;
            if (cursor_x > LCD_WIDTH - 1) {
                new_line();
            }
            if (likely(c > 31 || c < 8)) {
                lcd_buffer[cursor_y][cursor_x] = c;
                send_character(c);
                cursor_x++;
            } else {
                for (i = 0; i < NUM_DSP_CMDS; ++i) {
                    if (dsp_commands[i].command == c) {
                        if (dsp_commands[i].params > len) {
                            sz = -EFAULT;
                            goto lcd_write_out;
                        }
                        dsp_commands[i].handler(&buffer[1]);
                        len -= dsp_commands[i].params;
                        buffer += dsp_commands[i].params;
                        break;
                    }
                }
            }
            buffer++;
        }
    }

lcd_write_out:
    mutex_unlock(&dev_mutex);
    return sz;
}

static struct file_operations fops = {
    .write = lcd_write,
};

static void release_gpio(void) {
    size_t i;

    // free pins
    for (i = 0; i < NUM_GPIO_PINS; ++i) {
        if (used_pins[i]) {
            gpio_free(used_pins[i]);
        }
    }
}

static int setup_gpio(void) {
    int result = 0;
    size_t i;

    // clear
    memset(used_pins, 0, sizeof(used_pins));
    
    // setup pins
    for (i = 0; i < NUM_GPIO_PINS; ++i) {
        result = gpio_request(gpio_pins[i].pin, DRIVER_NAME);
        if (result) {
            release_gpio();
            return result;
        }
        if (gpio_pins[i].direction == GPIO_OUT) {
            gpio_direction_output(gpio_pins[i].pin, 0);
        } else {
            gpio_direction_input(gpio_pins[i].pin);
        }
        used_pins[i] = gpio_pins[i].pin;
    }

    // done 
    return 0;
}

static int __init st7066u_init(void) {
    int result = 0;

    printk(KERN_INFO "%s: %s (v%s)\n", DRIVER_NAME, DRIVER_DESCRIPTION, DRIVER_VERSION);

    /* setup gpio pins */
    result = setup_gpio();
    if (result) {
        printk(KERN_ALERT "%s: setting up gpio pins failed\n", DRIVER_NAME);
        return result;
    }
   
    /* register device */
    major_nr = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_nr < 0) {
        release_gpio();
        printk(KERN_ALERT "%s: failed to register a character device\n", DRIVER_NAME);
        return major_nr;
    }

    /* create class */
    lcd_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(lcd_class)) {
        release_gpio();
        unregister_chrdev(major_nr, DEVICE_NAME);
        printk(KERN_ALERT "%s: failed to register class\n", DRIVER_NAME);
        return PTR_ERR(lcd_class);
    }

    /* create device */
    lcd_device = device_create(lcd_class, NULL, MKDEV(major_nr, 0), NULL, DEVICE_NAME);
    if (IS_ERR(lcd_device)) {
        release_gpio();
        class_destroy(lcd_class);
        unregister_chrdev(major_nr, DEVICE_NAME);
        printk(KERN_ALERT "%s: failed to create lcd device\n", DEVICE_NAME);
        return PTR_ERR(lcd_device);
    }

    /* wake up sequence */
    mdelay(15);
    send_nibble(CMD_WAKE_UP);
    mdelay(5);
    send_nibble(CMD_WAKE_UP);
    udelay(100);
    send_nibble(CMD_WAKE_UP);
    udelay(50);

    /* init */
    send_nibble(CMD_FUNCTION_SET);
    udelay(50);

    /* configure lcd */
    send_command(CMD_FUNCTION_SET | CMD_FS_2LINE);
    send_command(CMD_DISPLAY);
    send_command(CMD_CLEAR_DISPLAY);
    send_command(CMD_ENTRY_MODE | CMD_EM_RIGHT);
    send_command(CMD_DISPLAY | CMD_DSP_ON);

    /* clear buffer */
    clear_buffer();
    cursor_x = cursor_y = 0;

    /* done */
    return 0;
}

static void __exit st7066u_exit(void) {
    release_gpio();
    device_destroy(lcd_class, MKDEV(major_nr, 0));
    class_destroy(lcd_class);
    unregister_chrdev(major_nr, DEVICE_NAME);

    printk(KERN_INFO "%s: driver unprobed\n", DRIVER_NAME);
}

module_init(st7066u_init);
module_exit(st7066u_exit);
