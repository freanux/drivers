#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/sched.h>
#include <linux/string.h>

#define DRIVER_AUTHOR "Philippe Widmer <pw@earthwave.ch>"
#define DRIVER_DESCRIPTION "visible light communication driver via GPIO"
#define DRIVER_VERSION "0.1"

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);

#define DEVICE_NAME "vlc_gpio"
#define CLASS_NAME "vlc"

#define ACCESS_MODE S_IWUSR | S_IRUGO

static int major_nr;
static struct class *vlc_class = NULL;
static struct device *vlc_device = NULL;
static DEFINE_MUTEX(dev_mutex);
static wait_queue_head_t wait_queue;

#define STATE_SYNC 1
#define STATE_DATA 2
#define SYNC_LEN 2

static bool configured = false;
static unsigned int gpio_vlc = 23;
static unsigned int irq_nr;
//static struct timespec64 ts_last, ts_current, ts_diff;
static ktime_t ts_last, ts_current;
static s64 ts_diff;
static long delay = 0;
static unsigned int debounce = 0;
static int state;
static int pilot_count;
static s64 pulse_len;
static int data;
static int data_bit;
static int pulse_bit;
static bool stop_bit;

#define RING_SIZE 64

typedef uint8_t ring_pos_t;
volatile ring_pos_t ring_head;
volatile ring_pos_t ring_tail;
volatile char ring_data[RING_SIZE];
ring_pos_t next_head;

static void reset_state(void) {
    state = STATE_SYNC;
    pilot_count = 0;
    pulse_len = 0;
    //printk(KERN_INFO "vlc: reset_state()\n");
}

static irqreturn_t vlc_irq_handler(int irq, void *dev_id) {
    ts_current = ktime_get();
    ts_diff = ktime_us_delta(ts_current, ts_last);
    ts_last = ts_current;

    if (unlikely(ts_diff > 999999)) {
        reset_state();
    } else {
        if (likely(ts_diff > delay)) {
            switch (state) {
                case STATE_SYNC:
                    //printk(KERN_INFO "pulse\n");
                    pulse_len += ts_diff;
                    pilot_count++;
                    if (pilot_count % 2 == 0) {
                        pulse_len /= SYNC_LEN;
                        pulse_len += (pulse_len / 2);
                    }
                    if (pilot_count == SYNC_LEN) {
                        state = STATE_DATA;
                        data = 0;
                        data_bit = 7;
                        stop_bit = false;
                        //printk(KERN_INFO "vlc: pulse_len = %luns\n", pulse_len);
                    }
                    break;

                case STATE_DATA:
                    if (likely(!stop_bit)) {
                        pulse_bit = (ts_diff < pulse_len ? 0 : 1);
                        //printk(KERN_INFO "vlc: nr = %d, bit = %d\n", data_bit, pulse_bit);
                        data |= (pulse_bit << data_bit);
                        if (likely(data_bit)) {
                            --data_bit;
                        } else {
                            next_head = (ring_head + 1) % RING_SIZE;
                            if (likely(next_head != ring_tail)) {
                                ring_data[ring_head] = data;
                                ring_head = next_head;
                                wake_up_interruptible(&wait_queue);
                            }
                            //printk(KERN_INFO "vlc: byte = %d\n", data);
                            data = 0;
                            data_bit = 7;
                            stop_bit = true;
                        }
                    } else {
                        stop_bit = false;
                    }
                    break;
            }
        }
    }

    return IRQ_HANDLED;
}

static void unhook_gpio_irq(void) {
    if (configured) {
        configured = false;
        free_irq(irq_nr, NULL);
        gpio_free(gpio_vlc); 
    }
}

static int hook_gpio_irq(unsigned int new_gpio) {
    int result = 0;
    unhook_gpio_irq();
    
    gpio_vlc = new_gpio;
    result = gpio_request(gpio_vlc, CLASS_NAME);
    if (result) {
        return result;
    }

    gpio_direction_input(gpio_vlc);
    gpio_set_debounce(gpio_vlc, debounce);
    irq_nr = gpio_to_irq(gpio_vlc);
    result = request_irq(
        irq_nr, 
        vlc_irq_handler,
        IRQF_TRIGGER_RISING,
        "vlc_irq_handler",
        NULL
    );
    reset_state();
    ring_head = 0;
    ring_tail = 0;
    configured = true;
    //printk(KERN_INFO "interrupt is mapped to %d\n", irq_nr);

    return result;
}

static ssize_t gpio_vlc_show(struct device *dev, struct device_attribute *attr, char *buf) {
    if (configured) {
        return sprintf(buf, "%d\n", gpio_vlc);
    }

    return sprintf(buf, "%s\n", "off");
}

static ssize_t gpio_vlc_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    int new_gpio = 0;
    if (!strncmp(buf, "off", (count > 3 ? 3 : count))) {
        unhook_gpio_irq();
    } else {
        sscanf(buf, "%du", &new_gpio);
        if (new_gpio < 0 || hook_gpio_irq(new_gpio)) {
            return -EINVAL;
        }
    }

    return count;
}

static struct device_attribute dev_attr_vlc = {
    .attr = {
        .name = "gpio",
        .mode = ACCESS_MODE,
    },
    .show = gpio_vlc_show,
    .store = gpio_vlc_store,
};

static ssize_t delay_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%lu\n", delay);
}

static ssize_t delay_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    long new_delay = 0;
    sscanf(buf, "%lu", &new_delay);
    if (new_delay >= 0 && new_delay < 500000000) {
        delay = new_delay;
        return count;
    }

    return -EINVAL;
}

static struct device_attribute dev_attr_delay = {
    .attr = {
        .name = "delay",
        .mode = ACCESS_MODE,
    },
    .show = delay_show,
    .store = delay_store,
};

static ssize_t debounce_show(struct device *dev, struct device_attribute *attr, char *buf) {
    return sprintf(buf, "%u\n", debounce);
}

static ssize_t debounce_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    sscanf(buf, "%u", &debounce);
    if (configured) {
        gpio_set_debounce(gpio_vlc, debounce);
    }

    return count;
}

static struct device_attribute dev_attr_debounce = {
    .attr = {
        .name = "debounce",
        .mode = ACCESS_MODE,
    },
    .show = debounce_show,
    .store = debounce_store,
};

static int dev_open(struct inode *inodep, struct file *filep) {
    if(!mutex_trylock(&dev_mutex)) {
        return -EBUSY;
    }

    return 0;
}

static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    int error_count = 0;
    char chr = 0;
    ssize_t result = 0;

    while (ring_head == ring_tail) {
        if (filep->f_flags & O_NONBLOCK) {
            result = -EAGAIN;
            goto read_out;
        }
        if (wait_event_interruptible(wait_queue, ring_head != ring_tail)) {
            result = -ERESTARTSYS;
            goto read_out;
        }
    }

    while (ring_head != ring_tail && len--) {
        chr = ring_data[ring_tail];
        error_count = copy_to_user(buffer++, &chr, 1);
        ring_tail = (ring_tail + 1) % RING_SIZE;
        result++;
        if (error_count) {
            result = -EFAULT;
            goto read_out;
        }
    }
    
read_out:
    return result;
}

static int dev_release(struct inode *inodep, struct file *filep) {
    mutex_unlock(&dev_mutex);

    return 0;
}

static struct file_operations fops = {
    .open = dev_open,
    .read = dev_read,
    .release = dev_release,
};

static int __init gpiovlc_init(void) {
    int result = 0;
    
    printk(KERN_INFO "%s: %s (v%s)\n", CLASS_NAME, DRIVER_DESCRIPTION, DRIVER_VERSION);

    init_waitqueue_head(&wait_queue);

    major_nr = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_nr < 0) {
        printk(KERN_ALERT "%s: failed to register a major number\n", CLASS_NAME);
        return major_nr;
    }

    vlc_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(vlc_class)) {
        unregister_chrdev(major_nr, DEVICE_NAME);
        printk(KERN_ALERT "%s: failed to register vlc device\n", CLASS_NAME);
        return PTR_ERR(vlc_class);
    }

    vlc_device = device_create(vlc_class, NULL, MKDEV(major_nr, 0), NULL, DEVICE_NAME);
    if (IS_ERR(vlc_device)) {
        class_destroy(vlc_class);
        unregister_chrdev(major_nr, DEVICE_NAME);
        printk(KERN_ALERT "%s: failed to create vlc device\n", CLASS_NAME);
        return PTR_ERR(vlc_device);
    }

    result = device_create_file(vlc_device, &dev_attr_vlc);
    result = device_create_file(vlc_device, &dev_attr_delay); 
    result = device_create_file(vlc_device, &dev_attr_debounce);

    mutex_init(&dev_mutex);
    ts_last = ktime_get();
    reset_state();
    
    return result;
}

static void __exit gpiovlc_exit(void) {
    mutex_destroy(&dev_mutex);
    device_remove_file(vlc_device, &dev_attr_debounce);
    device_remove_file(vlc_device, &dev_attr_delay);
    device_remove_file(vlc_device, &dev_attr_vlc);
    device_destroy(vlc_class, MKDEV(major_nr, 0));
    class_unregister(vlc_class);
    class_destroy(vlc_class);
    unregister_chrdev(major_nr, DEVICE_NAME);
    unhook_gpio_irq();
    printk(KERN_INFO "%s: module unloaded\n", CLASS_NAME);
}

module_init(gpiovlc_init);
module_exit(gpiovlc_exit);
