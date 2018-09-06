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

// H264 Encoder variables
MMAL_COMPONENT_T *pEncoder = NULL;
MMAL_PORT_T *enc_input = NULL;
MMAL_PORT_T *enc_output = NULL;
MMAL_POOL_T *enc_pool_out = NULL;

// Connections between components
MMAL_CONNECTION_T* conn_isp_deint = NULL;
MMAL_CONNECTION_T* conn_deint_encoder = NULL;

// hard-coded constants for test purposes only
#define VIDEO_WIDTH 	1920
#define VIDEO_HEIGHT	1080

FILE * pFile2 = NULL;

///////// TODO: REMOVE THIS AS SOON AS IT IS INCLUDED ON THE RPI FIRMWARE
/** Specify that the connection should not modify the port formats. */
#define MMAL_CONNECTION_FLAG_KEEP_PORT_FORMATS 0x20

void ConvertFrame(uint8_t* punBuffer, const uint32_t unBufferSize) {
	MMAL_BUFFER_HEADER_T *buffer;

	// send data to the input port of ISP component
	buffer = mmal_queue_get(isp_pool_in->queue);
	if (buffer != NULL) {
		mmal_buffer_header_mem_lock(buffer);
		buffer->flags = MMAL_BUFFER_HEADER_VIDEO_FLAG_INTERLACED | MMAL_BUFFER_HEADER_VIDEO_FLAG_TOP_FIELD_FIRST;
		buffer->length = unBufferSize;
		buffer->pts = buffer->dts = MMAL_TIME_UNKNOWN;

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

		if (new_buffer) {
			status = mmal_port_send_buffer(port, new_buffer);
		}

		if (!new_buffer || status != MMAL_SUCCESS) {
			printf("Unable to return a buffer to the encoder port!\n");
		}
	}
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

	status = mmal_port_parameter_set_boolean(isp_input, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
	if (status != MMAL_SUCCESS) {
		printf("Failed to set zero copy on ISP input!\n");
		return;
	}

	isp_input->buffer_size = isp_input->buffer_size_recommended;
	isp_input->buffer_num = 2;

	// create pool for input data
	isp_pool_in = mmal_port_pool_create(isp_input, isp_input->buffer_num, isp_input->buffer_size);
	if (isp_pool_in == NULL) {
		printf("Failed to create ISP input pool!\n");
	}

	// Setup ISP output (copy of input format, changing only the encoding)
	mmal_format_copy(isp_output->format, isp_input->format);
	isp_output->format->encoding = MMAL_ENCODING_I420;
	status = mmal_port_format_commit(isp_output);
	if (status != MMAL_SUCCESS) {
		printf("Failed to commit converter output format!\n");
		return;
	}

	status = mmal_port_parameter_set_boolean(isp_output, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
	if (status != MMAL_SUCCESS) {
		printf("Failed to set zero copy on ISP output!\n");
		return;
	}

	isp_output->buffer_size = isp_output->buffer_size_recommended;
	isp_output->buffer_num = 10;

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

	MMAL_PARAMETER_IMAGEFX_PARAMETERS_T img_fx_param;
	memset(&img_fx_param, 0, sizeof(MMAL_PARAMETER_IMAGEFX_PARAMETERS_T));
	img_fx_param.hdr.id = MMAL_PARAMETER_IMAGE_EFFECT_PARAMETERS;
	img_fx_param.hdr.size = sizeof(MMAL_PARAMETER_IMAGEFX_PARAMETERS_T);

	// Advanced deinterlacer using QPUs
	img_fx_param.effect = MMAL_PARAM_IMAGEFX_DEINTERLACE_ADV;
	img_fx_param.num_effect_params = 4;
	img_fx_param.effect_parameter[0] = 3; // interlaced input frame with both fields / top field first
	img_fx_param.effect_parameter[1] = 0; // frame period (1000000 * 1 / 25);
	img_fx_param.effect_parameter[2] = 0; // half framerate ?
	img_fx_param.effect_parameter[3] = 1; // use QPU ?

	if (mmal_port_parameter_set(deint_output, &img_fx_param.hdr) != MMAL_SUCCESS) {
		printf("Failed to configure deinterlacer output port (mode)\n");
		return;
	}

	// Setup image_fx input format
	deint_input->format->type = MMAL_ES_TYPE_VIDEO;
	deint_input->format->encoding = MMAL_ENCODING_I420;
	deint_input->format->es->video.width = VCOS_ALIGN_UP(VIDEO_WIDTH, 32);
	deint_input->format->es->video.height = VCOS_ALIGN_UP(VIDEO_HEIGHT, 16);
	deint_input->format->es->video.crop.x = 0;
	deint_input->format->es->video.crop.y = 0;
	deint_input->format->es->video.crop.width = VIDEO_WIDTH;
	deint_input->format->es->video.crop.height = VIDEO_HEIGHT;
	deint_input->format->es->video.frame_rate.num = 0;
	deint_input->format->es->video.frame_rate.den = 1;
	status = mmal_port_format_commit(deint_input);
	if (status != MMAL_SUCCESS) {
		printf("Failed to commit image_fx input format!\n");
		return;
	}

	status = mmal_port_parameter_set_boolean(deint_input, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
	if (status != MMAL_SUCCESS) {
		printf("Failed to set zero copy on image_fx input!\n");
		return;
	}

	deint_input->buffer_size = deint_input->buffer_size_recommended;
	deint_input->buffer_num = 15;

	// Setup image_fx output format (equal to input format)
	mmal_format_copy(deint_output->format, deint_input->format);
	status = mmal_port_format_commit(deint_output);
	if (status != MMAL_SUCCESS) {
		printf("Failed to commit image_fx output format!\n");
		return;
	}

	status = mmal_port_parameter_set_boolean(deint_output, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
	if (status != MMAL_SUCCESS) {
		printf("Failed to set zero copy on image_fx output!\n");
		return;
	}

	deint_output->buffer_size = deint_output->buffer_size_recommended;
	deint_output->buffer_num = 8;

	printf("Create connection ISP output to image_fx input...\n");
	status = mmal_connection_create(&conn_isp_deint, isp_output, deint_input,
	MMAL_CONNECTION_FLAG_TUNNELLING |
	MMAL_CONNECTION_FLAG_KEEP_BUFFER_REQUIREMENTS |
	MMAL_CONNECTION_FLAG_KEEP_PORT_FORMATS);
	if (status != MMAL_SUCCESS) {
		printf("Failed to create connection status %d: ISP->image_fx\n", status);
		return;
	}

	status = mmal_connection_enable(conn_isp_deint);
	if (status != MMAL_SUCCESS) {
		printf("Failed to enable connection ISP->image_fx\n");
		return;
	}

	// ################################## Encoder ######################################################

	status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &pEncoder);
	if (status != MMAL_SUCCESS) {
		printf("Failed to create video encoder component.\n");
		return;
	}

	enc_input = pEncoder->input[0];
	enc_output = pEncoder->output[0];

	printf("Create connection image_fx output to encoder input...\n");
	status = mmal_connection_create(&conn_deint_encoder, deint_output, enc_input, MMAL_CONNECTION_FLAG_TUNNELLING |
	MMAL_CONNECTION_FLAG_KEEP_BUFFER_REQUIREMENTS |
	MMAL_CONNECTION_FLAG_KEEP_PORT_FORMATS);
	if (status != MMAL_SUCCESS) {
		printf("Failed to create connection status %d: Deint->encoder\n", status);
		return;
	}

	/*if (mmal_port_parameter_set_boolean(enc_input, MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT, 1) != MMAL_SUCCESS) {
		printf("Unable to set immutable input flag!\n");
	}*/

	/*enc_input->format->type = MMAL_ES_TYPE_VIDEO;
	enc_input->format->encoding = MMAL_ENCODING_I420;
	enc_input->format->encoding_variant = MMAL_ENCODING_I420;
	enc_input->format->es->video.width = VCOS_ALIGN_UP(1920, 32);
	enc_input->format->es->video.height = VCOS_ALIGN_UP(1080, 16);
	enc_input->format->es->video.crop.x = 0;
	enc_input->format->es->video.crop.y = 0;
	enc_input->format->es->video.crop.width = 1920;
	enc_input->format->es->video.crop.height = 1080;
	enc_input->format->es->video.frame_rate.num = 0;
	enc_input->format->es->video.frame_rate.den = 1;*/
	mmal_format_copy(enc_input->format, deint_output->format);
	status = mmal_port_format_commit(enc_input);
	if (status != MMAL_SUCCESS) {
		printf("Video encoder input format couldn't be set!\n");
		return;
	}
    
    status = mmal_port_parameter_set_boolean(enc_input, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
	if (status != MMAL_SUCCESS) {
		printf("Failed to set zero copy on H264 encoder input!\n");
		return;
	}

	enc_input->buffer_size = enc_input->buffer_size_recommended;
	enc_input->buffer_num = 8;

	printf("H264 encoder buffer size = %i bytes\n", enc_input->buffer_size);

	mmal_format_copy(enc_output->format, enc_input->format);
	enc_output->format->encoding = MMAL_ENCODING_H264;
	enc_output->format->bitrate = 1000 * 1000 * 5;
	enc_output->format->es->video.frame_rate.num = 0;
	enc_output->format->es->video.frame_rate.den = 1;

	status = mmal_port_format_commit(enc_output);
	if (status != MMAL_SUCCESS) {
		printf("Video encoder output format couldn't be set!\n");
		return;
	}
    
    status = mmal_port_parameter_set_boolean(enc_output, MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
	if (status != MMAL_SUCCESS) {
		printf("Failed to set zero copy on H264 encoder output!\n");
		return;
	}

	enc_output->buffer_size = enc_output->buffer_size_recommended;
	enc_output->buffer_num = 2;

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

	status = mmal_connection_enable(conn_deint_encoder);
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
			printf("mmal_port_send_buffer failed enc on buffer %p, status %d\n", buffer, status);
			exit(1);
		}
	}

	printf("Image converter init OK!\n");
}

void CloseConverter() {
	MMAL_STATUS_T status;

	printf("Closing converter...\n");

	// disable and destroy connection between ISP and deinterlacer
	mmal_connection_disable(conn_isp_deint);
	mmal_connection_destroy(conn_isp_deint);

	mmal_connection_disable(conn_deint_encoder);
	mmal_connection_destroy(conn_deint_encoder);

	// disable ports
	mmal_port_disable(isp_input);
	mmal_port_disable(isp_output);
	mmal_port_disable(deint_input);
	mmal_port_disable(deint_output);
	mmal_port_disable(enc_input);
	mmal_port_disable(enc_output);

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

	// disable renderer
	status = mmal_component_disable(pEncoder);
	if (status != MMAL_SUCCESS) {
		printf("Failed to disable Renderer component!\n");
	}

	// destroy components
	mmal_component_destroy(pISP);
	mmal_component_destroy(pImageFx);
	mmal_component_destroy(pEncoder);

	if (pFile2 != NULL) {
		fclose(pFile2);
	}
}
