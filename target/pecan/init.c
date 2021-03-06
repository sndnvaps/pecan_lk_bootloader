/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 * Copyright (c) 2009-2010, Code Aurora Forum. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name of Google, Inc. nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <debug.h>
#include <dev/keys.h>
#include <dev/gpio_keypad.h>
#include <lib/ptable.h>
#include <dev/flash.h>
#include <smem.h>
#include <platform/iomap.h>

#define LINUX_MACHTYPE  1007014

#define VARIABLE_LENGTH        0x10101010
#define DIFF_START_ADDR        0xF0F0F0F0
#define NUM_PAGES_PER_BLOCK    0x40

static struct ptable flash_ptable;

/* for these partitions, start will be offset by either what we get from
 * smem, or from the above offset if smem is not useful. Also, we should
 * probably have smem_ptable code populate our flash_ptable.
 *
 * When smem provides us with a full partition table, we can get rid of
 * this altogether.
 *
 */
static struct ptentry board_part_list[] = {
	{
		.start = 0,
		.length = 40 
		.name = "boot",
	},
	{
		.start = 56,
		.length = 512 
		.name = "cache",
	},
	{
		.start = 568,
		.length = 40 
		.name = "recovery",
	},
	{
		.start = 608,
		.length = 3 ,
		.name = "splash",
	},
	{
		.start = 611,
		.length = 1560
		.name = "system",
	},
	{
		.start = 1891,
		.length = 22 
		.name = "lgdrm",
	},
	{
		.start = 1913,
		.length = 1788 
		.name = "userdata",
	},
	{
		.start = 3701,
		.length = 8 ,
		.name = "misc",
	},
	{
		.start = -109,
		.length = 4 
		.name = "usd",
	},
	{
		.start = -105,
		.length = 45 
		.name = "pkg",
	},
	{
		.start = 0,
		.length = 0,
		.name = "",
	},
};
static int num_parts = sizeof(board_part_list)/sizeof(struct ptentry);

void smem_ptable_init(void);
unsigned smem_get_apps_flash_start(void);

void keypad_init(void);

int target_is_emmc_boot(void);

void target_init(void)
{
	unsigned offset;
	struct flash_info *flash_info;
	unsigned total_num_of_blocks;
	unsigned next_ptr_start_adr = 0;
	unsigned blocks_per_1MB = 8; /* Default value of 2k page size on 256MB flash drive*/
	int i;

	dprintf(INFO, "target_init()\n");

#if (!ENABLE_NANDWRITE)
	keys_init();
	keypad_init();
#endif

	if (target_is_emmc_boot())
	{
		if(mmc_boot_main(MMC_SLOT, MSM_SDC1_BASE))
		{
			dprintf(CRITICAL, "mmc init failed!");
			ASSERT(0);
		}
		return;
	}

	ptable_init(&flash_ptable);
	smem_ptable_init();

	flash_init();
	flash_info = flash_get_info();
	ASSERT(flash_info);

	offset = smem_get_apps_flash_start();
	if (offset == 0xffffffff)
	        while(1);

	total_num_of_blocks = flash_info->num_blocks;
	blocks_per_1MB = (1 << 20) / (flash_info->block_size);

	for (i = 0; i < num_parts; i++) {
		struct ptentry *ptn = &board_part_list[i];
		unsigned len = ((ptn->length) * blocks_per_1MB);

		if(ptn->start != 0)
		        ASSERT(ptn->start == DIFF_START_ADDR);

		ptn->start = next_ptr_start_adr;

		if(ptn->length == VARIABLE_LENGTH)
		{
			unsigned length_for_prt = 0;
			unsigned j;
			for (j = i+1; j < num_parts; j++)
			{
			        struct ptentry *temp_ptn = &board_part_list[j];
			        ASSERT(temp_ptn->length != VARIABLE_LENGTH);
			        length_for_prt += ((temp_ptn->length) * blocks_per_1MB);
			}
		        len = (total_num_of_blocks - 1) - (offset + ptn->start + length_for_prt);
			ASSERT(len >= 0);
		}
		next_ptr_start_adr = ptn->start + len;
		ptable_add(&flash_ptable, ptn->name, offset + ptn->start,
			   len, ptn->flags, TYPE_APPS_PARTITION, PERM_WRITEABLE);
	}

	smem_add_modem_partitions(&flash_ptable);

	ptable_dump(&flash_ptable);
	flash_set_ptable(&flash_ptable);
}

unsigned board_machtype(void)
{
    return LINUX_MACHTYPE;
}

void reboot_device(unsigned reboot_reason)
{
    reboot(reboot_reason);
}

unsigned check_reboot_mode(void)
{
    unsigned mode[2] = {0, 0};
    unsigned int mode_len = sizeof(mode);
    unsigned smem_status;

    smem_status = smem_read_alloc_entry(SMEM_APPS_BOOT_MODE,
					&mode, mode_len );
    if(smem_status)
    {
      dprintf(CRITICAL, "ERROR: unable to read shared memory for reboot mode\n");
      return 0;
    }
    return mode[0];
}

static unsigned target_check_power_on_reason(void)
{
    unsigned power_on_status = 0;
    unsigned int status_len = sizeof(power_on_status);
    unsigned smem_status;

    smem_status = smem_read_alloc_entry(SMEM_POWER_ON_STATUS_INFO,
                                        &power_on_status, status_len);
    if (!smem_status)
    {
        dprintf(CRITICAL, "ERROR: unable to read shared memory for power on reason\n");
    }

    return power_on_status;
}

unsigned target_pause_for_battery_charge(void)
{
    if (target_check_power_on_reason() == PWR_ON_EVENT_USB_CHG)
        return 1;
    return 0;
}

void target_battery_charging_enable(unsigned enable, unsigned disconnect)
{
}
