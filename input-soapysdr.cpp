/*
 *  input-soapysdr.cpp
 *  SoapySDR-specific routines
 *
 *  Copyright (c) 2015-2017 Tomasz Lemiech <szpajder@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include <assert.h>
#include <limits.h>		// SCHAR_MAX, SHRT_MAX
#include <stdlib.h>		// calloc
#include <string.h>		// memcpy, strcmp
#include <syslog.h>		// LOG_* macros
#include <libconfig.h++>	// Setting
#include <SoapySDR/Device.h>	// SoapySDRDevice, SoapySDRDevice_makeStrArgs
#include <SoapySDR/Formats.h>	// SOAPY_SDR_CS constants
#include "input-common.h"	// input_t, sample_format_t, input_state_t, MODULE_EXPORT
#include "input-helpers.h"	// circbuffer_append
#include "input-soapysdr.h"	// soapysdr_dev_data_t
#include "rtl_airband.h"	// do_exit, fft_size, debug_print, XCALLOC, error()

using namespace std;

// Map SoapySDR sample format string to our internal sample format
// and set bytes_per_sample and fullscale values appropriately.
// We prefer U8 and S8 over S16 to minimize CPU load.
// If fullscale is > 0, it means it has been read by
// SoapySDRDevice_getNativeStreamFormat, so we treat this value as valid.
// Otherwise, guess a suitable default value.
static bool soapysdr_match_sfmt(input_t * const input, char const * const fmt, double const fullscale) {
	if(strcmp(fmt, SOAPY_SDR_CU8) == 0) {
		input->sfmt = SFMT_U8;
		input->bytes_per_sample = sizeof(unsigned char);
		input->fullscale = (fullscale > 0 ? fullscale : (float)SCHAR_MAX - 0.5f);
		goto matched;
	} else if(strcmp(fmt, SOAPY_SDR_CS8) == 0) {
		input->sfmt = SFMT_S8;
		input->bytes_per_sample = sizeof(char);
		input->fullscale = (fullscale > 0 ? fullscale : (float)SCHAR_MAX - 0.5f);
		goto matched;
	} else if(strcmp(fmt, SOAPY_SDR_CS16) == 0) {
		input->sfmt = SFMT_S16;
		input->bytes_per_sample = sizeof(short);
		input->fullscale = (fullscale > 0 ? fullscale : (float)SHRT_MAX - 0.5f);
		goto matched;
	}
	return false;
matched:
	soapysdr_dev_data_t *dev_data = (soapysdr_dev_data_t *)input->dev_data;
	dev_data->sample_format = strdup(fmt);
	return true;
}

// Open the device and choose a suitable sample format.
// Bail out if no supported sample format is found.
static bool soapysdr_choose_sample_format(input_t * const input) {
	bool ret = false;
	size_t len = 0;
	char **formats = NULL;
	soapysdr_dev_data_t *dev_data = (soapysdr_dev_data_t *)input->dev_data;
	SoapySDRDevice *sdr = SoapySDRDevice_makeStrArgs(dev_data->device_string);
	if (sdr == NULL) {
		log(LOG_ERR, "Failed to open SoapySDR device '%s': %s\n", dev_data->device_string,
			SoapySDRDevice_lastError());
		error();
	}
	input->sfmt = SFMT_UNDEF;
// First try device's native format to avoid extra conversion
	double fullscale = 0.0;
	char *fmt = SoapySDRDevice_getNativeStreamFormat(sdr, SOAPY_SDR_RX, 0, &fullscale);

	if(soapysdr_match_sfmt(input, fmt, fullscale) == true) {
		log(LOG_NOTICE, "SoapySDR: device '%s': using native sample format '%s' (fullScale=%.1f)\n",
			dev_data->device_string, fmt, input->fullscale);
		ret = true;
		goto end;
	}
// Native format is not supported by rtl_airband; find out if there is anything else.
	formats = SoapySDRDevice_getStreamFormats(sdr, SOAPY_SDR_RX, 0, &len);
	if(formats == NULL || len == 0) {
		log(LOG_ERR, "SoapySDR: device '%s': failed to read supported sample formats\n",
			dev_data->device_string);
		ret = false;
		goto end;
	}
	for(size_t i = 0; i < len; i++) {
		if(soapysdr_match_sfmt(input, formats[i], -1.0) == true) {
			log(LOG_NOTICE, "SoapySDR: device '%s': using non-native sample format '%s' (assuming fullScale=%.1f)\n",
				dev_data->device_string, formats[i], input->fullscale);
			ret = true;
			goto end;
		}
	}
// Nothing found; we can't use this device.
	log(LOG_ERR, "SoapySDR: device '%s': no supported sample format found\n", dev_data->device_string);
end:
	SoapySDRDevice_unmake(sdr);
	return ret;
}

int soapysdr_parse_config(input_t * const input, libconfig::Setting &cfg) {
	soapysdr_dev_data_t *dev_data = (soapysdr_dev_data_t *)input->dev_data;

	if(cfg.exists("device_string")) {
		dev_data->device_string = strdup(cfg["device_string"]);
	} else {
		cerr<<"SoapySDR configuration error: mandatory parameter missing: device_string\n";
		error();
	}
	if(cfg.exists("gain")) {
		if(cfg["gain"].getType() == libconfig::Setting::TypeInt) {
			dev_data->gain = (double)((int)cfg["gain"]);
		} else if(cfg["gain"].getType() == libconfig::Setting::TypeFloat) {
			dev_data->gain = (double)cfg["gain"];
		}
	} else {
		cerr<<"SoapySDR configuration error: device '"<<dev_data->device_string<<
			"': gain is not configured\n";
		error();
	}
	if(dev_data->gain < 0) {
		cerr<<"SoapySDR configuration error: device '"<<dev_data->device_string<<
			"': gain value must be positive\n";
		error();
	}
	if(cfg.exists("correction")) {
		if(cfg["correction"].getType() == libconfig::Setting::TypeInt) {
			dev_data->correction = (double)((int)cfg["correction"]);
		} else if(cfg["correction"].getType() == libconfig::Setting::TypeFloat) {
			dev_data->correction = (float)cfg["correction"];
		} else {
			cerr<<"SoapySDR configuration error: device '"<<dev_data->device_string<<
				"': correction value must be numeric\n";
			error();
		}
	}
// We have to do this here and not in soapysdr_init, because parse_devices()
// needs correct bytes_per_sample value to calculate the size of the sample buffer and
// this is done before soapysdr_init() is run.
	if(soapysdr_choose_sample_format(input) == false) {
		cerr<<"SoapySDR configuration error: device '"<<dev_data->device_string<<
			"': no suitable sample format found\n";
		error();
	}
	return 0;
}

int soapysdr_init(input_t * const input) {
	soapysdr_dev_data_t *dev_data = (soapysdr_dev_data_t *)input->dev_data;

	dev_data->dev = SoapySDRDevice_makeStrArgs(dev_data->device_string);
	if (dev_data->dev == NULL) {
		log(LOG_ERR, "Failed to open SoapySDR device '%s': %s\n", dev_data->device_string,
			SoapySDRDevice_lastError());
		error();
	}
	SoapySDRDevice *sdr = dev_data->dev;

	if(SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_RX, 0, input->sample_rate) != 0) {
		log(LOG_ERR, "Failed to set sample rate for SoapySDR device '%s': %s\n",
			dev_data->device_string, SoapySDRDevice_lastError());
		error();
	}
	if(SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_RX, 0, input->centerfreq, NULL) != 0) {
		log(LOG_ERR, "Failed to set frequency for SoapySDR device '%s': %s\n",
			dev_data->device_string, SoapySDRDevice_lastError());
		error();
	}
	if(SoapySDRDevice_setFrequencyCorrection(sdr, SOAPY_SDR_RX, 0, dev_data->correction) != 0) {
		log(LOG_ERR, "Failed to set frequency correction for SoapySDR device '%s': %s\n",
			dev_data->device_string, SoapySDRDevice_lastError());
		error();
	}
	if(SoapySDRDevice_setGainMode(sdr, SOAPY_SDR_RX, 0, false) != 0) {
		log(LOG_ERR, "Failed to set gain mode to manual for SoapySDR device '%s': %s\n",
			dev_data->device_string, SoapySDRDevice_lastError());
		error();
	}
	if(SoapySDRDevice_setGain(sdr, SOAPY_SDR_RX, 0, dev_data->gain) != 0) {
		log(LOG_ERR, "Failed to set gain for SoapySDR device '%s': %s\n",
			dev_data->device_string, SoapySDRDevice_lastError());
		error();
	}
	log(LOG_INFO, "SoapySDR: device '%s': gain set to %.1f dB\n",
		dev_data->device_string, SoapySDRDevice_getGain(sdr, SOAPY_SDR_RX, 0));
	log(LOG_INFO, "SoapySDR: device '%s' initialized\n", dev_data->device_string);
	return 0;
}

void *soapysdr_rx_thread(void *ctx) {
	input_t *input = (input_t *)ctx;
	soapysdr_dev_data_t *dev_data = (soapysdr_dev_data_t *)input->dev_data;
	SoapySDRDevice *sdr = dev_data->dev;
	assert(sdr != NULL);

	unsigned char buf[SOAPYSDR_BUFSIZE];
// size of the buffer in number of I/Q sample pairs
	size_t num_elems = SOAPYSDR_BUFSIZE / (2 * input->bytes_per_sample);

	SoapySDRStream *rxStream = NULL;
	if(SoapySDRDevice_setupStream(sdr, &rxStream, SOAPY_SDR_RX, dev_data->sample_format, NULL, 0, NULL) != 0) {
		log(LOG_ERR, "Failed to set up stream for SoapySDR device '%s': %s\n",
			dev_data->device_string, SoapySDRDevice_lastError());
		input->state = INPUT_FAILED;
		goto cleanup;
	}
	if(SoapySDRDevice_activateStream(sdr, rxStream, 0, 0, 0)) { //start streaming
		log(LOG_ERR, "Failed to activate stream for SoapySDR device '%s': %s\n",
			dev_data->device_string, SoapySDRDevice_lastError());
		input->state = INPUT_FAILED;
		goto cleanup;
	}
	input->state = INPUT_RUNNING;
	log(LOG_NOTICE, "SoapySDR: device '%s' started\n", dev_data->device_string);

	while(!do_exit) {
		void *bufs[] = { buf };		// array of buffers
		int flags;			// flags set by receive operation
		long long timeNs;		// timestamp for receive buffer
		int samples_read = SoapySDRDevice_readStream(sdr, rxStream, bufs, num_elems, &flags, &timeNs, 100000);
		if(samples_read < 0) {	// when it's negative, it's the error code
			log(LOG_ERR, "SoapySDR device '%s': readStream failed: %s, disabling\n",
				dev_data->device_string, SoapySDR_errToStr(samples_read));
			input->state = INPUT_FAILED;
			goto cleanup;
		}
		circbuffer_append(input, buf, (size_t)(samples_read * 2 * input->bytes_per_sample));
	}
cleanup:
	SoapySDRDevice_deactivateStream(sdr, rxStream, 0, 0);
	SoapySDRDevice_closeStream(sdr, rxStream);
	SoapySDRDevice_unmake(sdr);
	return 0;
}

int soapysdr_set_centerfreq(input_t * const input, int const centerfreq) {
	soapysdr_dev_data_t *dev_data = (soapysdr_dev_data_t *)input->dev_data;
	assert(dev_data->dev != NULL);

	if(SoapySDRDevice_setFrequency(dev_data->dev, SOAPY_SDR_RX, 0, input->centerfreq, NULL) != 0) {
		log(LOG_ERR, "Failed to set frequency for SoapySDR device '%s': %s\n",
			dev_data->device_string, SoapySDRDevice_lastError());
		return -1;
	}
	return 0;
}

MODULE_EXPORT input_t *soapysdr_input_new() {
	soapysdr_dev_data_t *dev_data = (soapysdr_dev_data_t *)XCALLOC(1, sizeof(soapysdr_dev_data_t));
	dev_data->gain = -1.0;	// invalid default gain value
/*	return &( input_t ){
		.dev_data = dev_data,
		.state = INPUT_UNKNOWN,
		.sfmt = SFMT_U8,
		.sample_rate = SOAPYSDR_DEFAULT_SAMPLE_RATE,
		.parse_config = &soapysdr_parse_config,
		.init = &soapysdr_init,
		.run_rx_thread = &soapysdr_rx_thread,
		.set_centerfreq = &soapysdr_set_centerfreq,
		.stop = &soapysdr_stop
	}; */
	input_t *input = (input_t *)XCALLOC(1, sizeof(input_t));
	input->dev_data = dev_data;
// invalid values as defaults
	input->state = INPUT_UNKNOWN;
	input->sfmt = SFMT_UNDEF;
	input->fullscale = 0.0f;
	input->bytes_per_sample = 0;

	input->sample_rate = SOAPYSDR_DEFAULT_SAMPLE_RATE;
	input->parse_config = &soapysdr_parse_config;
	input->init = &soapysdr_init;
	input->run_rx_thread = &soapysdr_rx_thread;
	input->set_centerfreq = &soapysdr_set_centerfreq;
	input->stop = NULL;
	return input;
}