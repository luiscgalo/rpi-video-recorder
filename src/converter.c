/*
 * converter.cpp
 *
 *  Created on: 05/07/2018
 *      Author: xubuntu
 */

#include "converter.h"

#include <stdio.h>
#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/util/mmal_default_components.h"

/**
 * Flow of image processing
 * 	  RawFrame -> ISP -> ImageFx -> Encoder
 *
 */

// ISP variables (converter from BGR24 to I420)
MMAL_COMPONENT_T *pISP = NULL;
MMAL_PORT_T *isp_input = NULL;
MMAL_PORT_T *isp_output = NULL;
MMAL_POOL_T *isp_pool_in = NULL;

// Image Fx variables (deinterlacer)
MMAL_COMPONENT_T *pImageFx = NULL;
MMAL_PORT_T *deint_input = NULL;
MMAL_PORT_T *deint_output = NULL;

/*
 // H264 Encoder variables
 MMAL_COMPONENT_T *pEncoder = NULL;
 MMAL_PORT_T *enc_input = NULL;
 MMAL_PORT_T *enc_output = NULL;
 MMAL_POOL_T *enc_pool_out = NULL;

 // Connections between components
 MMAL_CONNECTION_T* conn_isp_deint = NULL;
 MMAL_CONNECTION_T* conn_deint_enc = NULL;
 */

MMAL_CONNECTION_T* conn_isp_deint = NULL;
MMAL_POOL_T *enc_pool_out = NULL;

// hard-coded constants for test purposes only
#define VIDEO_WIDTH 	1920
#define VIDEO_HEIGHT	1080

int64_t pts = 0;
FILE * pFile2 = NULL;

void ConvertFrame(uint8_t* punBuffer, const uint32_t unBufferSize) {
	MMAL_BUFFER_HEADER_T *buffer;

	// send data to the input port of ISP component
	buffer = mmal_queue_get(isp_pool_in->queue);
	if (buffer != NULL) {
		mmal_buffer_header_mem_lock(buffer);
		buffer->flags = MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED | MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST;
		buffer->length = unBufferSize;

		buffer->pts = buffer->dts = MMAL_TIME_UNKNOWN;
		//buffer->pts = buffer->dts = pts;
		//pts += 1000000 / 25;

		memcpy(buffer->data, punBuffer, unBufferSize);
		mmal_buffer_header_mem_unlock(buffer);

		//printf("BGR24 frame -> ISP input (%d bytes)\n", buffer->length);
		if (mmal_port_send_buffer(isp_input, buffer) != MMAL_SUCCESS) {
			printf("Error sending data to ISP input!\n");
		}
	} else {
		printf("\t\tISP input buffer not available!\n");
	}
}

void isp_input_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
	mmal_buffer_header_release(buffer);
}

void enc_output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
	printf("TEST!!! - Image_fx output data Size=%i\n", buffer->length);

	// release buffer back to the pool
	mmal_buffer_header_release(buffer);

	// and send one back to the port (if still open)
	if (port->is_enabled) {
		MMAL_STATUS_T status;
		MMAL_BUFFER_HEADER_T* new_buffer = mmal_queue_get(enc_pool_out->queue);

		if (new_buffer)
			status = mmal_port_send_buffer(port, new_buffer);

		if (!new_buffer || status != MMAL_SUCCESS)
			printf("Unable to return a buffer to the encoder port!\n");
	}

#if 0

	printf("Encoder out data (%d bytes)!\n", buffer->length);

	if (pFile2 == NULL) {
		pFile2 = fopen("capture.h264", "wb");
	}

	// These are the header bytes, save them for final output
	mmal_buffer_header_mem_lock(buffer);
	fwrite(buffer->data, 1, buffer->length, pFile2);
	mmal_buffer_header_mem_unlock(buffer);

	// release buffer back to the pool
	mmal_buffer_header_release(buffer);

	// and send one back to the port (if still open)
	if (port->is_enabled) {
		MMAL_STATUS_T status;
		MMAL_BUFFER_HEADER_T* new_buffer = mmal_queue_get(enc_pool_out->queue);

		if (new_buffer)
		status = mmal_port_send_buffer(port, new_buffer);

		if (!new_buffer || status != MMAL_SUCCESS)
		printf("Unable to return a buffer to the encoder port!\n");
	}
#endif
}

void InitConverter() {
	MMAL_STATUS_T status;

	// ###################################### ISP converter ##################################################

	status = mmal_component_create("vc.ril.isp", &pISP);
	if (status != MMAL_SUCCESS) {
		printf("Failed to create ISP!\n");
		return;
	}

	isp_input = pISP->input[0];
	isp_output = pISP->output[0];

	/*printf("------ ISP supported output formats -----\n");
	 PrintSupportedPortEncodings(isp_output);*/

	isp_input->format->type = MMAL_ES_TYPE_VIDEO;
	isp_input->format->encoding = MMAL_ENCODING_BGR24;
	isp_input->format->es->video.width = VCOS_ALIGN_UP(VIDEO_WIDTH, 32);
	isp_input->format->es->video.height = VCOS_ALIGN_UP(VIDEO_HEIGHT, 16);
	isp_input->format->es->video.crop.x = 0;
	isp_input->format->es->video.crop.y = 0;
	isp_input->format->es->video.crop.width = VIDEO_WIDTH;
	isp_input->format->es->video.crop.height = VIDEO_HEIGHT;
	isp_input->format->es->video.frame_rate.num = 0;
	isp_input->format->es->video.frame_rate.den = 1;
	status = mmal_port_format_commit(isp_input);
	if (status != MMAL_SUCCESS) {
		printf("Failed to commit converter input format!\n");
		return;
	}

	isp_input->buffer_size = isp_input->buffer_size_recommended;
	isp_input->buffer_num = 6; // isp_input->buffer_num_recommended;

	// create pool for input data
	isp_pool_in = mmal_port_pool_create(isp_input, isp_input->buffer_num, isp_input->buffer_size);
	if (isp_pool_in == NULL) {
		printf("Failed to create ISP input pool!\n");
	}

	// Setup ISP output (copy of input format, changing only the encoding)
	mmal_format_copy(isp_output->format, isp_input->format);
	isp_output->format->encoding = MMAL_ENCODING_I420;
	//isp_output->format->encoding = MMAL_ENCODING_YUVUV128; // SAND format
	status = mmal_port_format_commit(isp_output);
	if (status != MMAL_SUCCESS) {
		printf("Failed to commit converter output format!\n");
		return;
	}

	isp_output->buffer_size = isp_output->buffer_size_recommended;
	isp_output->buffer_num = 6; //isp_output->buffer_num_recommended;

	// Enable ports and ISP component
	status = mmal_port_enable(isp_input, isp_input_cb);
	if (status != MMAL_SUCCESS) {
		printf("Error enabling ISP input port!\n");
		return;
	}

	// ################################## deinterlacer ######################################################

	status = mmal_component_create("vc.ril.image_fx", &pImageFx);
	if (status != MMAL_SUCCESS) {
		printf("Failed to create image_fx!\n");
		return;
	}

	deint_input = pImageFx->input[0];
	deint_output = pImageFx->output[0];

	/*printf("------ image_fx supported input formats -----\n");
	 PrintSupportedPortEncodings(deint_input);
	 printf("------ image_fx supported output formats -----\n");
	 PrintSupportedPortEncodings(deint_output);*/

	MMAL_PARAMETER_IMAGEFX_PARAMETERS_T img_fx_param;
	memset(&img_fx_param, 0, sizeof(MMAL_PARAMETER_IMAGEFX_PARAMETERS_T));
	img_fx_param.hdr.id = MMAL_PARAMETER_IMAGE_EFFECT_PARAMETERS;
	img_fx_param.hdr.size = sizeof(MMAL_PARAMETER_IMAGEFX_PARAMETERS_T);

	// Advanced deinterlacer using QPUs - !!! NOT WORKING !!!
	img_fx_param.effect = MMAL_PARAM_IMAGEFX_DEINTERLACE_ADV;
	img_fx_param.num_effect_params = 4;
	img_fx_param.effect_parameter[0] = 3; // interlaced input frame with both fields / top field first
	img_fx_param.effect_parameter[1] = 0; // frame period (1000000 * 1 / 25);
	img_fx_param.effect_parameter[2] = 0; // half framerate ?
	img_fx_param.effect_parameter[3] = 1; // use QPU ?

	// Fast deinterlacer (50fps) - Working with some glitches on the output video
	/*img_fx_param.effect = MMAL_PARAM_IMAGEFX_DEINTERLACE_FAST;
	 img_fx_param.num_effect_params = 3;
	 img_fx_param.effect_parameter[0] = 3; // interlaced input frame with both fields / top field first
	 img_fx_param.effect_parameter[1] = 0; // frame period (1000000 * 1 / 25);
	 img_fx_param.effect_parameter[2] = 0; // half framerate ?*/


	 // Fast deinterlacer with half frame rate (25fps) - Working OK
	 /*img_fx_param.effect = MMAL_PARAM_IMAGEFX_DEINTERLACE_FAST;
	 img_fx_param.num_effect_params = 3;
	 img_fx_param.effect_parameter[0] = 3; // interlaced input frame with both fields / top field first
	 img_fx_param.effect_parameter[1] = 0; // frame period
	 img_fx_param.effect_parameter[2] = 1; // half framerate ?*/


	if (mmal_port_parameter_set(deint_output, &img_fx_param.hdr) != MMAL_SUCCESS) {
		printf("Failed to configure deinterlacer output port (mode)\n");
		return;
	}

	// Image_fx assumed 3 frames of context. simple deinterlace doesn't require this
	if (mmal_port_parameter_set_uint32(deint_input, MMAL_PARAMETER_EXTRA_BUFFERS, 2) != MMAL_SUCCESS) {
		printf("Failed to configure deinterlacer extra buffers!\n");
		return;
	}

	printf("Create connection ISP output to image_fx input...\n");
	status = mmal_connection_create(&conn_isp_deint, isp_output, deint_input,
	MMAL_CONNECTION_FLAG_TUNNELLING);
	if (status != MMAL_SUCCESS) {
		printf("Failed to create connection status %d: ISP->image_fx\n", status);
		return;
	}

	status = mmal_connection_enable(conn_isp_deint);
	if (status != MMAL_SUCCESS) {
		printf("Failed to enable connection ISP->image_fx\n");
		return;
	}

	// Setup image_fx input format (equal to ISP output)
	//mmal_format_copy(deint_input->format, isp_output->format);
	/*status = mmal_port_format_commit(deint_input);
	 if (status != MMAL_SUCCESS) {
	 printf("Failed to commit image_fx input format!\n");
	 return;
	 }*/

	deint_input->buffer_size = deint_input->buffer_size_recommended;
	deint_input->buffer_num = 8; //deint_input->buffer_num_recommended;

	// Setup image_fx output format (equal to input format)
	//mmal_format_copy(deint_output->format, deint_input->format);

	deint_output->format->es->video.crop.width = VIDEO_WIDTH;
	deint_output->format->es->video.crop.height = VIDEO_HEIGHT;
	deint_output->format->es->video.width = VCOS_ALIGN_UP(VIDEO_WIDTH, 32);
	deint_output->format->es->video.height = VCOS_ALIGN_UP(VIDEO_HEIGHT, 16);
	deint_output->format->encoding = MMAL_ENCODING_I420;
	status = mmal_port_format_commit(deint_output);
	if (status != MMAL_SUCCESS) {
		printf("Failed to commit image_fx output format!\n");
		return;
	}

	deint_output->buffer_size = deint_output->buffer_size_recommended;
	deint_output->buffer_num = 8; // deint_output->buffer_num_recommended;

	enc_pool_out = mmal_port_pool_create(deint_output, deint_output->buffer_num, deint_output->buffer_size);
	if (enc_pool_out == NULL) {
		printf("Failed to create encoder output pool!\n");
	}

	// Enable output port
	status = mmal_port_enable(deint_output, enc_output_callback);
	if (status != MMAL_SUCCESS) {
		printf("Error enabling deinterlacer output port!\n");
		return;
	}

	status = mmal_component_enable(pISP);
	if (status != MMAL_SUCCESS) {
		printf("Error enabling ISP!\n");
		return;
	}

	status = mmal_component_enable(pImageFx);
	if (status != MMAL_SUCCESS) {
		printf("Error enabling deinterlacer!\n");
		return;
	}

	// Send buffers for output pool
	for (uint8_t i = 0; i < deint_output->buffer_num; i++) {
		MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(enc_pool_out->queue);

		if (!buffer) {
			printf("Buffer is NULL!\n");
			exit(1);
		}
		status = mmal_port_send_buffer(deint_output, buffer);
		if (status != MMAL_SUCCESS) {
			printf("mmal_port_send_buffer failed deint on buffer %p, status %d\n", buffer, status);
			exit(1);
		}
	}

#if 0
	// ################################## encoder ######################################################

	// create H264 video encoder component
	status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &pEncoder);
	if (status != MMAL_SUCCESS) {
		printf("Unable to create video encoder component!\n");
		return;
	}

	enc_input = pEncoder->input[0];
	enc_output = pEncoder->output[0];

	printf("------ H264 encoder supported input formats -----\n");
	PrintSupportedPortEncodings(enc_input);

	enc_input->format->es->video.frame_rate.num = 0;
	enc_input->format->es->video.frame_rate.den = 1;
	enc_input->buffer_size = enc_input->buffer_size_recommended;
	enc_input->buffer_num = 6; //enc_input->buffer_num_recommended;

	printf("Create connection ISP output to image_fx input...\n");
	status = mmal_connection_create(&conn_deint_enc, deint_output, enc_input, MMAL_CONNECTION_FLAG_TUNNELLING);
	if (status != MMAL_SUCCESS) {
		printf("Failed to create connection status %d: Deint->encoder\n", status);
		return;
	}

	mmal_format_copy(enc_output->format, enc_input->format);
	enc_output->format->encoding = MMAL_ENCODING_H264;
	enc_output->format->bitrate = 1000 * 1000 * 4; // 4 Mbps output video
	enc_output->format->es->video.frame_rate.num = 0;
	enc_output->format->es->video.frame_rate.den = 1;
	status = mmal_port_format_commit(enc_output);
	if (status != MMAL_SUCCESS) {
		printf("Video encoder output format couldn't be set!\n");
		return;
	}

	enc_output->buffer_size = enc_output->buffer_size_recommended;
	enc_output->buffer_num = 6; //enc_output->buffer_num_recommended;

	printf("H264 encoder buffers In=%i | Out=%i\n", enc_input->buffer_num, enc_output->buffer_num);

	// set H264 profile and level
	MMAL_PARAMETER_VIDEO_PROFILE_T param2;
	param2.hdr.id = MMAL_PARAMETER_PROFILE;
	param2.hdr.size = sizeof(param2);
	param2.profile[0].profile = MMAL_VIDEO_PROFILE_H264_HIGH;
	param2.profile[0].level = MMAL_VIDEO_LEVEL_H264_4;
	status = mmal_port_parameter_set(enc_output, &param2.hdr);
	if (status != MMAL_SUCCESS) {
		printf("Unable to set H264 profile!\n");
		return;
	}

	// Set keyframe interval
	MMAL_PARAMETER_UINT32_T param = { {MMAL_PARAMETER_INTRAPERIOD, sizeof(param)}, 500};
	status = mmal_port_parameter_set(enc_output, &param.hdr);
	if (status != MMAL_SUCCESS) {
		printf("Unable to set intraperiod!\n");
		return;
	}

	enc_pool_out = mmal_port_pool_create(enc_output, enc_output->buffer_num, enc_output->buffer_size);
	if (enc_pool_out == NULL) {
		printf("Failed to create encoder output pool!\n");
	}

	// Enable output port
	status = mmal_port_enable(enc_output, enc_output_callback);
	if (status != MMAL_SUCCESS) {
		printf("Error enabling deinterlacer output port!\n");
		return;
	}

	status = mmal_connection_enable(conn_deint_enc);
	if (status != MMAL_SUCCESS) {
		printf("Failed to enable connection Deint->encoder\n");
		return;
	}

	// ###################################### generic initialization ##################################################

	status = mmal_component_enable(pISP);
	if (status != MMAL_SUCCESS) {
		printf("Error enabling ISP!\n");
		return;
	}

	status = mmal_component_enable(pImageFx);
	if (status != MMAL_SUCCESS) {
		printf("Error enabling deinterlacer!\n");
		return;
	}

	status = mmal_component_enable(pEncoder);
	if (status != MMAL_SUCCESS) {
		printf("Error enabling encoder!\n");
		return;
	}

	// Send buffers for output pool
	for (uint8_t i = 0; i < enc_output->buffer_num; i++) {
		MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(enc_pool_out->queue);

		if (!buffer) {
			printf("Buffer is NULL!\n");
			exit(1);
		}
		status = mmal_port_send_buffer(enc_output, buffer);
		if (status != MMAL_SUCCESS) {
			printf("mmal_port_send_buffer failed deint on buffer %p, status %d\n", buffer, status);
			exit(1);
		}
	}
#endif

	printf("Image converter init OK!\n");
}

void CloseConverter() {
	MMAL_STATUS_T status;

	printf("Closing converter...\n");

	if (pFile2 != NULL) {
		fclose(pFile2);
	}

	// disable and destroy connection between ISP and deinterlacer
	/*	mmal_connection_disable(conn_isp_deint);
	 mmal_connection_destroy(conn_isp_deint);

	 mmal_connection_disable(conn_deint_enc);
	 mmal_connection_destroy(conn_deint_enc);

	 // disable ports
	 mmal_port_disable(isp_input);
	 mmal_port_disable(enc_output);*/

	mmal_connection_disable(conn_isp_deint);
	mmal_port_disable(isp_input);
	mmal_port_disable(deint_output);

	// disable ISP
	status = mmal_component_disable(pISP);
	if (status != MMAL_SUCCESS) {
		printf("Failed to disable ISP component!\n");
	}

	// disable deinterlacer
	status = mmal_component_disable(pImageFx);
	if (status != MMAL_SUCCESS) {
		printf("Failed to disable ISP component!\n");
	}

	/*
	 // disable encoder
	 status = mmal_component_disable(pEncoder);
	 if (status != MMAL_SUCCESS) {
	 printf("Failed to disable Encoder component!\n");
	 }*/

	// destroy components
	mmal_component_destroy(pISP);
	mmal_component_destroy(pImageFx);
	/* mmal_component_destroy(pEncoder);*/
}
