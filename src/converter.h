/*
 * converter.h
 *
 *  Created on: 05/07/2018
 *      Author: xubuntu
 */

#ifndef CONVERTER_H_
#define CONVERTER_H_

#include <stdint.h>

void InitConverter();
void CloseConverter();

void ConvertFrame(uint8_t* punBuffer, const uint32_t unBufferSize);
void SetOutputBitrate(const uint16_t unBitrate);

#endif /* CONVERTER_H_ */
