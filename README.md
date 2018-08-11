# Raspberry Pi Video Recorder
*TC358743 (Auvidea B101 HDMI to CSI-2 board) interlaced video recorder application for the Raspberry Pi*

This prototype application intents to capture 1080i50 video, recording it as progressive in a elementary H264 video file (1080p25 or 1080p50)

The data processing chain is as follows
	HDMI (1080i50) -> TC358743 -> Rawcam (odd/even BGR24 fields) -> ISP (BGR24 to I420) -> ImageFx (deinterlacer) -> H264 video encoder
	
In order to test this application you should compile the project as a standard CMake project

	1. Checkout the source code and navigate to the "src" directory;
	2. Create a new directory ("build" as example), navigate into it and run the following commands;
			cmake ..
			make
	3. Done and you can try to run the application! :)
	
*NOTE*: This is a work in progress application and there are some known issues with it (choppy movements from the deinterlacer and sporadic crashes)
      Feel free to use it and to suggest improvements.
      
      
*NOTE2*: Due to the amount of data to be processed on the Raspberry Pi (FullHD video in realtime!) there is the need of overclocking it.
       My current overclock settings are:
   			arm_freq=1400
			gpu_mem=200
			over_voltage=7
			core_freq=550
			sdram_freq=550
			sdram_schmoo=0x02000020
			force_turbo=1
			boot_delay=1
