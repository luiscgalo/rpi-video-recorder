#include "utils.h"

#define I2C_ADDR 0x0F

void i2c_rd(int fd, uint16_t reg, uint8_t *values, uint32_t n) {
	int err;
	uint8_t buf[2] = { reg >> 8, reg & 0xff };
	struct i2c_rdwr_ioctl_data msgset;
	struct i2c_msg msgs[2] = { { .addr = I2C_ADDR, .flags = 0, .len = 2, .buf = buf, }, { .addr = I2C_ADDR, .flags =
	I2C_M_RD, .len = n, .buf = values, }, };

	msgset.msgs = msgs;
	msgset.nmsgs = 2;

	err = ioctl(fd, I2C_RDWR, &msgset);
	if (err != msgset.nmsgs)
		vcos_log_error("%s: reading register 0x%x from 0x%x failed, err %d\n", __func__, reg, I2C_ADDR, err);
}

void i2c_wr(int fd, uint16_t reg, uint8_t *values, uint32_t n) {
	uint8_t data[1024];
	int err, i;
	struct i2c_msg msg;
	struct i2c_rdwr_ioctl_data msgset;

	if ((2 + n) > sizeof(data))
		vcos_log_error("i2c wr reg=%04x: len=%d is too big!\n", reg, 2 + n);

	msg.addr = I2C_ADDR;
	msg.buf = data;
	msg.len = 2 + n;
	msg.flags = 0;

	data[0] = reg >> 8;
	data[1] = reg & 0xff;

	for (i = 0; i < n; i++)
		data[2 + i] = values[i];

	msgset.msgs = &msg;
	msgset.nmsgs = 1;

	err = ioctl(fd, I2C_RDWR, &msgset);
	if (err != 1) {
		vcos_log_error("%s: writing register 0x%x from 0x%x failed\n", __func__, reg, I2C_ADDR);
		return;
	}
}

u8 i2c_rd8(int fd, u16 reg) {
	u8 val;

	i2c_rd(fd, reg, &val, 1);

	return val;
}

void i2c_wr8(int fd, u16 reg, u8 val) {
	i2c_wr(fd, reg, &val, 1);
}

void i2c_wr8_and_or(int fd, u16 reg, u8 mask, u8 val) {
	i2c_wr8(fd, reg, (i2c_rd8(fd, reg) & mask) | val);
}

u16 i2c_rd16(int fd, u16 reg) {
	u16 val;

	i2c_rd(fd, reg, (u8 *) &val, 2);

	return val;
}

void i2c_wr16(int fd, u16 reg, u16 val) {
	i2c_wr(fd, reg, (u8 *) &val, 2);
}

void i2c_wr16_and_or(int fd, u16 reg, u16 mask, u16 val) {
	i2c_wr16(fd, reg, (i2c_rd16(fd, reg) & mask) | val);
}

u32 i2c_rd32(int fd, u16 reg) {
	u32 val;

	i2c_rd(fd, reg, (u8 *) &val, 4);

	return val;
}

void i2c_wr32(int fd, u16 reg, u32 val) {
	i2c_wr(fd, reg, (u8 *) &val, 4);
}

void write_regs(int fd, struct cmds_t *regs, int count) {
	int i;
	for (i = 0; i < count; i++) {
		switch (regs[i].num_bytes) {
		case 1:
			i2c_wr8(fd, regs[i].addr, (uint8_t) regs[i].value);
			break;
		case 2:
			i2c_wr16(fd, regs[i].addr, (uint16_t) regs[i].value);
			break;
		case 4:
			i2c_wr32(fd, regs[i].addr, regs[i].value);
			break;
		case 0x11:
			i2c_wr8_and_or(fd, regs[i].addr, 0xFF, (uint8_t) regs[i].value);
			break;
		case 0x12:
			i2c_wr16_and_or(fd, regs[i].addr, 0xFFFF, (uint16_t) regs[i].value);
			break;
		case 0xFFFF:
			vcos_sleep(regs[i].value);
			break;
		default:
			vcos_log_error("%u bytes specified in entry %d - not supported", regs[i].num_bytes, i);
			break;
		}
	}
}

