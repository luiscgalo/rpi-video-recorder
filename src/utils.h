#ifndef UTILS_H
#define UTILS_H

#include <getopt.h>
#include <string.h>

#include <signal.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

//#include <wiringPi.h>
//#include <wiringPiI2C.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
//#include "i2c-dev.h"

#include <sys/ioctl.h>
#include <stdbool.h>

#include "interface/vcos/vcos.h"
#include "bcm_host.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/util/mmal_default_components.h"

#define FIELD_TOP		0xAA
#define FIELD_BOTTOM	0xBB

typedef struct {
	uint8_t* ptrData;
	uint32_t unDataLength;
	uint8_t unField;
} SImageData;

#define u8  uint8_t
#define u16 uint16_t
#define u32 uint32_t

struct cmds_t {
	uint16_t addr;
	uint32_t value;
	int num_bytes;
};

u8 i2c_rd8(int fd, u16 reg);
void i2c_wr8(int fd, u16 reg, u8 val);
void i2c_wr8_and_or(int fd, u16 reg, u8 mask, u8 val);
u16 i2c_rd16(int fd, u16 reg);
void i2c_wr16(int fd, u16 reg, u16 val);
void i2c_wr16_and_or(int fd, u16 reg, u16 mask, u16 val);
u32 i2c_rd32(int fd, u16 reg);
void i2c_wr32(int fd, u16 reg, u32 val);
void write_regs(int fd, struct cmds_t *regs, int count);

void PrintSupportedPortEncodings(MMAL_PORT_T *port);

#endif /* UTILS_H */
