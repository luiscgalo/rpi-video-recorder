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
MMAL_COMPONENT_T *pRenderer = NULL;
MMAL_PORT_T *enc_input = NULL;
MMAL_PORT_T *enc_output = NULL;
MMAL_POOL_T *enc_pool_out = NULL;

// Connections between components
MMAL_CONNECTION_T* conn_isp_deint = NULL;
MMAL_CONNECTION_T* conn_deint_renderer = NULL;

// hard-coded constants for test purposes only
#define VIDEO_WIDTH 	1920
#define VIDEO_HEIGHT	1080


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

	isp_input->buffer_size = isp_input->buffer_size_recommended;
	isp_input->buffer_num = 4;

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

	isp_output->buffer_size = isp_output->buffer_size_recommended;
	isp_output->buffer_num = 4;

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

	deint_input->buffer_size = deint_input->buffer_size_recommended;
	deint_input->buffer_num = 6;

	// Setup image_fx output format (equal to input format)
	mmal_format_copy(deint_output->format, deint_input->format);

	status = mmal_port_format_commit(deint_output);
	if (status != MMAL_SUCCESS) {
		printf("Failed to commit image_fx output format!\n");
		return;
	}

	deint_output->buffer_size = deint_output->buffer_size_recommended;
	deint_output->buffer_num = 6;

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

	// ################################## renderer ######################################################

	// renderer component
	status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, &pRenderer);
	if (status != MMAL_SUCCESS) {
		printf("Unable to create video encoder component!\n");
		return;
	}

	enc_input = pRenderer->input[0];

	printf("Create connection image_fx output to renderer input...\n");
	status = mmal_connection_create(&conn_deint_renderer, deint_output, enc_input,
			MMAL_CONNECTION_FLAG_TUNNELLING
			| MMAL_CONNECTION_FLAG_KEEP_BUFFER_REQUIREMENTS
			/*| MMAL_CONNECTION_FLAG_KEEP_PORT_FORMAT */
			);
	if (status != MMAL_SUCCESS) {
		printf("Failed to create connection status %d: Deint->encoder\n", status);
		return;
	}

	status = mmal_connection_enable(conn_deint_renderer);
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

	status = mmal_component_enable(pRenderer);
	if (status != MMAL_SUCCESS) {
		printf("Error enabling encoder!\n");
		return;
	}

	printf("Image converter init OK!\n");
}

void CloseConverter() {
	MMAL_STATUS_T status;

	printf("Closing converter...\n");

	// disable and destroy connection between ISP and deinterlacer
	mmal_connection_disable(conn_isp_deint);
	mmal_connection_destroy(conn_isp_deint);

	mmal_connection_disable(conn_deint_renderer);
	mmal_connection_destroy(conn_deint_renderer);

	// disable ports
	mmal_port_disable(isp_input);

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
	status = mmal_component_disable(pRenderer);
	if (status != MMAL_SUCCESS) {
		printf("Failed to disable Renderer component!\n");
	}

	// destroy components
	mmal_component_destroy(pISP);
	mmal_component_destroy(pImageFx);
	mmal_component_destroy(pRenderer);
}
