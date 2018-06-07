/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2018 Linaro Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* Bit bang driver for the 96Boards Developerbox (a.k.a. Synquacer E-series)
 * on-board debug UART.  The Developerbox implements its debug UART using a
 * CP2102N, a USB to UART bridge will also provides four GPIO pins. On
 * Developerbox these can be hooked up to the onboard SPI NOR FLASH and used
 * for emergency de-brick without any additional hardware programmer. Bit
 * banging over USB is extremely slow compared to a proper SPI programmer so
 * this is only practical as a de-brick tool.
 *
 * Schematic is available here:
 * https://www.96boards.org/documentation/enterprise/developerbox/hardware-docs/
 *
 * To prepare a Developerbox for programming via the debug UART DSW4 must be
 * changed from the default 00000000 to 10001000 (DSW4-1 and DSW4-5 should be
 * turned on)
 */

#include "platform.h"

#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <libusb.h>
#include "flash.h"
#include "chipdrivers.h"
#include "programmer.h"
#include "spi.h"

/* Bit positions for each pin. */
#define DEVELOPERBOX_SPI_SCK		0
#define DEVELOPERBOX_SPI_CS		1
#define DEVELOPERBOX_SPI_MISO		2
#define DEVELOPERBOX_SPI_MOSI		3

/* Config request types */
#define REQTYPE_HOST_TO_DEVICE  0x40
#define REQTYPE_DEVICE_TO_HOST  0xc0

/* Config request codes */
#define CP210X_VENDOR_SPECIFIC  0xFF

/* CP210X_VENDOR_SPECIFIC */
#define CP210X_WRITE_LATCH      0x37E1
#define CP210X_READ_LATCH       0x00C2

const struct dev_entry devs_developerbox[] = {
	{0x10C4, 0xEA60, OK, "Silicon Labs", "CP2102N USB to UART Bridge Controller"},
	{0},
};

struct libusb_context *usb_ctx;
static libusb_device_handle *cp210x_handle;

static int cp210x_gpio_get()
{
	int res;
	uint8_t gpio;

	res = libusb_control_transfer(cp210x_handle, REQTYPE_DEVICE_TO_HOST,
			CP210X_VENDOR_SPECIFIC, CP210X_READ_LATCH,
			0, &gpio, 1, 0);
	if (res < 0) {
		msg_perr("Failed to read GPIO pins (%s)\n", libusb_error_name(res));
		return 0;
	}

	return gpio;
}

static void cp210x_gpio_set(uint8_t val, uint8_t mask)
{
	int res;
	uint16_t gpio;

	gpio = ((val & 0xf) << 8) | (mask & 0xf);

	/* Set relay state on the card */
	res = libusb_control_transfer(cp210x_handle, REQTYPE_HOST_TO_DEVICE,
			CP210X_VENDOR_SPECIFIC, CP210X_WRITE_LATCH,
			gpio, NULL, 0, 0);
	if (res < 0)
		msg_perr("Failed to read GPIO pins (%s)\n", libusb_error_name(res));
}

static void cp210x_bitbang_set_cs(int val)
{
	cp210x_gpio_set(val << DEVELOPERBOX_SPI_CS, 1 << DEVELOPERBOX_SPI_CS);
}

static void cp210x_bitbang_set_sck(int val)
{
	cp210x_gpio_set(val << DEVELOPERBOX_SPI_SCK, 1 << DEVELOPERBOX_SPI_SCK);
}

static void cp210x_bitbang_set_mosi(int val)
{
	cp210x_gpio_set(val << DEVELOPERBOX_SPI_MOSI, 1 << DEVELOPERBOX_SPI_MOSI);
}

static int cp210x_bitbang_get_miso(void)
{
	return !!(cp210x_gpio_get() & (1 << DEVELOPERBOX_SPI_MISO));
}
static void cp210x_bitbang_set_sck_set_mosi(int sck, int mosi)
{
	cp210x_gpio_set(sck << DEVELOPERBOX_SPI_SCK | mosi << DEVELOPERBOX_SPI_MOSI,
			  1 << DEVELOPERBOX_SPI_SCK |    1 << DEVELOPERBOX_SPI_MOSI);
}

static const struct bitbang_spi_master bitbang_spi_master_cp210x = {
	.type = BITBANG_SPI_MASTER_DEVELOPERBOX,
	.set_cs = cp210x_bitbang_set_cs,
	.set_sck = cp210x_bitbang_set_sck,
	.set_mosi = cp210x_bitbang_set_mosi,
	.get_miso = cp210x_bitbang_get_miso,
	.set_sck_set_mosi = cp210x_bitbang_set_sck_set_mosi,
};

static struct libusb_device_handle *get_device_by_vid_pid_serial(uint16_t vid, uint16_t pid, char *serialno)
{
	struct libusb_device **list;
	ssize_t count = libusb_get_device_list(usb_ctx, &list);
	if (count < 0) {
		msg_perr("Getting the USB device list failed (%s)!\n", libusb_error_name(count));
		return NULL;
	}

	ssize_t i = 0;
	for (i = 0; i < count; i++) {
		struct libusb_device *dev = list[i];
		struct libusb_device_descriptor desc;
		struct libusb_device_handle *handle;

		int res = libusb_get_device_descriptor(dev, &desc);
		if (res != 0) {
			msg_perr("Reading the USB device descriptor failed (%s)!\n", libusb_error_name(res));
			continue;
		}

		if ((desc.idVendor != vid) && (desc.idProduct != pid))
			continue;

		msg_pdbg("Found USB device %04"PRIx16":%04"PRIx16" at address %d-%d.\n",
			 desc.idVendor, desc.idProduct,
			 libusb_get_bus_number(dev), libusb_get_device_address(dev));

		res = libusb_open(dev, &handle);
		if (res != 0) {
			msg_perr("Opening the USB device failed (%s)!\n", libusb_error_name(res));
			continue;
		}

		if (serialno) {
			unsigned char myserial[64];
			res = libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, myserial, 64);
			if (res < 0) {
				msg_perr("Reading the USB serialno failed (%s)!\n", libusb_error_name(res));
				libusb_close(handle);
				continue;
			}
			msg_pdbg("Serial number is %s\n", myserial);

			if (0 != strncmp(serialno, (char *) myserial, strlen(serialno))) {
				libusb_close(handle);
				continue;
			}
		}

		libusb_free_device_list(list, 1);
		return handle;
	}

	libusb_free_device_list(list, 1);
	return NULL;
}

static int developerbox_spi_shutdown(void *data)
{
	libusb_close(cp210x_handle);
	libusb_exit(usb_ctx);

	return 0;
}

int developerbox_spi_init()
{
	char *serialno = extract_programmer_param("serial");
	if (serialno)
		msg_pinfo("Looking for serial number commencing %s\n", serialno);

	libusb_init(&usb_ctx);
	if (!usb_ctx) {
		msg_perr("Could not initialize libusb!\n");
		free(serialno);
		return 1;
	}

	const uint16_t vid = devs_developerbox[0].vendor_id;
	const uint16_t pid = devs_developerbox[0].device_id;
	cp210x_handle = get_device_by_vid_pid_serial(vid, pid, serialno);
	free(serialno);
	if (!cp210x_handle) {
		msg_perr("Could not find a Developerbox programmer on USB.\n");
		libusb_exit(usb_ctx);
		return 1;
	}

	if (register_shutdown(developerbox_spi_shutdown, NULL))
		return 1;

	if (register_spi_bitbang_master(&bitbang_spi_master_cp210x)) {
		/* This should never happen. */
		msg_perr("Developerbox bitbang SPI master init failed!\n");
		return 1;
	}

	return 0;
}
