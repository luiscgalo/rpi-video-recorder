#include "utils.h"
#include "converter.h"
#include "rawcam.h"

uint8_t called_quit = 0;

/* Clean up after Ctrl+c */
void signal_callback_handler(int signum) {
	printf("\n\nCaught signal %d!\nCleanly exiting...\n", signum);
	called_quit = 1;
}

int main() {
	bcm_host_init();
	vcos_log_register("RaspiRaw", VCOS_LOG_CATEGORY);
	signal(SIGINT, signal_callback_handler);

	InitConverter();
	InitRawCam();

	printf("Working until ctrl+c action...\n");
	while (called_quit != 1) {
		sleep(1);
	}

	CloseConverter();
	StopRawCam();

	printf("Done!\n");
	return 0;
}
