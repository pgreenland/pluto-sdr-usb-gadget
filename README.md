# SDR USB Gadget

This repository implements a simple(ish) daemon which provides a Linux USB Gadget, attempting to perform a high performance interface to transfer IIO buffers.

It was specifically developed for the Analog Adalm Pluto, however could be adapted for other devices utilizing Linux's IIO interface.

Linux AIO and USB Gadget form provide a vendor specific USB interface.

Bulk IN transfers to the interface are queued for transmit via the DAC DMA with the help of its IIO interface.

ADC DMA transfers arriving via the IIO interface are queued for transmission on the USB bulk OUT interface.
