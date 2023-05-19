/* Public header file */
#include "usb_descriptors.h"

/* Standard / system libraries */
#include <byteswap.h>
#include <linux/usb/functionfs.h>
#include <stdio.h>
#include <unistd.h>

/* Macros */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	#define htole16(x) (x)
	#define htole32(x) (x)
#else
	#define htole16(x) __builtin_bswap16(x)
	#define htole32(x) __builtin_bswap32(x)
#endif

/* Definitions */
#define MAX_BULK_TRANSFER_HS (512)
#define INTERFACE_NAME "sdrgadget"

/* Private variables */
static const struct
{
	struct usb_functionfs_descs_head_v2 header;
	__le32 fs_count;
	__le32 hs_count;
	struct
	{
		struct usb_interface_descriptor intf;
		struct usb_endpoint_descriptor_no_audio bulk_sink;
		struct usb_endpoint_descriptor_no_audio bulk_source;
	} __attribute__ ((__packed__)) fs_descs, hs_descs;
} __attribute__ ((__packed__)) usb_descriptors =
{
	.header =
	{
		.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
		.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC),
		.length = htole32(sizeof(usb_descriptors)),
	},
	.fs_count = htole32(3),
	.fs_descs = {
		.intf = {
			.bLength = sizeof(usb_descriptors.fs_descs.intf),
			.bDescriptorType = USB_DT_INTERFACE,
			.bNumEndpoints = 2,
			.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
			.iInterface = 1,
		},
		.bulk_sink = {
			.bLength = sizeof(usb_descriptors.fs_descs.bulk_sink),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 1 | USB_DIR_IN,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
		},
		.bulk_source = {
			.bLength = sizeof(usb_descriptors.fs_descs.bulk_source),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 2 | USB_DIR_OUT,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
		},
	},
	.hs_count = htole32(3),
	.hs_descs =
	{
		.intf =
		{
			.bLength = sizeof(usb_descriptors.hs_descs.intf),
			.bDescriptorType = USB_DT_INTERFACE,
			.bNumEndpoints = 2,
			.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
			.iInterface = 1,
		},
		.bulk_sink =
		{
			.bLength = sizeof(usb_descriptors.hs_descs.bulk_sink),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 1 | USB_DIR_IN,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = htole16(MAX_BULK_TRANSFER_HS),
		},
		.bulk_source =
		{
			.bLength = sizeof(usb_descriptors.hs_descs.bulk_source),
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 2 | USB_DIR_OUT,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = htole16(MAX_BULK_TRANSFER_HS),
		},
	},
};

static const struct
{
	struct usb_functionfs_strings_head header;
	struct
	{
		__le16 code;
		const char str1[sizeof(INTERFACE_NAME)];
	} __attribute__ ((__packed__)) lang0;
} __attribute__ ((__packed__)) usb_strings =
{
	.header =
	{
		.magic = htole32(FUNCTIONFS_STRINGS_MAGIC),
		.length = htole32(sizeof(usb_strings)),
		.str_count = htole32(1),
		.lang_count = htole32(1),
	},
	.lang0 =
	{
		htole16(0x0409), /* en-us */
		INTERFACE_NAME,
	},
};

/* Public functions */
bool USB_DESCRIPTORS_WriteToEP0(int fd)
{
	/* Write descriptors */
	if (write(fd, &usb_descriptors, sizeof(usb_descriptors)) < 0)
	{
		perror("Failed to write descriptors");
		return false;
	}

	/* Write strings */
	if (write(fd, &usb_strings, sizeof(usb_strings)) < 0)
	{
		perror("Failed to write strings");
		return false;
	}

	return true;
}
