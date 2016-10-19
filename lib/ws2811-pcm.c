/*
 * ws2811-pcm.c
 *
 * Copyright (c) 2014 Jeremy Garff <jer @ jers.net>
 * Adapted for PCM: 2016 Ton van Overbeek <tvoverbeek @ gmail.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *
 *     1.  Redistributions of source code must retain the above copyright notice, this list of
 *         conditions and the following disclaimer.
 *     2.  Redistributions in binary form must reproduce the above copyright notice, this list
 *         of conditions and the following disclaimer in the documentation and/or other materials
 *         provided with the distribution.
 *     3.  Neither the name of the owner nor the names of its contributors may be used to endorse
 *         or promote products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */


#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <signal.h>

#include "mailbox.h"
#include "clk.h"
#include "gpio.h"
#include "dma.h"
#include "pcm.h"
#include "rpihw.h"

#include "gamma.h"

#include "ws2811-pcm.h"


#define BUS_TO_PHYS(x)                           ((x)&~0xC0000000)

#define OSC_FREQ                                 19200000   // crystal frequency

/* 3 colors, 8 bits per byte, 3 symbols per bit + 55uS low for reset signal */
#define LED_RESET_uS                             55
#define LED_BIT_COUNT(leds, freq)                ((leds * 3 * 8 * 3) + ((LED_RESET_uS * \
                                                  (freq * 3)) / 1000000))

// Pad out to the nearest uint32 + 32-bits for idle low/high times the number of channels
#define PCM_BYTE_COUNT(leds, freq)               ((((LED_BIT_COUNT(leds, freq) >> 3) & ~0x7) + 4) + 4)

#define SYMBOL_HIGH                              0x6  // 1 1 0
#define SYMBOL_LOW                               0x4  // 1 0 0

#define ARRAY_SIZE(stuff)                        (sizeof(stuff) / sizeof(stuff[0]))


// We use the mailbox interface to request memory from the VideoCore.
// This lets us request one physically contiguous chunk, find its
// physical address, and map it 'uncached' so that writes from this
// code are immediately visible to the DMA controller.  This struct
// holds data relevant to the mailbox interface.
typedef struct videocore_mbox {
    int handle;             /* From mbox_open() */
    unsigned mem_ref;       /* From mem_alloc() */
    unsigned bus_addr;      /* From mem_lock() */
    unsigned size;          /* Size of allocation */
    uint8_t *virt_addr;     /* From mapmem() */
} videocore_mbox_t;

typedef struct ws2811_device
{
    volatile uint8_t *pcm_raw;
    volatile dma_t *dma;
    volatile pcm_t *pcm;
    volatile dma_cb_t *dma_cb;
    uint32_t dma_cb_addr;
    volatile gpio_t *gpio;
    volatile cm_pcm_t *cm_pcm;
    videocore_mbox_t mbox;
    int max_count;
} ws2811_device_t;

/**
 * Return the channel led count.
 *
 * @param    ws2811  ws2811 instance pointer.
 *
 * @returns  Number of LEDs in channel.
 */
static int max_channel_led_count(ws2811_t *ws2811)
{
    return ws2811->channel->count;
}

/**
 * Map all devices into userspace memory.
 *
 * @param    ws2811  ws2811 instance pointer.
 *
 * @returns  0 on success, -1 otherwise.
 */
static int map_registers(ws2811_t *ws2811)
{
    ws2811_device_t *device = ws2811->device;
    const rpi_hw_t *rpi_hw = ws2811->rpi_hw;
    uint32_t base = ws2811->rpi_hw->periph_base;
    uint32_t dma_addr;

    dma_addr = dmanum_to_offset(ws2811->dmanum);
    if (!dma_addr)
    {
        return -1;
    }
    dma_addr += rpi_hw->periph_base;

    device->dma = mapmem(dma_addr, sizeof(dma_t));
    if (!device->dma)
    {
        return -1;
    }

    device->pcm = mapmem(PCM_OFFSET + base, sizeof(pcm_t));
    if (!device->pcm)
    {
        return -1;
    }

    device->gpio = mapmem(GPIO_OFFSET + base, sizeof(gpio_t));
    if (!device->gpio)
    {
        return -1;
    }

    device->cm_pcm = mapmem(CM_PCM_OFFSET + base, sizeof(cm_pcm_t));
    if (!device->cm_pcm)
    {
        return -1;
    }

    return 0;
}

/**
 * Unmap all devices from virtual memory.
 *
 * @param    ws2811  ws2811 instance pointer.
 *
 * @returns  None
 */
static void unmap_registers(ws2811_t *ws2811)
{
    ws2811_device_t *device = ws2811->device;

    if (device->dma)
    {
        unmapmem((void *)device->dma, sizeof(dma_t));
    }

    if (device->pcm)
    {
        unmapmem((void *)device->pcm, sizeof(pcm_t));
    }

    if (device->cm_pcm)
    {
        unmapmem((void *)device->cm_pcm, sizeof(cm_pcm_t));
    }

    if (device->gpio)
    {
        unmapmem((void *)device->gpio, sizeof(gpio_t));
    }
}

/**
 * Given a userspace address pointer, return the matching bus address used by DMA.
 *     Note: The bus address is not the same as the CPU physical address.
 *
 * @param    addr   Userspace virtual address pointer.
 *
 * @returns  Bus address for use by DMA.
 */
static uint32_t addr_to_bus(ws2811_device_t *device, const volatile void *virt)
{
    videocore_mbox_t *mbox = &device->mbox;

    uint32_t offset = (uint8_t *)virt - mbox->virt_addr;

    return mbox->bus_addr + offset;
}

/**
 * Stop the PCM controller.
 *
 * @param    ws2811  ws2811 instance pointer.
 *
 * @returns  None
 */
static void stop_pcm(ws2811_t *ws2811)
{
    ws2811_device_t *device = ws2811->device;
    volatile pcm_t *pcm = device->pcm;
    volatile cm_pcm_t *cm_pcm = device->cm_pcm;

    // Turn off the PCM in case already running
    pcm->cs = 0;
    usleep(10);

    // Kill the clock if it was already running
    cm_pcm->ctl = CM_PCM_CTL_PASSWD | CM_PCM_CTL_KILL;
    usleep(10);
    while (cm_pcm->ctl & CM_PCM_CTL_BUSY)
        ;
}

/**
 * Setup the PCM controller with one 32-bit channel in a 32-bit frame using DMA to feed the PCM FIFO.
 *
 * @param    ws2811  ws2811 instance pointer.
 *
 * @returns  None
 */
static int setup_pcm(ws2811_t *ws2811)
{
    ws2811_device_t *device = ws2811->device;
    volatile dma_t *dma = device->dma;
    volatile dma_cb_t *dma_cb = device->dma_cb;
    volatile pcm_t *pcm = device->pcm;
    volatile cm_pcm_t *cm_pcm = device->cm_pcm;
    int maxcount = max_channel_led_count(ws2811);
    uint32_t freq = ws2811->freq;
    int32_t byte_count;

    stop_pcm(ws2811);

    // Setup the PCM Clock - Use OSC @ 19.2Mhz w/ 3 clocks/tick
    cm_pcm->div = CM_PCM_DIV_PASSWD | CM_PCM_DIV_DIVI(OSC_FREQ / (3 * freq));
    cm_pcm->ctl = CM_PCM_CTL_PASSWD | CM_PCM_CTL_SRC_OSC;
    cm_pcm->ctl = CM_PCM_CTL_PASSWD | CM_PCM_CTL_SRC_OSC | CM_PCM_CTL_ENAB;
    usleep(10);
    while (!(cm_pcm->ctl & CM_PCM_CTL_BUSY))
        ;

    // Setup the PCM, use delays as the block is rumored to lock up without them.  Make
    // sure to use a high enough priority to avoid any FIFO underruns, especially if
    // the CPU is busy doing lots of memory accesses, or another DMA controller is
    // busy.  The FIFO will clock out data at a much slower rate (2.6Mhz max), so
    // the odds of a DMA priority boost are extremely low.

    pcm->cs = RPI_PCM_CS_EN;            // Enable PCM hardware
    pcm->mode = (RPI_PCM_MODE_FLEN(31) | RPI_PCM_MODE_FSLEN(1));
                // Framelength 32, clock enabled, frame sync pulse
    pcm->txc = RPI_PCM_TXC_CH1WEX | RPI_PCM_TXC_CH1EN | RPI_PCM_TXC_CH1POS(0) | RPI_PCM_TXC_CH1WID(8);
               // Single 32-bit channel
    pcm->cs |= RPI_PCM_CS_TXCLR;        // Reset transmit fifo
    usleep(10);
    pcm->cs |= RPI_PCM_CS_DMAEN;         // Enable DMA DREQ
    pcm->dreq = (RPI_PCM_DREQ_TX(0x3F) | RPI_PCM_DREQ_TX_PANIC(0x10)); // Set FIFO tresholds

    // Initialize the DMA control block
    byte_count = PCM_BYTE_COUNT(maxcount, freq);
    dma_cb->ti = RPI_DMA_TI_NO_WIDE_BURSTS |  // 32-bit transfers
                 RPI_DMA_TI_WAIT_RESP |       // wait for write complete
                 RPI_DMA_TI_DEST_DREQ |       // user peripheral flow control
                 RPI_DMA_TI_PERMAP(2) |       // PCM TX peripheral
                 RPI_DMA_TI_SRC_INC;          // Increment src addr

    dma_cb->source_ad = addr_to_bus(device, device->pcm_raw);
    dma_cb->dest_ad = (uint32_t)&((pcm_t *)PCM_PERIPH_PHYS)->fifo;
    dma_cb->txfr_len = byte_count;
    dma_cb->stride = 0;
    dma_cb->nextconbk = 0;

    dma->cs = 0;
    dma->txfr_len = 0;

    return 0;
}

/**
 * Start the DMA feeding the PCM TX FIFO.  This will stream the entire DMA buffer.
 *
 * @param    ws2811  ws2811 instance pointer.
 *
 * @returns  None
 */
static void dma_start(ws2811_t *ws2811)
{
    ws2811_device_t *device = ws2811->device;
    volatile dma_t *dma = device->dma;
    volatile pcm_t *pcm = device->pcm;
    uint32_t dma_cb_addr = device->dma_cb_addr;

    dma->cs = RPI_DMA_CS_RESET;
    usleep(10);

    dma->cs = RPI_DMA_CS_INT | RPI_DMA_CS_END;
    usleep(10);

    dma->conblk_ad = dma_cb_addr;
    dma->debug = 7; // clear debug error flags
    dma->cs = RPI_DMA_CS_WAIT_OUTSTANDING_WRITES |
              RPI_DMA_CS_PANIC_PRIORITY(15) | 
              RPI_DMA_CS_PRIORITY(15) |
              RPI_DMA_CS_ACTIVE;

    pcm->cs |= RPI_PCM_CS_TXON;  // Start transmission
}

/**
 * Initialize the application selected GPIO pin for PCM operation.
 *
 * @param    ws2811  ws2811 instance pointer.
 *
 * @returns  0 on success, -1 on unsupported pin
 */
static int gpio_init(ws2811_t *ws2811)
{
    volatile gpio_t *gpio = ws2811->device->gpio;

    int pinnum = ws2811->channel->gpionum;

    if (pinnum) {
        int altnum = pcm_pin_alt(PCMFUN_DOUT, pinnum);

        if (altnum < 0) {
            return -1;
        }

        gpio_function_set(gpio, pinnum, altnum);
    }
    return 0;
}

/**
 * Initialize the PCM DMA buffer with all zeros.
 * The DMA buffer length is assumed to be a word multiple.
 *
 * @param    ws2811  ws2811 instance pointer.
 *
 * @returns  None
 */
void pcm_raw_init(ws2811_t *ws2811)
{
    volatile uint32_t *pcm_raw = (uint32_t *)ws2811->device->pcm_raw;
    int maxcount = max_channel_led_count(ws2811);
    int wordcount = PCM_BYTE_COUNT(maxcount, ws2811->freq) / sizeof(uint32_t);
    int i;

    for (i = 0; i < wordcount; i++) {
        pcm_raw[wordcount] = 0x0;
    }
}

/**
 * Cleanup previously allocated device memory and buffers.
 *
 * @param    ws2811  ws2811 instance pointer.
 *
 * @returns  None
 */
void ws2811_cleanup(ws2811_t *ws2811)
{
    ws2811_device_t *device = ws2811->device;

    if (ws2811->channel->leds) {
        free(ws2811->channel->leds);
    }
    ws2811->channel->leds = NULL;

    if (device->mbox.handle != -1) {
        videocore_mbox_t *mbox = &device->mbox;

        unmapmem(mbox->virt_addr, mbox->size);
        mem_unlock(mbox->handle, mbox->mem_ref);
        mem_free(mbox->handle, mbox->mem_ref);
        mbox_close(mbox->handle);

        mbox->handle = -1;
    }

    if (device) {
        free(device);
    }
    ws2811->device = NULL;
}


/*
 *
 * Application API Functions
 *
 */


/**
 * Allocate and initialize memory, buffers, pages, PCM, DMA, and GPIO.
 *
 * @param    ws2811  ws2811 instance pointer.
 *
 * @returns  0 on success, -1 otherwise.
 */
int ws2811_init(ws2811_t *ws2811)
{
    ws2811_device_t *device;
    const rpi_hw_t *rpi_hw;

    ws2811->rpi_hw = rpi_hw_detect();
    if (!ws2811->rpi_hw)
    {
        return -1;
    }
    rpi_hw = ws2811->rpi_hw;

    ws2811->device = malloc(sizeof(*ws2811->device));
    if (!ws2811->device)
    {
        return -1;
    }
    device = ws2811->device;

    // Determine how much physical memory we need for DMA
    device->mbox.size = PCM_BYTE_COUNT(max_channel_led_count(ws2811), ws2811->freq) +
                        sizeof(dma_cb_t);
    // Round up to page size multiple
    device->mbox.size = (device->mbox.size + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);

    device->mbox.handle = mbox_open();
    if (device->mbox.handle == -1)
    {
        return -1;
    }

    device->mbox.mem_ref = mem_alloc(device->mbox.handle, device->mbox.size, PAGE_SIZE,
                                     rpi_hw->videocore_base == 0x40000000 ? 0xC : 0x4);
    if (device->mbox.mem_ref == 0)
    {
       return -1;
    }

    device->mbox.bus_addr = mem_lock(device->mbox.handle, device->mbox.mem_ref);
    if (device->mbox.bus_addr == (uint32_t) ~0UL)
    {
       mem_free(device->mbox.handle, device->mbox.size);
       return -1;
    }
    device->mbox.virt_addr = mapmem(BUS_TO_PHYS(device->mbox.bus_addr), device->mbox.size);

    // Initialize all pointers to NULL.  Any non-NULL pointers will be freed on cleanup.
    device->pcm_raw = NULL;
    device->dma_cb = NULL;
    ws2811->channel->leds = NULL;

    // Allocate the LED buffer
    ws2811_channel_t *channel = ws2811->channel;

    channel->leds = malloc(sizeof(ws2811_led_t) * channel->count);
    if (!channel->leds) {
        goto err;
    }

    memset(channel->leds, 0, sizeof(ws2811_led_t) * channel->count);

    if (!channel->strip_type) {
      channel->strip_type=WS2811_STRIP_RGB;
    }

    device->dma_cb = (dma_cb_t *)device->mbox.virt_addr;
    device->pcm_raw = (uint8_t *)device->mbox.virt_addr + sizeof(dma_cb_t);

    pcm_raw_init(ws2811);

    memset((dma_cb_t *)device->dma_cb, 0, sizeof(dma_cb_t));

    // Cache the DMA control block bus address
    device->dma_cb_addr = addr_to_bus(device, device->dma_cb);

    // Map the physical registers into userspace
    if (map_registers(ws2811))
    {
        goto err;
    }

    // Initialize the GPIO pins
    if (gpio_init(ws2811))
    {
        unmap_registers(ws2811);
        goto err;
    }

    // Setup the PCM, clock, and DMA
    if (setup_pcm(ws2811))
    {
        unmap_registers(ws2811);
        goto err;
    }

    return 0;

err:
    ws2811_cleanup(ws2811);

    return -1;
}

/**
 * Shut down DMA, PCM, and cleanup memory.
 *
 * @param    ws2811  ws2811 instance pointer.
 *
 * @returns  None
 */
void ws2811_fini(ws2811_t *ws2811)
{
    volatile pcm_t *pcm = ws2811->device->pcm;

    ws2811_wait(ws2811);                     // Wait till DMA is finished
    while (!(pcm->cs & RPI_PCM_CS_TXE)) ;    // Wait till TX FIFO is empty

    stop_pcm(ws2811);

    unmap_registers(ws2811);

    ws2811_cleanup(ws2811);
}

/**
 * Wait for any executing DMA operation to complete before returning.
 *
 * @param    ws2811  ws2811 instance pointer.
 *
 * @returns  0 on success, -1 on DMA competion error
 */
int ws2811_wait(ws2811_t *ws2811)
{
    volatile dma_t *dma = ws2811->device->dma;

    while ((dma->cs & RPI_DMA_CS_ACTIVE) &&
           !(dma->cs & RPI_DMA_CS_ERROR))
    {
        usleep(10);
    }

    if (dma->cs & RPI_DMA_CS_ERROR)
    {
        fprintf(stderr, "DMA Error: %08x\n", dma->debug);
        return -1;
    }

    return 0;
}

/**
 * Render the PCM DMA buffer from the user supplied LED arrays and start the DMA
 * controller.  This will update all LEDs.
 *
 * @param    ws2811  ws2811 instance pointer.
 *
 * @returns  None
 */
int ws2811_render(ws2811_t *ws2811)
{
    volatile uint8_t *pcm_raw = ws2811->device->pcm_raw;
    int bitpos = 31;
    int i, k, l;
    unsigned j;

    ws2811_channel_t *channel = ws2811->channel;
    int wordpos = 0;
    int scale   = (channel->brightness & 0xff) + 1;
    int rshift  = (channel->strip_type >> 16) & 0xff;
    int gshift  = (channel->strip_type >> 8)  & 0xff;
    int bshift  = (channel->strip_type >> 0)  & 0xff;

    for (i = 0; i < channel->count; i++)                // Led 
    {   
        uint8_t color[] = {
            ws281x_gamma[(((channel->leds[i] >> rshift) & 0xff) * scale) >> 8], // red
            ws281x_gamma[(((channel->leds[i] >> gshift) & 0xff) * scale) >> 8], // green
            ws281x_gamma[(((channel->leds[i] >> bshift) & 0xff) * scale) >> 8], // blue
        };

        for (j = 0; j < ARRAY_SIZE(color); j++)        // Color
        {
            for (k = 7; k >= 0; k--)                   // Bit
            {
                uint8_t symbol = (channel->invert ? SYMBOL_HIGH : SYMBOL_LOW);

                if (color[j] & (1 << k)) {
                    symbol = (channel ->invert ? SYMBOL_LOW : SYMBOL_HIGH);
                }

                for (l = 2; l >= 0; l--)               // Symbol
                {
                    uint32_t *wordptr = &((uint32_t *)pcm_raw)[wordpos];

                    *wordptr &= ~(1 << bitpos);
                    if (symbol & (1 << l))
                    {
                        *wordptr |= (1 << bitpos);
                    }

                    bitpos--;
                    if (bitpos < 0) {
                        wordpos ++;
                        bitpos = 31;
                    }
                }
            }
        }
    }

    // Wait for any previous DMA operation to complete.
    if (ws2811_wait(ws2811))
    {
        return -1;
    }

    dma_start(ws2811);

    return 0;
}

