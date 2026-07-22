#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdbool.h>

void usb_msc_init(void);
bool usb_msc_switch_to_usb(void);
bool usb_msc_switch_to_app(void);
bool usb_msc_is_app_mode(void);

#endif
