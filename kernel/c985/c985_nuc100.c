// SPDX-License-Identifier: GPL-2.0
/* c985_nuc100.c - NUC100 MCU access via GPIO bit-bang I2C (pins 14=SCL 15=SDA)
 *
 * Transport ground truth: host GPIO bit-bang is the only silicon-proven path
 * to the NUC100 (vault dmesg: "NUC100: ID 0x393835, FW v0x16"). Firmware's own
 * I2C stack is reachable solely from a debug shell command and never runs on a
 * timer, so a host bus owner cannot collide with it. Boot-time gpio defaults
 * (BAR1+0x610=0) leave pins Hi-Z pulled up: idle 0x618 bits14/15 read high.
 *
 * Register map (Windows driver RE + legacy cross-checked):
 *   0x04 FW version   0x0B ID '9','8','5'
 *   0x1B busy (poll until 0)          0x1C bit2 = HDMI video present
 *   0x1D[7] timing nibble-packed {T,H,T,H} + PixelCNT (PCLK=0x34BC00/cnt kHz)
 *   0x26 sync polarity                0x26|0xFC decode
 * Downstream chip proxy: 0x0E {chip,cmd,reg,cnt} / 0x13 data / 0x12 commit.
 */

#include <linux/delay.h>
#include <linux/mutex.h>

#include "c985.h"

#define NUC_SCL			14
#define NUC_SDA			15
#define NUC_ADDR7		0x15	/* C985McuAddr 0x2B >> 1 */
#define NUC_I2C_DELAY_US	5

#define REG_GPIO_DIR		0x610
#define REG_GPIO_VAL		0x614
#define REG_GPIO_IN		0x618

static DEFINE_MUTEX(nuc_bus_lock);

/* Open-drain primitives: LOW = dir out + val 0; HIGH/Z = dir in (pull-up). */
static void gpio_drive_low(struct c985_dev *dev, int pin)
{
	u32 dir = c985_read_bar1(dev, REG_GPIO_DIR);
	u32 val = c985_read_bar1(dev, REG_GPIO_VAL);

	dir |= BIT(pin);
	val &= ~BIT(pin);
	c985_write_bar1(dev, REG_GPIO_DIR, dir);
	c985_write_bar1(dev, REG_GPIO_VAL, val);
}

static void gpio_release(struct c985_dev *dev, int pin)
{
	u32 dir = c985_read_bar1(dev, REG_GPIO_DIR);

	dir &= ~BIT(pin);
	c985_write_bar1(dev, REG_GPIO_DIR, dir);
}

static int gpio_read(struct c985_dev *dev, int pin)
{
	gpio_release(dev, pin);
	return (c985_read_bar1(dev, REG_GPIO_IN) & BIT(pin)) ? 1 : 0;
}

static inline void i2c_delay(void)
{
	udelay(NUC_I2C_DELAY_US);
}

static void i2c_start(struct c985_dev *dev)
{
	gpio_release(dev, NUC_SDA); i2c_delay();
	gpio_release(dev, NUC_SCL); i2c_delay();
	gpio_drive_low(dev, NUC_SDA); i2c_delay();
	gpio_drive_low(dev, NUC_SCL); i2c_delay();
}

static void i2c_stop(struct c985_dev *dev)
{
	gpio_drive_low(dev, NUC_SDA); i2c_delay();
	gpio_release(dev, NUC_SCL); i2c_delay();
	gpio_release(dev, NUC_SDA); i2c_delay();
}

/* Returns ACK status: 1 = acked, 0 = NAK. */
static int i2c_tx_byte(struct c985_dev *dev, u8 byte)
{
	int i, ack;

	for (i = 7; i >= 0; i--) {
		if ((byte >> i) & 1)
			gpio_release(dev, NUC_SDA);
		else
			gpio_drive_low(dev, NUC_SDA);
		i2c_delay();
		gpio_release(dev, NUC_SCL); i2c_delay();
		gpio_drive_low(dev, NUC_SCL); i2c_delay();
	}

	gpio_release(dev, NUC_SDA); i2c_delay();
	gpio_release(dev, NUC_SCL); i2c_delay();
	ack = gpio_read(dev, NUC_SDA) == 0;
	gpio_drive_low(dev, NUC_SCL); i2c_delay();

	return ack;
}

static u8 i2c_rx_byte(struct c985_dev *dev, bool master_acks)
{
	u8 val = 0;
	int i;

	gpio_release(dev, NUC_SDA);

	for (i = 7; i >= 0; i--) {
		gpio_release(dev, NUC_SCL); i2c_delay();
		if (gpio_read(dev, NUC_SDA))
			val |= BIT(i);
		gpio_drive_low(dev, NUC_SCL); i2c_delay();
	}

	if (master_acks)
		gpio_drive_low(dev, NUC_SDA);
	else
		gpio_release(dev, NUC_SDA);
	i2c_delay();
	gpio_release(dev, NUC_SCL); i2c_delay();
	gpio_drive_low(dev, NUC_SCL); i2c_delay();
	gpio_release(dev, NUC_SDA);

	return val;
}

/* Write-then-read: [addr+W, reg] repeated-start [addr+R, len bytes]. */
static int nuc_wtr(struct c985_dev *dev, u8 reg, u8 *rbuf, int rlen)
{
	int i;

	mutex_lock(&nuc_bus_lock);

	i2c_start(dev);
	if (!i2c_tx_byte(dev, NUC_ADDR7 << 1))
		goto nak;
	if (!i2c_tx_byte(dev, reg))
		goto nak;

	i2c_start(dev);
	if (!i2c_tx_byte(dev, (NUC_ADDR7 << 1) | 1))
		goto nak;

	for (i = 0; i < rlen; i++)
		rbuf[i] = i2c_rx_byte(dev, i < rlen - 1);

	i2c_stop(dev);
	mutex_unlock(&nuc_bus_lock);
	return 0;

nak:
	i2c_stop(dev);
	mutex_unlock(&nuc_bus_lock);
	return -EIO;
}

/*
 * Full status sample. Timing field order follows the legacy silicon-tested
 * decoder; raw bytes are returned so the alternative KB ordering can be
 * calibrated live (1080p reference: T=2200 H=1920 T=1125 V=1080).
 */
int c985_nuc100_status(struct c985_dev *dev, u8 id3[3], u8 *status,
		       u16 *hact, u16 *vact, u16 *htot, u16 *vtot,
		       int *pclk_khz, u8 raw7[7])
{
	int ret, tries;

	ret = nuc_wtr(dev, 0x0B, id3, 3);
	if (ret)
		return ret;

	/* busy gate @0x1B: poll until zero */
	for (tries = 0; tries < 5; tries++) {
		u8 busy;

		ret = nuc_wtr(dev, 0x1B, &busy, 1);
		if (ret)
			return ret;
		if (!busy)
			break;
		msleep(20);
	}
	if (tries == 5)
		return -ETIMEDOUT;

	ret = nuc_wtr(dev, 0x1C, status, 1);
	if (ret)
		return ret;

	*hact = *vact = *htot = *vtot = 0;
	*pclk_khz = 0;
	memset(raw7, 0, 7);

	if (*status & 0x04) {
		ret = nuc_wtr(dev, 0x1D, raw7, 7);
		if (ret)
			return ret;

		*htot  = ((raw7[1] & 0x0F) << 8) | raw7[0];
		*hact  = ((raw7[1] & 0xF0) << 4) | raw7[2];
		*vtot  = ((raw7[4] & 0x0F) << 8) | raw7[3];
		*vact  = ((raw7[4] & 0xF0) << 4) | raw7[5];
		if (raw7[6])
			*pclk_khz = 0x34BC00 / raw7[6];
	}
	return 0;
}
