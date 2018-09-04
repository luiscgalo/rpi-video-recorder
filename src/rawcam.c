/*
 * rawcam.c
 *
 *  Created on: 12/07/2018
 *      Author: xubuntu
 */

#include "rawcam.h"
#include "utils.h"
#include "tc358743_regs.h"
#include "converter.h"

// Do the GPIO waggling from here, except that needs root access, plus
// there is variation on pin allocation between the various Pi platforms.
// Provided for reference, but needs tweaking to be useful.
//#define DO_PIN_CONFIG

bool g_bFirstTopFieldFound = false;
uint8_t* g_punBGR24Frame = NULL;
uint32_t g_unBGR24FrameSize = 0;

int i2c_fd = 0;
MMAL_COMPONENT_T *rawcam = NULL;
MMAL_POOL_T *rawcam_pool = NULL;

bool is_hdmi(int sd) {
	return i2c_rd8(sd, SYS_STATUS) & MASK_S_HDMI;
}

bool tx_5v_power_present(int sd) {
	return i2c_rd8(sd, SYS_STATUS) & MASK_S_DDC5V;
}

bool no_signal(int sd) {
	return !(i2c_rd8(sd, SYS_STATUS) & MASK_S_TMDS);
}

bool no_sync(int sd) {
	return !(i2c_rd8(sd, SYS_STATUS) & MASK_S_SYNC);
}

bool audio_present(int sd) {
	return i2c_rd8(sd, AU_STATUS0) & MASK_S_A_SAMPLE;
}

int get_audio_sampling_rate(int sd) {
	static const int code_to_rate[] = { 44100, 0, 48000, 32000, 22050, 384000, 24000, 352800, 88200, 768000, 96000,
			705600, 176400, 0, 192000, 0 };

	/* Register FS_SET is not cleared when the cable is disconnected */
	if (no_signal(sd))
		return 0;

	return code_to_rate[i2c_rd8(sd, FS_SET) & MASK_FS];
}

void start_camera_streaming(int fd) {
	const uint32_t refclk_hz = 27000000;
	const uint32_t sys_freq = refclk_hz / 10000;
	const uint16_t fh_min = refclk_hz / 10000;
	const uint16_t fh_max = (fh_min * 66) / 10;
	const uint32_t lockdet_ref = refclk_hz / 100;

	struct cmds_t cmds[] = { { CONFCTL, 0x0000, 2 }, // Disable video TX Buffer
			// Turn off power and put in reset
			{ SYSCTL, 0x0F00, 2 },    // Assert Reset, [0] = 0: Exit Sleep, wait
			{ 0x0000, 5, 0xFFFF },      // delay
			{ SYSCTL, 0x0000, 2 },    // Release Reset, Exit Sleep

			{ FIFOCTL, 16, 2 },    // FIFO level

			{ VID_PKT_ID1, 0x3435, 2 },    // VID_PKT_ID1 -->  Set packets IDs for interlaced frames

			//{0x0008, 0x005f, 2},    // Audio buffer level -- 96 bytes = 0x5F + 1
			{ INTSTATUS, 0xFFFF, 2 },     // Clear HDMI Rx, CSI Tx and System Interrupt Status
			//{INTMASK, 0x051f, 2},    // Enable HDMI-Rx Interrupt (bit 9), Sys interrupt (bit 5). Disable others. 11-15, 6-7 reserved
			{ INTMASK, 0x0000, 2 }, // do not enable any interrupt

			{ PLLCTL0, 0x8111, 2 },        // PRD[15:12], FBD[8:0]
			{ PLLCTL1, 0x0213, 2 },    // FRS[11:10], LBWS[9:8]= 2, Clock Enable[4] = 1,  ResetB[1] = 1,  PLL En[0]

			// MASK_AUDCHNUM_2 |MASK_AUDOUTSEL_I2S | MASK_AUTOINDEX)
			//{CONFCTL, r0004,  2},    // PwrIso[15], 422 output, send infoframe
			{ CONFCTL, (MASK_AUDCHNUM_2 | MASK_AUDOUTSEL_I2S | MASK_AUTOINDEX), 2 },

			{ CLW_CNTRL, 0x0, 4 },    //Enable CSI-2 Clock lane
			{ D0W_CNTRL, 0x0, 4 },    //Enable CSI-2 Data lane 0
			{ D1W_CNTRL, 0x0, 4 },    //Enable CSI-2 Data lane 1
			{ D2W_CNTRL, MASK_D2W_LANEDISABLE, 4 },    //Disable CSI-2 Data lane 2
			{ D3W_CNTRL, MASK_D3W_LANEDISABLE, 4 },    //Disable CSI-2 Data lane 3

			{ LINEINITCNT, 0x00002988, 4 },   // LP11 = 100 us for D-PHY Rx Init
			{ LPTXTIMECNT, 0x00000005, 4 },   // LP Tx Count[10:0]
			{ TCLK_HEADERCNT, 0x00001d04, 4 },   // TxClk_Zero[15:8]
			{ TCLK_TRAILCNT, 0x00000002, 4 },   // TClk_Trail =
			{ THS_HEADERCNT, 0x00000504, 4 },   // HS_Zero[14:8] =
			{ TWAKEUP, 0x00004882, 4 },   // TWAKEUP Counter[15:0]
			{ TCLK_POSTCNT, 0x0000000A, 4 },   // TxCLk_PostCnt[10:0]
			{ THS_TRAILCNT, 0x00000004, 4 },   // THS_Trail =
			{ HSTXVREGEN, (MASK_CLM_HSTXVREGEN | MASK_D0M_HSTXVREGEN | MASK_D1M_HSTXVREGEN), 4 }, // Enable Voltage Regulator for CSI (2 Data lanes + Clk)

			{ STARTCNTRL, MASK_START, 4 },   // Start PPI
			{ CSI_START, MASK_STRT, 4 },   // Start CSI-2 Tx

			//{CSI_CONFW, r0500, 4},   // SetBit[31:29]
			{ CSI_CONFW, (MASK_MODE_SET | MASK_ADDRESS_CSI_CONTROL |
			MASK_CSI_MODE | MASK_TXHSMD | MASK_NOL_2), 4 },

			/*
			 i2c_wr32(sd, CSI_CONFW, MASK_MODE_SET |
			 MASK_ADDRESS_CSI_ERR_INTENA | MASK_TXBRK | MASK_QUNK |
			 MASK_WCER | MASK_INER);

			 i2c_wr32(sd, CSI_CONFW, MASK_MODE_CLEAR |
			 MASK_ADDRESS_CSI_ERR_HALT | MASK_TXBRK | MASK_QUNK);

			 i2c_wr32(sd, CSI_CONFW, MASK_MODE_SET |
			 MASK_ADDRESS_CSI_INT_ENA | MASK_INTER);
			 */

			//{SYS_INT, 0x01, 1},      // Enable HPD DDC Power Interrupt
			//{SYS_INTM, 0xFE, 1},      // Disable HPD DDC Power Interrupt Mask
			//{0x8513, (uint8_t) ~0x20, 1},    // Receive interrupts for video format change (bit 5)
			//{0x8515, (uint8_t) ~0x02, 1},    // Receive interrupts for format change (bit 1)
			// disable all interrupts
			{ SYS_INTM, 0xFF, 1 }, { CLK_INTM, 0xFF, 1 }, { CBIT_INTM, 0xFF, 1 }, { AUDIO_INTM, 0xFF, 1 }, { MISC_INTM,
					0xFF, 1 },

			{ SYS_FREQ0, (sys_freq & 0x00ff), 1 }, { SYS_FREQ1, (sys_freq & 0xff00) >> 8, 1 },

			{ PHY_CTL0, ((refclk_hz == 42000000) ? MASK_PHY_SYSCLK_IND : 0x0), 1 }, // [1] = 1: RefClk 42 MHz, [0] = 1, DDC5V Auto detection

			{ FH_MIN0, (fh_min & 0x00ff), 1 }, { FH_MIN1, (fh_min & 0xff00) >> 8, 1 },

			{ FH_MAX0, (fh_max & 0x00ff), 1 }, { FH_MAX1, (fh_max & 0xff00) >> 8, 1 },

			{ LOCKDET_REF0, lockdet_ref & 0x0000ff, 1 }, { LOCKDET_REF1, (lockdet_ref & 0x00ff00) >> 8, 1 }, {
			LOCKDET_REF2, (lockdet_ref & 0x0f0000) >> 16, 1 },

			//{NCO_F0_MOD, 0x01, 1},         // SysClk 27MHz
			{ NCO_F0_MOD, 0x00, 1 },         // SysClk 42MHz

			{ PHY_CTL1, SET_PHY_AUTO_RST1_US(1600) | SET_FREQ_RANGE_MODE_CYCLES(1), 1 }, // PHY_AUTO_RST[7:4] = 1600 us, PHY_Range_Mode = 12.5 us
			{ PHY_BIAS, 0x40, 1 },      // [7:4] Ibias: TBD, [3:0] BGR_CNT: Default
			{ PHY_CSQ, SET_CSQ_CNT_LEVEL(0x0a), 1 },      // [3:0] = 0x0a: PHY TMDS CLK line squelch level: 50 uA

			{ DDC_CTL, 0x32, 1 },      // [5:4] = 2'b11: 5V Comp, [1:0] = 10, DDC 5V active detect delay setting: 100 ms
			//{HPD_CTL, 0x10, 1},      // DDC5V detection interlock -- enable
			{ ANA_CTL, MASK_APPL_PCSX_NORMAL | MASK_ANALOG_ON, 1 }, //  [5:4] = 2'b11: Audio PLL charge pump setting to Normal, [0] = 1: DAC/PLL Power On
			{ AVM_CTL, 45, 1 }, // [7:0] = 0x2D: AVMUTE automatic clear setting (when in MUTE and no AVMUTE CMD received) 45 * 100 ms

			// {0x85C7, 0x01, 1},      // [6:4] EDID_SPEED: 100 KHz, [1:0] EDID_MODE: Internal EDID-RAM & DDC2B mode
			// {0x85CB, 0x01, 1},      // EDID Data size read from EEPROM EDID_LEN[10:8] = 0x01, 256-Byte

			/*{PHY_CTL0, 0x01, 1},      // [1] = 1: RefClk 42 MHz, [0] = 1, DDC5V Auto detection
			 {0x8540, 0x0A8C, 2}, // SysClk Freq count with RefClk = 27 MHz (0x1068 for 42 MHz, default)
			 {0x8630, 0x00041eb0, 4},   // Audio FS Lock Detect Control [19:0]: 041EB0 for 27 MHz, 0668A0 for 42 MHz (default)
			 {0x8670, 0x01, 1},         // SysClk 27/42 MHz: 00:= 42 MHz

			 {0x8532, 0x80, 1},      // PHY_AUTO_RST[7:4] = 1600 us, PHY_Range_Mode = 12.5 us
			 {0x8536, 0x40, 1},      // [7:4] Ibias: TBD, [3:0] BGR_CNT: Default
			 {0x853F, 0x0A, 1},      // [3:0] = 0x0a: PHY TMDS CLK line squelch level: 50 uA

			 {0x8543, 0x32, 1},      // [5:4] = 2'b11: 5V Comp, [1:0] = 10, DDC 5V active detect delay setting: 100 ms
			 {0x8544, 0x10, 1},      // DDC5V detection interlock -- enable
			 {0x8545, 0x31, 1},      //  [5:4] = 2'b11: Audio PLL charge pump setting to Normal, [0] = 1: DAC/PLL Power On
			 {0x8546, 0x2D, 1},      // [7:0] = 0x2D: AVMUTE automatic clear setting (when in MUTE and no AVMUTE CMD received) 45 * 100 ms

			 {0x85C7, 0x01, 1},      // [6:4] EDID_SPEED: 100 KHz, [1:0] EDID_MODE: Internal EDID-RAM & DDC2B mode
			 {0x85CB, 0x01, 1},      // EDID Data size read from EEPROM EDID_LEN[10:8] = 0x01, 256-Byte
			 */

			/* HDMI specification requires HPD to be pulsed low for 100ms when EDID changed */
			{ HPD_CTL, 0x01, 1 },      // DDC5V detection interlock -- disable
			{ HPD_CTL, 0x00, 1 },      // DDC5V detection interlock -- pulse low
			{ 0x0000, 100, 0xFFFF },  // sleep
			{ HPD_CTL, 0x10, 1 },      // DDC5V detection interlock -- enable

			//{0x85D1, 0x01, 1},         // Key HDCP loading command (not in functional description)
			{ HDCP_MODE, 0x24, 1 },         // KSV Auto Clear Mode
			//{HDCP_REG1, 0x11, 1},         // EESS_Err auto-unAuth
			//{HDCP_REG2, 0x0F, 1},      // DI_Err (Data Island Error) auto-unAuth

			{ VOUT_SET3, MASK_VOUT_EXTCNT, 1 }, // color settings

			{ VOUT_SET2, MASK_VOUTCOLORMODE_THROUGH, 1 },  // color settings BGR24
			{ VI_REP, MASK_VOUT_COLOR_RGB_FULL, 1 }, // color settings BGR24
			//{ VOUT_SET2, MASK_SEL422 | MASK_VOUT_422FIL_100 | MASK_VOUTCOLORMODE_AUTO, 1 },  // color settings I422
			//{ VI_REP, MASK_VOUT_COLOR_709_YCBCR_LIMITED, 1 }, // color settings I422

			{ FORCE_MUTE, 0x00, 1 },      // Forced Mute Off
			{ AUTO_CMD0, 0xF3, 1 },      // AUTO Mute (AB_sel, PCM/NLPCM_chg, FS_chg, PX_chg, PX_off, DVI)
			{ AUTO_CMD1, MASK_AUTO_MUTE9, 1 }, // AUTO Mute (AVMUTE)
			{ AUTO_CMD2, MASK_AUTO_PLAY3 | MASK_AUTO_PLAY2, 1 }, // AUTO Play (mute-->BufInit-->play)
			{ BUFINIT_START, SET_BUFINIT_START_MS(500), 1 }, // BufInit start time = 0.5sec
			{ FS_MUTE, 0x00, 1 },         // Disable mute

			// {FS_IMODE, 0x00, 1},      // [5] = 0: LPCM/NLPCMinformation extraction from Cbit
			{ FS_IMODE, MASK_NLPCM_SMODE | MASK_FS_SMODE, 1 },

			{ ACR_MODE, MASK_CTS_MODE, 1 },         // CTS adjustment=ON
			{ ACR_MDF0, MASK_ACR_L2MDF_1976_PPM | MASK_ACR_L1MDF_976_PPM, 1 }, // Adjustment level 1=1000ppm, Adjustment level 2=2000ppm
			{ ACR_MDF1, MASK_ACR_L3MDF_3906_PPM, 1 },         // Adjustment level 3=4000ppm
			{ SDO_MODE1, MASK_SDO_FMT_I2S, 1 },      // Audio Data Output Format I2S Format
			{ DIV_MODE, SET_DIV_DLY_MS(100), 1 }, // [7:4] 128 Fs Clock divider  Delay 1 * 0.1 s, [0] = 0: 44.1/48 KHz Auto switch setting

			{ 0x8709, 0x00, 1 },
			// {0x8709, 0xFF, 1},      // ""FF"": Updated secondary Pkts even if there are errors received
			//{0x870B, 0x2C, 1},      // [7:4]: ACP packet Intervals before interrupt, [3:0] AVI packet Intervals, [] * 80 ms
			// {0x870C, 0x53, 1},      // [6:0]: PKT receive interrupt is detected,storage register automatic clear, video signal with RGB and no repeat are set
			// {0x870D, 0x01, 1},      // InFo Pkt: [7]: Correctable Error is included, [6:0] # of Errors before assert interrupt
			// {0x870E, 0x30, 1},      // [7:4]: VS packet Intervals before interrupt, [3:0] SPD packet Intervals, [] * 80 ms
			// {0x9007, 0x10, 1},      // [5:0]  Auto clear by not receiving 16V GBD
			{ INIT_END, MASK_INIT_END, 1 },      // HDMIRx Initialization Completed, THIS MUST BE SET AT THE LAST!

			// Enable audio and video buffers, start streaming
			{ CONFCTL, (MASK_AUDCHNUM_2 | MASK_AUDOUTSEL_I2S | MASK_AUTOINDEX | MASK_ABUFEN | MASK_VBUFEN), 2 }, };
#define NUM_REGS_CMD (sizeof(cmds)/sizeof(cmds[0]))

	write_regs(fd, cmds, NUM_REGS_CMD);
}

void stop_camera_streaming(int fd) {
	struct cmds_t stop_cmds[] = { { HPD_CTL, 0x01, 1 },   // regain manual control
			{ HPD_CTL, 0x00, 1 },   // disable HPD
			//{0x0014, 0x8000, 0x12}, //power island enable (or'ed with register)
			//{0x0000, 10, 0xFFFF}, //Sleep 10ms
			//{0x0002, 0x0001, 2}, // put to sleep
			};
#define NUM_REGS_STOP (sizeof(stop_cmds)/sizeof(stop_cmds[0]))

	write_regs(fd, stop_cmds, NUM_REGS_STOP);
#ifdef DO_PIN_CONFIG
	digitalWrite(41, 0); //Shutdown pin on B+ and Pi2
	digitalWrite(32, 0);//LED pin on B+ and Pi2
#endif
}

void video_field_cb(SImageData sField) {
	uint16_t i, unFieldOffset;
	uint32_t unSourcePos, unDestPos;

	const uint16_t width = 1920;
	const uint16_t height = 1080;

	if (g_punBGR24Frame == NULL) {
		// allocate space for RGB24 buffer
		g_unBGR24FrameSize = VCOS_ALIGN_UP(width, 32) * VCOS_ALIGN_UP(height, 16) * 3;
		g_punBGR24Frame = (uint8_t*) malloc(g_unBGR24FrameSize);
		memset(g_punBGR24Frame, 0, g_unBGR24FrameSize);
	}

	uint8_t* punSourceData = sField.ptrData;
	const uint32_t unWidthStep = width * 3;

	if (sField.unField == FIELD_TOP) {
		//printf("Top field...\n");
		unFieldOffset = 1;
	} else {
		//printf("Bottom field...\n");
		unFieldOffset = 0;
	}

	unDestPos = 0;
	unSourcePos = 0;
	for (i = 0; i < 540; i++) {
		unSourcePos = i * unWidthStep;
		unDestPos = (i * 2 + unFieldOffset) * unWidthStep;
		memcpy(&g_punBGR24Frame[unDestPos], &punSourceData[unSourcePos], unWidthStep);
	}

	if (sField.unField == FIELD_BOTTOM) {
		ConvertFrame(g_punBGR24Frame, g_unBGR24FrameSize);
	}
}

void rawcam_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
	const static uint32_t unPattern = 0xAABBCCDD;

	mmal_buffer_header_mem_lock(buffer);

	// read first 4 bytes of the buffer to check if it was set
	// (check if buffer contains image data)
	uint32_t unBuffInitValue;
	memcpy(&unBuffInitValue, buffer->data, 4);
	//printf(" | Data %x\n", unBuffInitValue);

	if (unBuffInitValue != unPattern) {
		SImageData sField;
		sField.ptrData = buffer->data;
		sField.unDataLength = buffer->length;

		// buffer was set which means that it contains image data
		if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO) {
			//printf("Bottom field (%d bytes)\n", buffer->length);
			sField.unField = FIELD_BOTTOM;
		} else {
			//printf("Top field (%d bytes)\n", buffer->length);
			sField.unField = FIELD_TOP;
			g_bFirstTopFieldFound = true;
		}

		if (g_bFirstTopFieldFound == true) {
			video_field_cb(sField);
			if (sField.unField == FIELD_BOTTOM)
				g_bFirstTopFieldFound = false;
		}
		else
		{
			printf("Dropping valid field as field order wrong\n");
		}
	}

	mmal_buffer_header_mem_unlock(buffer);

	mmal_buffer_header_release(buffer);
	if (port->is_enabled) {
		MMAL_BUFFER_HEADER_T *_buffer = mmal_queue_get(rawcam_pool->queue);

		// set pattern on the next buffer
		memcpy(_buffer->data, &unPattern, 4);
		mmal_port_send_buffer(port, _buffer);
	}
}

void InitRawCam() {
	MMAL_STATUS_T status;
	MMAL_PORT_T *output = NULL;
	unsigned int width, height, fps, frame_interval;
	unsigned int frame_width, frame_height;

	printf("Starting rawcam capture...\n");

	// Setup TC358743 chip
	i2c_fd = open("/dev/i2c-0", O_RDWR);
	if (!i2c_fd) {
		vcos_log_error("Couldn't open I2C device");
		return;
	}
	if (ioctl(i2c_fd, I2C_SLAVE, 0x0F) < 0) {
		vcos_log_error("Failed to set I2C address");
		return;
	}

	// Start TC358743 streaming
	start_camera_streaming(i2c_fd);

	// Give some delay for stabilization
	vcos_sleep(1500);
	/*
	 vcos_log_error("Waiting to detect signal...");
	 int count=0;
	 while((count<20) && (no_sync(i2c_fd) || no_signal(i2c_fd)))
	 {
	 vcos_sleep(200);
	 count++;
	 }
	 vcos_log_error("Signal reported");
	 */

	width = ((i2c_rd8(i2c_fd, DE_WIDTH_H_HI) & 0x1f) << 8) + i2c_rd8(i2c_fd, DE_WIDTH_H_LO);
	height = ((i2c_rd8(i2c_fd, DE_WIDTH_V_HI) & 0x1f) << 8) + i2c_rd8(i2c_fd, DE_WIDTH_V_LO);
	frame_width = ((i2c_rd8(i2c_fd, H_SIZE_HI) & 0x1f) << 8) + i2c_rd8(i2c_fd, H_SIZE_LO);
	frame_height = (((i2c_rd8(i2c_fd, V_SIZE_HI) & 0x3f) << 8) + i2c_rd8(i2c_fd, V_SIZE_LO)) / 2;

	//printf("VPID data = 0x%x\n", i2c_rd16(i2c_fd, VID_PKT_ID1));
	printf("Audio sample rate = %iHz\n", get_audio_sampling_rate(i2c_fd));
	printf("Audio present = %i\n", audio_present(i2c_fd));

	// frame interval in milliseconds * 10
	// Require SYS_FREQ0 and SYS_FREQ1 are precisely set
	frame_interval = ((i2c_rd8(i2c_fd, FV_CNT_HI) & 0x3) << 8) + i2c_rd8(i2c_fd, FV_CNT_LO);
	fps = (frame_interval > 0) ? (10000 / frame_interval) : 0;
	vcos_log_error("Signal is %u x %u, frame interval %u, so %u fps", width, height, frame_interval, fps);
	vcos_log_error("Frame w x h is %u x %u", frame_width, frame_height);

	// Read video input format (progressive or interlaced)
	int ret;
	ret = i2c_rd8(i2c_fd, VI_STATUS1);
	if (ret & MASK_S_V_INTERLACE) {
		printf("Video is interlaced!\n");
	} else {
		printf("Video is progressive!\n");
	}

	// Setup RPi rawcam component
	status = mmal_component_create("vc.ril.rawcam", &rawcam);
	if (status != MMAL_SUCCESS) {
		vcos_log_error("Failed to create rawcam");
		return;
	}

	output = rawcam->output[0];

	MMAL_PARAMETER_CAMERA_RX_CONFIG_T rx_cfg;
	memset(&rx_cfg, 0, sizeof(MMAL_PARAMETER_CAMERA_RX_CONFIG_T));
	rx_cfg.hdr.id = MMAL_PARAMETER_CAMERA_RX_CONFIG;
	rx_cfg.hdr.size = sizeof(MMAL_PARAMETER_CAMERA_RX_CONFIG_T);

	// setup CSI config on rawcam
	status = mmal_port_parameter_get(output, &rx_cfg.hdr);
	if (status != MMAL_SUCCESS) {
		vcos_log_error("Failed to get cfg");
	}

	rx_cfg.image_id = 0x35;
	rx_cfg.data_lanes = 2;
	rx_cfg.unpack = MMAL_CAMERA_RX_CONFIG_UNPACK_NONE;
	rx_cfg.pack = MMAL_CAMERA_RX_CONFIG_PACK_NONE;
	rx_cfg.embedded_data_lines = 128;
	status = mmal_port_parameter_set(output, &rx_cfg.hdr);
	if (status != MMAL_SUCCESS) {
		vcos_log_error("Failed to set cfg");
	}

	vcos_log_error("Enable rawcam....");
	status = mmal_component_enable(rawcam);
	if (status != MMAL_SUCCESS) {
		vcos_log_error("Failed to enable");
	}
	status = mmal_port_parameter_set_boolean(output, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
	if (status != MMAL_SUCCESS) {
		vcos_log_error("Failed to set zero copy");
	}
	printf("Enable raw camera success!\n");

	// Set format
	output->format->es->video.crop.width = width;
	output->format->es->video.crop.height = height;
	output->format->es->video.width = VCOS_ALIGN_UP(width, 32);
	output->format->es->video.height = VCOS_ALIGN_UP(height, 16);
	output->format->encoding = MMAL_ENCODING_BGR24;

	status = mmal_port_format_commit(output);
	if (status != MMAL_SUCCESS) {
		vcos_log_error("Failed port_format_commit");
	}

	// set buffer settings
	output->buffer_size = output->buffer_size_recommended;
	output->buffer_num = output->buffer_num_recommended;

	vcos_log_error("Create pool of %d buffers of size %d", output->buffer_num, output->buffer_size);
	rawcam_pool = mmal_port_pool_create(output, output->buffer_num, output->buffer_size);
	if (rawcam_pool == NULL) {
		vcos_log_error("Failed to create pool");
	}

	status = mmal_port_enable(output, rawcam_callback);
	if (status != MMAL_SUCCESS) {
		vcos_log_error("Failed to enable port");
		exit(1);
	}

	// send output buffers
	for (uint8_t i = 0; i < output->buffer_num; i++) {
		MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(rawcam_pool->queue);

		if (!buffer) {
			printf("Buffer is NULL!\n");
			exit(1);
		}
		status = mmal_port_send_buffer(output, buffer);
		if (status != MMAL_SUCCESS) {
			printf("mmal_port_send_buffer failed on buffer %p, status %d\n", buffer, status);
			exit(1);
		}
		printf("Sent buffer %p\n", buffer);
	}
}

void StopRawCam() {
	MMAL_STATUS_T status;

	printf("Stopping rawcam capture...\n");
	stop_camera_streaming(i2c_fd);

	status = mmal_component_disable(rawcam);
	if (status != MMAL_SUCCESS) {
		vcos_log_error("Failed to disable rawcam");
	}

	mmal_component_destroy(rawcam);

	close(i2c_fd);
}

