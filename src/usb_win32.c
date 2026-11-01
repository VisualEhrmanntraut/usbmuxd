/*
 * Changes the USB configuration of a device through libusb-win32, which
 * libusb-1.0 can't do on Windows. libusb0.dll is loaded at runtime because
 * its exported usb_init() would clash with usbmuxd's own usb_init().
 */
#include <string.h>

#include <winsock2.h>
#include <windows.h>

#include "usb_win32.h"
#include "usb.h"
#include "log.h"

/* Layout-compatible copies of the structures in libusb-win32's lusb0_usb.h */
struct usb0_device_descriptor {
	unsigned char  bLength, bDescriptorType;
	unsigned short bcdUSB;
	unsigned char  bDeviceClass, bDeviceSubClass, bDeviceProtocol;
	unsigned char  bMaxPacketSize0;
	unsigned short idVendor, idProduct, bcdDevice;
	unsigned char  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};

struct usb0_device {
	struct usb0_device *next, *prev;
	char filename[512];
	struct usb0_bus *bus;
	struct usb0_device_descriptor descriptor;
	void *config;
	void *dev;
	unsigned char devnum;
	unsigned char num_children;
	struct usb0_device **children;
};

struct usb0_bus {
	struct usb0_bus *next, *prev;
	char dirname[512];
	struct usb0_device *devices;
	unsigned long location;
	struct usb0_device *root_dev;
};

typedef void usb0_dev_handle;

static void (*p_usb_init)(void);
static int (*p_usb_find_busses)(void);
static int (*p_usb_find_devices)(void);
static struct usb0_bus *(*p_usb_get_busses)(void);
static usb0_dev_handle *(*p_usb_open)(struct usb0_device *);
static int (*p_usb_close)(usb0_dev_handle *);
static int (*p_usb_set_configuration)(usb0_dev_handle *, int);
static int (*p_usb_get_string_simple)(usb0_dev_handle *, int, char *, size_t);

static HMODULE libusb0_mod = NULL;

static int load_libusb0(void)
{
	libusb0_mod = LoadLibraryA("libusb0.dll");
	if (!libusb0_mod) {
		usbmuxd_log(LL_ERROR, "Could not load libusb0.dll (error %lu)", GetLastError());
		return -1;
	}

#define LOAD(name) do { \
	p_##name = (void *)GetProcAddress(libusb0_mod, #name); \
	if (!p_##name) { \
		usbmuxd_log(LL_ERROR, "libusb0.dll is missing symbol %s", #name); \
		FreeLibrary(libusb0_mod); \
		libusb0_mod = NULL; \
		return -1; \
	} \
} while (0)

	LOAD(usb_init);
	LOAD(usb_find_busses);
	LOAD(usb_find_devices);
	LOAD(usb_get_busses);
	LOAD(usb_open);
	LOAD(usb_close);
	LOAD(usb_set_configuration);
	LOAD(usb_get_string_simple);
#undef LOAD

	return 0;
}

void usb_win32_init(void)
{
	if (libusb0_mod || load_libusb0() < 0)
		return;
	p_usb_init();
}

static int is_apple_device(const struct usb0_device_descriptor *desc)
{
	if (desc->idVendor != VID_APPLE)
		return 0;
	return (desc->idProduct >= PID_RANGE_LOW && desc->idProduct <= PID_RANGE_MAX)
		|| (desc->idProduct >= PID_APPLE_SILICON_RESTORE_LOW && desc->idProduct <= PID_APPLE_SILICON_RESTORE_MAX)
		|| desc->idProduct == PID_APPLE_T2_COPROCESSOR;
}

static usb0_dev_handle *open_by_serial(const char *serial)
{
	struct usb0_bus *bus;
	struct usb0_device *dev;
	char want[64];
	size_t i = 0;

	/* usbmuxd inserts a '-' into 24 character serial numbers */
	for (; *serial && i < sizeof(want) - 1; serial++) {
		if (*serial != '-')
			want[i++] = *serial;
	}
	want[i] = '\0';

	p_usb_find_busses();
	p_usb_find_devices();

	for (bus = p_usb_get_busses(); bus; bus = bus->next) {
		for (dev = bus->devices; dev; dev = dev->next) {
			char dev_serial[64] = { 0 };
			usb0_dev_handle *handle;

			if (!is_apple_device(&dev->descriptor))
				continue;
			handle = p_usb_open(dev);
			if (!handle)
				continue;
			if (p_usb_get_string_simple(handle, dev->descriptor.iSerialNumber, dev_serial, sizeof(dev_serial)) > 0
			    && !strcmp(dev_serial, want))
				return handle;
			p_usb_close(handle);
		}
	}

	return NULL;
}

int usb_win32_set_configuration(const char *serial, uint8_t configuration)
{
	usb0_dev_handle *handle;
	int res;

	if (!libusb0_mod)
		return -1;

	handle = open_by_serial(serial);
	if (!handle) {
		usbmuxd_log(LL_WARNING, "Could not find device %s through libusb-win32", serial);
		return -1;
	}

	res = p_usb_set_configuration(handle, configuration);
	p_usb_close(handle);
	return res;
}
