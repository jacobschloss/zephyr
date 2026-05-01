/*
 * Copyright (c) 2026 Jacob Schloss
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT microchip_rng90

#include <zephyr/drivers/entropy.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(entropy_rng90, CONFIG_ENTROPY_LOG_LEVEL);

struct entropy_rng90_config {
	struct i2c_dt_spec bus;
};

enum rng90_addr {
	RNG90_RST = 0x00,
	RNG90_SLP = 0x01,
	RNG90_CMD = 0x03
};

enum rng90_cmd {
	RNG90_CMD_INFO = 0x30,
	RNG90_CMD_RAND = 0x16,
	RNG90_CMD_READ = 0x02,
	RNG90_CMD_TEST = 0x77
};

enum rng90_code {
	RNG90_SUCCESS          = 0x00,
	RNG90_PARSE_ERROR      = 0x03,
	RNG90_SELFTEST_ERROR   = 0x07,
	RNG90_HEALTHTEST_ERROR = 0x08,
	RNG90_EXEC_ERROR       = 0x0F,
	RNG90_WAKE             = 0x11,
	RNG90_BAD_CRC          = 0xFF
};

enum rng90_selftest_code {
	RNG90_SELFTEST_OK = 0x00,
	RNG90_SELFTEST_FAIL_DRBG = 0x01,
	RNG90_SELFTEST_FAIL_SHA  = 0x20,
	RNG90_SELFTEST_FAIL_BOTH = 0x21,
	RNG90_SELFTEST_NR_DRBG   = 0x02,
	RNG90_SELFTEST_NR_SHA    = 0x10,
	RNG90_SELFTEST_NR_BOTH   = 0x12
};

#define TYP_TIME_PU_US            1000
#define MAX_TIME_PU_US            1800

#define TYP_TIME_INFO_US            280
#define TYP_TIME_RAND_1ST_US        57000
#define TYP_TIME_RAND_2ND_US        20200
#define TYP_TIME_READ_US            400
#define TYP_TIME_SELFTEST_DRBG_US   25300
#define TYP_TIME_SELFTEST_SHA_US    11400
#define TYP_TIME_SELFTEST_STATUS_US 270

#define MAX_TIME_INFO_US            400
#define MAX_TIME_RAND_1ST_US        72000
#define MAX_TIME_RAND_2ND_US        25300
#define MAX_TIME_READ_US            600
#define MAX_TIME_SELFTEST_DRBG_US   31800
#define MAX_TIME_SELFTEST_SHA_US    14500
#define MAX_TIME_SELFTEST_STATUS_US 400

#define PARAM_SELFTEST_READ     0x00
#define PARAM_SELFTEST_DRBG     0x01
#define PARAM_SELFTEST_SHA      0x20
#define PARAM_SELFTEST_DRBG_SHA 0x21

#define RNG90_CRC16_POLYNOMIAL    0x8005
#define RNG90_CRC16_INITIAL_VALUE 0x0000

// HOST TX packet
// I2C address
// Word Address
// Command packet [count, cmd, crc]

// HOST RX packet
// I2C address
// Resp packet [count, status, crc]

// Return negative error code or length of data written to resp
static int entropy_rng90_exec_cmd(const struct device *dev, 
	uint8_t *cmd, uint16_t cmdlen,
	uint8_t *resp, uint16_t resplen,
	int typ_us, int max_us
	)
{
	const struct entropy_rng90_config *cfg = dev->config;
	uint32_t start_cycles;
	uint32_t max_cycle_cnt;

	int ret = i2c_write_dt(&cfg->bus, cmd, cmdlen);
	if(ret)
	{
		return ret;
	}

	start_cycles  = k_cycle_get_32();
	max_cycle_cnt = max_us * sys_clock_hw_cycles_per_sec() / USEC_PER_SEC;

	// Wait the typical amount for command completion
	k_sleep(K_USEC(typ_us));

	// Packets have two forms
	// <len=1> <status> <crcl> <crch>
	// <len=n> <n bytes> <crcl> <crch>, where n is 4, 16, or 32
	// We are permitted to split fifo read into multiple read transactions
	// We have option to reset fifo to start to re-read by sending RNG90_RST
	if(resplen)
	{
		// First read length while polling for command completion
		do 
		{
			ret = i2c_read_dt(&cfg->bus, resp, 1);
			if(ret)
			{
				k_sleep(K_MSEC(1));
			}
		} while(ret && ((k_cycle_get_32() - start_cycles) > max_cycle_cnt));
		if(ret)
		{
			return ret;
		}

		// Either we have not enough space, or length is corrupt
		// Reject it - we could reset and retry read
		if( (resp[0]+2) > resplen )
		{
			return -EIO;
		}

		// Read rest of packet and crc
		ret = i2c_read_dt(&cfg->bus, resp, resp[0]+2);
		if(ret)
		{
			return ret;
		}
		
		// Verify CRC
		crc = crc16(RNG90_CRC16_POLYNOMIAL, RNG90_CRC16_INITIAL_VALUE, resp+resp[0]+1, 2);
		if(crc != sys_get_le16(&resp_buf[2]))
		{
			return -EIO;
		}

		ret = resp[0] + 3;
	}

	return ret;
}

static int entropy_rng90_cmd_wake(const struct device *dev)
{
	const struct entropy_rng90_config *cfg = dev->config;
	uint16_t crc;
	int ret;

	uint8_t cmd_buf[1] = {RNG90_RST};
	uint8_t resp_buf[4];

	ret = entropy_rng90_exec_cmd(dev,
		cmd_buf, sizeof(cmd_buf),
		resp_buf, sizeof(resp_buf),
		TYP_TIME_PU_US, MAX_TIME_PU_US
	);

	return ret;
}

static int entropy_rng90_cmd_sleep(const struct device *dev)
{
	const struct entropy_rng90_config *cfg = dev->config;

	uint8_t cmd_buf[1] = {RNG90_SLP};
	int ret = i2c_write_dt(&cfg->bus, cmd_buf, sizeof(cmd_buf));
	if(ret)
	{
		LOG_ERR("Sleep failed");
	}

	return ret;
}

static int entropy_rng90_cmd_read(const struct device *dev, uint8_t *buffer, uint16_t length)
{
	
}

static int entropy_rng90_cmd_info(const struct device *dev, uint8_t *buffer, uint16_t length)
{
	
}

static int entropy_rng90_cmd_self_test(const struct device *dev)
{
	uint8_t cmd_buf[8] = {RNG90_CMD, 4, RNG90_CMD_TEST, PARAM_SELFTEST_DRBG_SHA, 0, 0, 0, 0};
	uint8_t resp_buf[4]; // [count, status, crcl, crch]
	uint16_t crc;

	crc = crc16(RNG90_CRC16_POLYNOMIAL, RNG90_CRC16_INITIAL_VALUE, cmd_buf, 6);
	sys_put_le16(crc, &cmd_buf[6]);

	int ret = entropy_rng90_exec_cmd(dev,
		cmd_buf, sizeof(cmd_buf),
		resp_buf, sizeof(resp_buf),
		TYP_TIME_SELFTEST_DRBG_US + TYP_TIME_SELFTEST_SHA_US,
		MAX_TIME_SELFTEST_DRBG_US + MAX_TIME_SELFTEST_SHA_US
		);

	if(ret)
	{
		return -EIO;
	}

	if(resp_buf[0] != 1)
	{
		return -EIO;
	}

	crc = crc16(RNG90_CRC16_POLYNOMIAL, RNG90_CRC16_INITIAL_VALUE, resp_buf, 2);
	if(crc != sys_get_le16(&resp_buf[2]))
	{
		return -EIO;
	}

	// First check for error code
	switch(resp_buf[1])
	{
		case RNG90_PARSE_ERROR:
		case RNG90_SELFTEST_ERROR:
		case RNG90_HEALTHTEST_ERROR:
		case RNG90_EXEC_ERROR:
		case RNG90_WAKE:
		case RNG90_BAD_CRC:
		{
			ret = -EIO;
			break;
		}
	}

	// If not an error, check against SELFTEST codes
	switch(resp_buf[1])
	{
		case RNG90_SELFTEST_OK:
		{
			ret = 0;
			break;		
		}
		case RNG90_SELFTEST_FAIL_DRBG:
		case RNG90_SELFTEST_FAIL_SHA:
		case RNG90_SELFTEST_FAIL_BOTH:
		{
			break;
		}
		case RNG90_SELFTEST_NR_DRBG:
		case RNG90_SELFTEST_NR_SHA:
		case RNG90_SELFTEST_NR_BOTH:
		{
			LOG_ERR("Self test not run when requested");
			ret = -EINVAL;
			break;
		}
		default:
		{
			LOG_ERR("Self test returned invalid status");
			ret = -EINVAL;
			break;
		}
	}

	return ret;
}

static int entropy_rng90_cmd_rand(const struct device *dev, uint8_t rand[32])
{
	uint8_t cmd_buf[28];  // [word, count, OpCode, 0, 0, 0, b[20], crcl, crch]
	uint8_t resp_buf[35]; // [count, data[32], crcl, crch]
	uint16_t crc;

	memset(cmd_buf, 0, sizeof(cmd_buf));
	cmd_buf[0] = RNG90_CMD;
	cmd_buf[1] = 24;
	cmd_buf[2] = RNG90_CMD_RAND;

	crc = crc16(RNG90_CRC16_POLYNOMIAL, RNG90_CRC16_INITIAL_VALUE, cmd_buf+1, 25);
	sys_put_le16(crc, &cmd_buf[6]);

	int ret = entropy_rng90_exec_cmd(dev,
		cmd_buf, sizeof(cmd_buf),
		resp_buf, sizeof(resp_buf),
		TYP_TIME_RAND_2ND_US,
		MAX_TIME_RAND_1ST_US
	);

	if(ret)
	{
		return -EIO;
	}

	crc = crc16(RNG90_CRC16_POLYNOMIAL, RNG90_CRC16_INITIAL_VALUE, resp_buf, 33);
	if(crc != sys_get_le16(&resp_buf[33]))
	{
		return -EIO;
	}

	memcpy(rand, resp_buf + 1, 32);
	return 0;
}

static int entropy_rng90_get_entropy(const struct device *dev, uint8_t *buffer, uint16_t length)
{
	int ret;
	uint8_t buf[32];
	uint16_t num_written = 0;

	for(size_t i = 0; i < length; i+=32)
	{
		ret = entropy_rng90_cmd_rand(dev, buf);
		if(ret)
		{
			return ret;
		}

		if((num_written + 32) > length)
		{
			memcpy(&buffer[i], buf, length - num_written);
			num_written = length;
		}
		else
		{
			memcpy(&buffer[i], buf, 32);
			num_written += 32;
		}
	}

	return ret;
}

static int entropy_rng90_init(const struct device *dev)
{
	const struct entropy_rng90_config *cfg = dev->config;

	if (!device_is_ready(cfg->bus.bus)) {
		LOG_ERR("Device not ready.");
		return -ENODEV;
	}

	return 0;
}

static DEVICE_API(entropy, entropy_rng90_api_funcs) = {
	.get_entropy = entropy_rng90_get_entropy
};

#define RNG90_INIT(n) =                                                   \
	static const struct entropy_rng90_config entropy_rng90_config_##n = { \
		.bus = I2C_DT_SPEC_INST_GET(n)                                    \
	};                                                                    \
	DEVICE_DT_INST_DEFINE(n,                                              \
		    entropy_rng90_init, NULL, NULL, entropy_rng90_config_##n,     \
		    POST_KERNEL, CONFIG_ENTROPY_INIT_PRIORITY,                    \
		    &entropy_rng90_api_funcs);

DT_INST_FOREACH_STATUS_OKAY(RNG90_INIT);
