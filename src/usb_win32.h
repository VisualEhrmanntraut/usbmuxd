#ifndef USB_WIN32_H
#define USB_WIN32_H

#include <stdint.h>

void usb_win32_init(void);
int usb_win32_set_configuration(const char *serial, uint8_t configuration);

#endif
