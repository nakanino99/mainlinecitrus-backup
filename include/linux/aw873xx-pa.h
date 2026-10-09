/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_AW873XX_PA_H
#define _LINUX_AW873XX_PA_H

#include <linux/types.h>

struct i2c_client;

int aw873xx_pa_set_enable(struct i2c_client *client, bool on);

#endif /* _LINUX_AW873XX_PA_H */
