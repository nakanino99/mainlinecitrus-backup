// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal mainline driver for AWINIC AW87359 / AW87519 audio PAs
 * used on Xiaomi Poco M3 (SM6115, codename "citrus").
 *
 * This is a deliberately stripped-down port: unlike the downstream
 * techpack driver, it does NOT implement the request_firmware()
 * profile-switching system (dspk/drcv/abspk/abrcv .bin containers).
 * It simply writes a fixed register table extracted from the
 * device's own vendor.img firmware blobs (aw87359_dspk.bin /
 * aw87519_kspk.bin) once at probe time. This is enough to bring
 * the PA into its default "speaker" output mode.
 *
 * Register tables below were extracted byte-for-byte from:
 *   /vendor/firmware/aw87359_dspk.bin  (44 bytes, AW87359 default speaker)
 *   /vendor/firmware/aw87519_kspk.bin  (42 bytes, AW87519 "K speaker")
 * confirmed against the parsing loop in the downstream
 * techpack/audio/asoc/codecs/aw8735{9,19}_audio.c
 * (`for (i = 0; i < cont->size; i += 2)`, no header).
 *
 * NOT implemented (future work): amixer/ALSA control to switch
 * between speaker/earpiece profiles, runtime PM, IRQ handling.
 * Right now the chip is simply programmed once at boot into its
 * default speaker-output mode and left there.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/aw873xx-pa.h>

/* ---- AW87359 (earpiece/top speaker PA, no reset pin) ---- */
static const u8 aw87359_dspk_regs[][2] = {
	{ 0x70, 0x80 },
	{ 0x01, 0x00 },
	{ 0x01, 0x00 },
	{ 0x02, 0x0c },
	{ 0x03, 0x06 },
	{ 0x04, 0x05 },
	{ 0x05, 0x0c },
	{ 0x06, 0x08 },
	{ 0x07, 0xb2 },
	{ 0x08, 0x09 },
	{ 0x09, 0x08 },
	{ 0x0a, 0x4b },
	{ 0x61, 0xbb },
	{ 0x62, 0x80 },
	{ 0x63, 0x29 },
	{ 0x64, 0x58 },
	{ 0x65, 0xcd },
	{ 0x66, 0x3c },
	{ 0x67, 0x2f },
	{ 0x68, 0x07 },
	{ 0x69, 0xdb },
	{ 0x01, 0x0d }, /* enable, must be last */
};

/* ---- AW87519 (bottom/main speaker PA, has reset pin) ---- */
static const u8 aw87519_kspk_regs[][2] = {
	{ 0x69, 0xb7 },
	{ 0x69, 0xb7 },
	{ 0x02, 0x09 },
	{ 0x03, 0xe8 },
	{ 0x04, 0x11 },
	{ 0x05, 0x0c },
	{ 0x06, 0x4b },
	{ 0x07, 0xb6 },
	{ 0x08, 0x0b },
	{ 0x09, 0x08 },
	{ 0x0a, 0x4b },
	{ 0x60, 0x16 },
	{ 0x61, 0x20 },
	{ 0x62, 0x01 },
	{ 0x63, 0x0b },
	{ 0x64, 0xc5 },
	{ 0x65, 0xa4 },
	{ 0x66, 0x78 },
	{ 0x67, 0xc4 },
	{ 0x68, 0x90 },
	{ 0x01, 0xf0 }, /* enable, must be last */
};

struct aw873xx_pa {
	struct i2c_client *client;
	struct gpio_desc *reset_gpio; /* NULL for aw87359 */
	const u8 (*regs)[2];
	size_t nregs;
	u8 on_val;
	u8 off_val;
};

static int aw873xx_write_table(struct aw873xx_pa *pa)
{
	int i, ret;

	for (i = 0; i < pa->nregs; i++) {
		ret = i2c_smbus_write_byte_data(pa->client,
						 pa->regs[i][0],
						 pa->regs[i][1]);
		if (ret < 0) {
			dev_err(&pa->client->dev,
				"reg write 0x%02x=0x%02x failed: %d\n",
				pa->regs[i][0], pa->regs[i][1], ret);
			return ret;
		}
	}
	return 0;
}

static int aw873xx_probe(struct i2c_client *client)
{
	struct aw873xx_pa *pa;
	int ret;

	pa = devm_kzalloc(&client->dev, sizeof(*pa), GFP_KERNEL);
	if (!pa)
		return -ENOMEM;

	pa->client = client;
	i2c_set_clientdata(client, pa);

	if (of_device_is_compatible(client->dev.of_node, "awinic,aw87519_pa")) {
		pa->regs = aw87519_kspk_regs;
		pa->nregs = ARRAY_SIZE(aw87519_kspk_regs);
		pa->on_val = 0xf0;	/* BARU */
		pa->off_val = 0x00;	/* BARU */

		/* AW87519 has a hardware reset line; AW87359 does not. */
		pa->reset_gpio = devm_gpiod_get(&client->dev, "reset",
						 GPIOD_OUT_LOW);
		if (IS_ERR(pa->reset_gpio)) {
			dev_err(&client->dev, "failed to get reset gpio\n");
			return PTR_ERR(pa->reset_gpio);
		}

		/* Pulse reset: hold low, then release before I2C access. */
		gpiod_set_value_cansleep(pa->reset_gpio, 0);
		usleep_range(1000, 1500);
		gpiod_set_value_cansleep(pa->reset_gpio, 1);
		usleep_range(1000, 1500);
	} else {
		pa->regs = aw87359_dspk_regs;
		pa->nregs = ARRAY_SIZE(aw87359_dspk_regs);
		pa->on_val = 0x0d;	/* BARU */
		pa->off_val = 0x04;	/* BARU */
		pa->reset_gpio = NULL;
	}

	ret = aw873xx_write_table(pa);
	if (ret)
		return ret;

	dev_info(&client->dev, "AW873xx PA programmed (%zu regs)\n",
		 pa->nregs);
	return 0;
}

/* BARU: seluruh fungsi di bawah ini */
int aw873xx_pa_set_enable(struct i2c_client *client, bool on)
{
	struct aw873xx_pa *pa;
	int ret;

	device_lock(&client->dev);
	pa = i2c_get_clientdata(client);
	if (!pa) {
		device_unlock(&client->dev);
		return -ENODEV;
	}
	ret = i2c_smbus_write_byte_data(pa->client, 0x01,
					on ? pa->on_val : pa->off_val);
	device_unlock(&client->dev);

	return ret;
}
EXPORT_SYMBOL_GPL(aw873xx_pa_set_enable);

static const struct of_device_id aw873xx_of_match[] = {
	{ .compatible = "awinic,aw87359_pa" },
	{ .compatible = "awinic,aw87519_pa" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw873xx_of_match);

static const struct i2c_device_id aw873xx_i2c_id[] = {
	{ "aw87359_pa", 0 },
	{ "aw87519_pa", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw873xx_i2c_id);

static struct i2c_driver aw873xx_i2c_driver = {
	.driver = {
		.name = "aw873xx_pa_minimal",
		.of_match_table = aw873xx_of_match,
	},
	.probe = aw873xx_probe,
	.id_table = aw873xx_i2c_id,
};
module_i2c_driver(aw873xx_i2c_driver);

MODULE_DESCRIPTION("Minimal AWINIC AW87359/AW87519 PA driver (no firmware system)");
MODULE_LICENSE("GPL");
