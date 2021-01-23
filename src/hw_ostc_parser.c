/*
 * libdivecomputer
 *
 * Copyright (C) 2010 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

#include "libdivecomputer/units.h"

#include "hw_ostc.h"
#include "hw_ostc3.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &hw_ostc_parser_vtable)

#define MAXCONFIG 7
#define NGASMIXES 15

#define UNDEFINED 0xFFFFFFFF

#define ALL    0
#define FIXED  1
#define MANUAL 2

#define HEADER  1
#define PROFILE 2

#define TEMPERATURE 0
#define DECO        1
#define GF          2
#define PPO2        3
#define DECOPLAN    4
#define CNS         5
#define TANK        6

#define OSTC_ZHL16_OC    0
#define OSTC_GAUGE       1
#define OSTC_ZHL16_CC    2
#define OSTC_APNEA       3
#define OSTC_ZHL16_OC_GF 4
#define OSTC_ZHL16_CC_GF 5
#define OSTC_PSCR_GF     6

#define FROG_ZHL16    0
#define FROG_ZHL16_GF 1
#define FROG_APNEA    2

#define OSTC3_OC    0
#define OSTC3_CC    1
#define OSTC3_GAUGE 2
#define OSTC3_APNEA 3
#define OSTC3_PSCR  4

#define OSTC3_ZHL16    0
#define OSTC3_ZHL16_GF 1
#define OSTC4_VPM      2

#define OSTC4      0x3B

#define UNSUPPORTED 0xFFFFFFFF

#define OSTC3FW(major,minor) ( \
		(((major) & 0xFF) << 8) | \
		((minor) & 0xFF))

#define OSTC4FW(major,minor,micro,beta) ( \
		(((major) & 0x1F) << 11) | \
		(((minor) & 0x1F) >> 6) | \
		(((micro) & 0x1F) << 1) | \
		((beta) & 0x01))

typedef struct hw_ostc_sample_info_t {
	unsigned int type;
	unsigned int divisor;
	unsigned int size;
} hw_ostc_sample_info_t;

typedef struct hw_ostc_layout_t {
	unsigned int datetime;
	unsigned int maxdepth;
	unsigned int avgdepth;
	unsigned int divetime;
	unsigned int atmospheric;
	unsigned int salinity;
	unsigned int duration;
	unsigned int temperature;
	unsigned int battery;
	unsigned int desat;
	unsigned int firmware;
	unsigned int deco_info1;
	unsigned int deco_info2;
	unsigned int decomode;
	unsigned int battery_percentage;
} hw_ostc_layout_t;

typedef struct hw_ostc_gasmix_t {
	unsigned int oxygen;
	unsigned int helium;
} hw_ostc_gasmix_t;

typedef struct hw_ostc_parser_t {
	dc_parser_t base;
	unsigned int hwos;
	unsigned int model;
	unsigned int serial;
	// Cached fields.
	unsigned int cached;
	unsigned int version;
	unsigned int header;
	const hw_ostc_layout_t *layout;
	unsigned int ngasmixes;
	unsigned int nfixed;
	unsigned int initial;
	unsigned int initial_setpoint;
	unsigned int initial_cns;
	hw_ostc_gasmix_t gasmix[NGASMIXES];
} hw_ostc_parser_t;

static dc_status_t hw_ostc_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t hw_ostc_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t hw_ostc_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t hw_ostc_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t hw_ostc_parser_vtable = {
	sizeof(hw_ostc_parser_t),
	DC_FAMILY_HW_OSTC,
	hw_ostc_parser_set_data, /* set_data */
	hw_ostc_parser_get_datetime, /* datetime */
	hw_ostc_parser_get_field, /* fields */
	hw_ostc_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static const hw_ostc_layout_t hw_ostc_layout_ostc = {
	3,  /* datetime */
	8,  /* maxdepth */
	45, /* avgdepth */
	10, /* divetime */
	15, /* atmospheric */
	43, /* salinity */
	47, /* duration */
	13, /* temperature */
	34, /* battery volt after dive */
	17, /* desat */
	32, /* firmware */
	49, /* deco_info1 */
	50, /* deco_info1 */
	51, /* decomode */
	0,  /* battery percentage TBD */
};

static const hw_ostc_layout_t hw_ostc_layout_frog = {
	9,  /* datetime */
	14, /* maxdepth */
	45, /* avgdepth */
	16, /* divetime */
	21, /* atmospheric */
	43, /* salinity */
	47, /* duration */
	19, /* temperature */
	34, /* battery volt after dive */
	23, /* desat */
	32, /* firmware */
	49, /* deco_info1 */
	50, /* deco_info2 */
	51, /* decomode */
	0,  /* battery percentage TBD */
};

static const hw_ostc_layout_t hw_ostc_layout_ostc3 = {
	12, /* datetime */
	17, /* maxdepth */
	73, /* avgdepth */
	19, /* divetime */
	24, /* atmospheric */
	70, /* salinity */
	75, /* duration */
	22, /* temperature */
	50, /* battery volt after dive */
	26, /* desat */
	48, /* firmware */
	77, /* deco_info1 */
	78, /* deco_info2 */
	79, /* decomode */
        59, /* battery percentage */
};

static unsigned int
hw_ostc_find_gasmix (hw_ostc_parser_t *parser, unsigned int o2, unsigned int he, unsigned int type)
{
	unsigned int offset = 0;
	unsigned int count = parser->ngasmixes;
	if (type == FIXED) {
		count = parser->nfixed;
	} else if (type == MANUAL) {
		offset = parser->nfixed;
	}

	unsigned int i = offset;
	while (i < count) {
		if (o2 == parser->gasmix[i].oxygen && he == parser->gasmix[i].helium)
			break;
		i++;
	}

	return i;
}

static dc_status_t
hw_ostc_parser_cache (hw_ostc_parser_t *parser)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}

	if (size < 9) {
		ERROR(abstract->context, "Header too small.");
		return DC_STATUS_DATAFORMAT;
	}

	// Check the profile version
	unsigned int version = data[parser->hwos ? 8 : 2];
	const hw_ostc_layout_t *layout = NULL;
	unsigned int header = 0;
	switch (version) {
	case 0x20:
		layout = &hw_ostc_layout_ostc;
		header = 47;
		break;
	case 0x21:
		layout = &hw_ostc_layout_ostc;
		header = 57;
		break;
	case 0x22:
		layout = &hw_ostc_layout_frog;
		header = 256;
		break;
	case 0x23:
	case 0x24:
		layout = &hw_ostc_layout_ostc3;
		header = 256;
		break;
	default:
		ERROR(abstract->context, "Unknown data format version.");
		return DC_STATUS_DATAFORMAT;
	}

	if (size < header) {
		ERROR(abstract->context, "Header too small.");
		return DC_STATUS_DATAFORMAT;
	}

	// Get all the gas mixes, the index of the inital mix,
	// the initial setpoint (used in the fixed setpoint CCR mode),
	// and the initial CNS from the header
	unsigned int initial = UNDEFINED;
	unsigned int initial_setpoint = UNDEFINED;
	unsigned int initial_cns = UNDEFINED;
	unsigned int ngasmixes = 0;
	hw_ostc_gasmix_t gasmix[NGASMIXES] = {{0}};
	if (version == 0x22) {
		ngasmixes = 3;
		if (data[31] != 0xFF) {
			initial = data[31];
		}
		for (unsigned int i = 0; i < ngasmixes; ++i) {
			gasmix[i].oxygen = data[25 + 2 * i];
			gasmix[i].helium = 0;
		}
	} else if (version == 0x23 || version == 0x24) {
		ngasmixes = 5;
		for (unsigned int i = 0; i < ngasmixes; ++i) {
			gasmix[i].oxygen = data[28 + 4 * i + 0];
			gasmix[i].helium = data[28 + 4 * i + 1];
			// Find the first gas marked as the initial gas.
			if (initial == UNDEFINED && data[28 + 4 * i + 3] == 1) {
				initial = i + 1; /* One based index! */
			}
		}
		// The first fixed setpoint is the initial setpoint in CCR mode.
		if (data[82] == OSTC3_CC) {
			initial_setpoint = data[60];
		}
		// Initial CNS
		initial_cns = array_uint16_le (data + 53);
	} else {
		ngasmixes = 5;
		if (data[31] != 0xFF) {
			initial = data[31];
		}
		for (unsigned int i = 0; i < ngasmixes; ++i) {
			gasmix[i].oxygen = data[19 + 2 * i + 0];
			gasmix[i].helium = data[19 + 2 * i + 1];
		}
	}
	if (initial != UNDEFINED) {
		if (initial < 1 || initial > ngasmixes) {
			ERROR(abstract->context, "Invalid initial gas mix.");
			return DC_STATUS_DATAFORMAT;
		}
		initial--; /* Convert to a zero based index. */
	} else {
		WARNING(abstract->context, "No initial gas mix available.");
	}

	// Cache the data for later use.
	parser->version = version;
	parser->header = header;
	parser->layout = layout;
	parser->ngasmixes = ngasmixes;
	parser->nfixed = ngasmixes;
	parser->initial = initial;
	parser->initial_setpoint = initial_setpoint;
	parser->initial_cns = initial_cns;
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->gasmix[i] = gasmix[i];
	}
	parser->cached = HEADER;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
hw_ostc_parser_create_internal (dc_parser_t **out, dc_context_t *context, unsigned int serial, unsigned int hwos, unsigned int model)
{
	hw_ostc_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (hw_ostc_parser_t *) dc_parser_allocate (context, &hw_ostc_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->hwos = hwos;
	parser->model = model;
	parser->cached = 0;
	parser->version = 0;
	parser->header = 0;
	parser->layout = NULL;
	parser->ngasmixes = 0;
	parser->nfixed = 0;
	parser->initial = 0;
	parser->initial_setpoint = 0;
	parser->initial_cns = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->gasmix[i].oxygen = 0;
		parser->gasmix[i].helium = 0;
	}
	parser->serial = serial;

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}


dc_status_t
hw_ostc_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int serial)
{
	return hw_ostc_parser_create_internal (out, context, serial, 0, 0);
}

dc_status_t
hw_ostc3_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int serial, unsigned int model)
{
	return hw_ostc_parser_create_internal (out, context, serial, 1, model);
}

static dc_status_t
hw_ostc_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	hw_ostc_parser_t *parser = (hw_ostc_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->version = 0;
	parser->header = 0;
	parser->layout = NULL;
	parser->ngasmixes = 0;
	parser->nfixed = 0;
	parser->initial = 0;
	parser->initial_setpoint = 0;
	parser->initial_cns = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->gasmix[i].oxygen = 0;
		parser->gasmix[i].helium = 0;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	hw_ostc_parser_t *parser = (hw_ostc_parser_t *) abstract;
	const unsigned char *data = abstract->data;

	// Cache the header data.
	dc_status_t rc = hw_ostc_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int version = parser->version;
	const hw_ostc_layout_t *layout = parser->layout;

	unsigned int divetime = 0;
	if (version > 0x20) {
		// Use the dive time stored in the extended header, rounded down towards
		// the nearest minute, to match the value displayed by the ostc.
		divetime = (array_uint16_le (data + layout->duration) / 60) * 60;
	} else {
		// Use the normal dive time (excluding the shallow parts of the dive).
		divetime = array_uint16_le (data + layout->divetime) * 60 + data[layout->divetime + 2];
	}

	const unsigned char *p = data + layout->datetime;

	dc_datetime_t dt;
	if (version == 0x23 || version == 0x24) {
		dt.year   = p[0] + 2000;
		dt.month  = p[1];
		dt.day    = p[2];
	} else {
		dt.year   = p[2] + 2000;
		dt.month  = p[0];
		dt.day    = p[1];
	}
	dt.hour   = p[3];
	dt.minute = p[4];
	dt.second = 0;
	dt.timezone = DC_TIMEZONE_NONE;

	if (version == 0x24) {
		if (datetime)
			*datetime = dt;
	} else {
		dc_ticks_t ticks = dc_datetime_mktime (&dt);
		if (ticks == (dc_ticks_t) -1)
			return DC_STATUS_DATAFORMAT;

		ticks -= divetime;

		if (!dc_datetime_gmtime (datetime, ticks))
			return DC_STATUS_DATAFORMAT;

		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}

#define BUFLEN 32

static dc_status_t
hw_ostc_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	hw_ostc_parser_t *parser = (hw_ostc_parser_t *) abstract;
	const unsigned char *data = abstract->data;

	// Cache the header data.
	dc_status_t rc = hw_ostc_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Cache the profile data.
	if (parser->cached < PROFILE) {
		rc = hw_ostc_parser_samples_foreach (abstract, NULL, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	unsigned int version = parser->version;
	const hw_ostc_layout_t *layout = parser->layout;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;
	dc_field_string_t *string = (dc_field_string_t *) value;

	unsigned int salinity = data[layout->salinity];
	if (version == 0x23 || version == 0x24)
		salinity += 100;
	char buf[BUFLEN];

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_le (data + layout->divetime) * 60 + data[layout->divetime + 2];
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_le (data + layout->maxdepth) / 100.0;
			break;
		case DC_FIELD_AVGDEPTH:
			if (parser->version < 0x21)
				return DC_STATUS_UNSUPPORTED;
			*((double *) value) = array_uint16_le (data + layout->avgdepth) / 100.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->oxygen = parser->gasmix[flags].oxygen / 100.0;
			gasmix->helium = parser->gasmix[flags].helium / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_SALINITY:
			if (salinity < 100 || salinity > 104)
				return DC_STATUS_UNSUPPORTED;

			if (salinity == 100)
				water->type = DC_WATER_FRESH;
			else
				water->type = DC_WATER_SALT;
			water->density = salinity * 10.0;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = array_uint16_le (data + layout->atmospheric) / 1000.0;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = (signed short) array_uint16_le (data + layout->temperature) / 10.0;
			break;
		case DC_FIELD_DIVEMODE:
			if (version == 0x21) {
				switch (data[51]) {
				case OSTC_APNEA:
					*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
					break;
				case OSTC_GAUGE:
					*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
					break;
				case OSTC_ZHL16_OC:
				case OSTC_ZHL16_OC_GF:
					*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
					break;
				case OSTC_ZHL16_CC:
				case OSTC_ZHL16_CC_GF:
					*((dc_divemode_t *) value) = DC_DIVEMODE_CCR;
					break;
				case OSTC_PSCR_GF:
					*((dc_divemode_t *) value) = DC_DIVEMODE_SCR;
					break;
				default:
					return DC_STATUS_DATAFORMAT;
				}
			} else if (version == 0x22) {
				switch (data[51]) {
				case FROG_ZHL16:
				case FROG_ZHL16_GF:
					*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
					break;
				case FROG_APNEA:
					*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
					break;
				default:
					return DC_STATUS_DATAFORMAT;
				}
			} else if (version == 0x23 || version == 0x24) {
				switch (data[82]) {
				case OSTC3_OC:
					*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
					break;
				case OSTC3_CC:
					*((dc_divemode_t *) value) = DC_DIVEMODE_CCR;
					break;
				case OSTC3_GAUGE:
					*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
					break;
				case OSTC3_APNEA:
					*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
					break;
				case OSTC3_PSCR:
					*((dc_divemode_t *) value) = DC_DIVEMODE_SCR;
					break;
				default:
					return DC_STATUS_DATAFORMAT;
				}
			} else {
				return DC_STATUS_UNSUPPORTED;
			}
			break;
		case DC_FIELD_STRING:
			switch(flags) {
			case 0: /* serial */
				string->desc = "Serial";
				snprintf(buf, BUFLEN, "%u", parser->serial);
				break;
			case 1: /* battery */
				string->desc = "Battery at end";
				unsigned int percentage = (unsigned int) data[layout->battery_percentage];
				if (percentage != 0xFF && (version == 0x23 || version == 0x24)) {
					percentage = percentage>100? 100: percentage;
					snprintf(buf, BUFLEN, "%.2fV, %u%% remaining",
					         array_uint16_le (data + layout->battery) / 1000.0,
					         percentage);
				} else {
					snprintf(buf, BUFLEN, "%.2fV", array_uint16_le (data + layout->battery) / 1000.0);
				}
				break;
			case 2: /* desat */
				string->desc = "Desat time";
				snprintf(buf, BUFLEN, "%0u:%02u", array_uint16_le (data + layout->desat) / 60,
						 array_uint16_le (data + layout->desat) % 60);
				break;
			case 3: /* firmware */
				string->desc = "FW Version";
				/* OSTC4 stores firmware as XXXX XYYY YYZZ ZZZB, -> X.Y.Z beta? */
				if (parser->model == OSTC4) {
					int firmwareOnDevice = array_uint16_le (data + layout->firmware);
					unsigned char X = 0, Y = 0, Z = 0, beta = 0;
					X = (firmwareOnDevice & 0xF800) >> 11;
					Y = (firmwareOnDevice & 0x07C0) >> 6;
					Z = (firmwareOnDevice & 0x003E) >> 1;
					beta = firmwareOnDevice & 0x0001;

					snprintf(buf, BUFLEN, "%u.%u.%u%s\n", X, Y, Z, beta? "beta": "");
				} else {
					snprintf(buf, BUFLEN, "%0u.%02u", data[layout->firmware], data[layout->firmware + 1]);
				}
				break;

			case 4: /* Deco model */
				string->desc = "Deco model";
				if (((version == 0x23 || version == 0x24) && data[layout->decomode] == OSTC3_ZHL16) ||
						(version == 0x22 && data[layout->decomode] == FROG_ZHL16) ||
						(version == 0x21 && (data[layout->decomode] == OSTC_ZHL16_OC || data[layout->decomode] == OSTC_ZHL16_CC)))
					strncpy(buf, "ZH-L16", BUFLEN);
				else if (((version == 0x23 || version == 0x24) && data[layout->decomode] == OSTC3_ZHL16_GF) ||
						(version == 0x22 && data[layout->decomode] == FROG_ZHL16_GF) ||
						(version == 0x21 && (data[layout->decomode] == OSTC_ZHL16_OC_GF || data[layout->decomode] == OSTC_ZHL16_CC_GF)))
					strncpy(buf, "ZH-L16-GF", BUFLEN);
				else if (((version == 0x24) && data[layout->decomode] == OSTC4_VPM))
					strncpy(buf, "VPM", BUFLEN);
				else
					return DC_STATUS_DATAFORMAT;
				break;
			case 5: /* Deco model info */
				string->desc = "Deco model info";
				if (((version == 0x23 || version == 0x24) && data[layout->decomode] == OSTC3_ZHL16) ||
						(version == 0x22 && data[layout->decomode] == FROG_ZHL16) ||
						(version == 0x21 && (data[layout->decomode] == OSTC_ZHL16_OC || data[layout->decomode] == OSTC_ZHL16_CC)))
					snprintf(buf, BUFLEN, "Saturation %u, Desaturation %u", layout->deco_info1, layout->deco_info2);
				else if (((version == 0x23 || version == 0x24) && data[layout->decomode] == OSTC3_ZHL16_GF) ||
						(version == 0x22 && data[layout->decomode] == FROG_ZHL16_GF) ||
						(version == 0x21 && (data[layout->decomode] == OSTC_ZHL16_OC_GF || data[layout->decomode] == OSTC_ZHL16_CC_GF)))
					snprintf(buf, BUFLEN, "GF %u/%u", data[layout->deco_info1], data[layout->deco_info2]);
				else
					return DC_STATUS_DATAFORMAT;
				break;
			default:
				return DC_STATUS_UNSUPPORTED;
			}
			string->value = strdup(buf);
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
hw_ostc_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	hw_ostc_parser_t *parser = (hw_ostc_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	// Cache the parser data.
	dc_status_t rc = hw_ostc_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int version = parser->version;
	unsigned int header = parser->header;
	const hw_ostc_layout_t *layout = parser->layout;

	// Exit if no profile data available.
	if (size == header || (size == header + 2 &&
		data[header] == 0xFD && data[header + 1] == 0xFD)) {
		parser->cached = PROFILE;
		return DC_STATUS_SUCCESS;
	}

	// Check the header length.
	if (version == 0x23 || version == 0x24) {
		if (size < header + 5) {
			ERROR (abstract->context, "Buffer overflow detected!");
			return DC_STATUS_DATAFORMAT;
		}
	}

	// Get the sample rate.
	unsigned int samplerate = 0;
	if (version == 0x23 || version == 0x24)
		samplerate = data[header + 3];
	else
		samplerate = data[36];

	// Get the number of sample descriptors.
	unsigned int nconfig = 0;
	if (version == 0x23 || version == 0x24)
		nconfig = data[header + 4];
	else
		nconfig = 6;
	if (nconfig > MAXCONFIG) {
		ERROR(abstract->context, "Too many sample descriptors.");
		return DC_STATUS_DATAFORMAT;
	}

	// Check the header length.
	if (version == 0x23 || version == 0x24) {
		if (size < header + 5 + 3 * nconfig) {
			ERROR (abstract->context, "Buffer overflow detected!");
			return DC_STATUS_DATAFORMAT;
		}
	}

	// Get the extended sample configuration.
	hw_ostc_sample_info_t info[MAXCONFIG] = {{0}};
	for (unsigned int i = 0; i < nconfig; ++i) {
		if (version == 0x23 || version == 0x24) {
			info[i].type    = data[header + 5 + 3 * i + 0];
			info[i].size    = data[header + 5 + 3 * i + 1];
			info[i].divisor = data[header + 5 + 3 * i + 2];
		} else {
			info[i].type    = i;
			info[i].divisor = (data[37 + i] & 0x0F);
			info[i].size    = (data[37 + i] & 0xF0) >> 4;
		}

		if (info[i].divisor) {
			switch (info[i].type) {
			case TEMPERATURE:
			case DECO:
			case TANK:
				if (info[i].size != 2) {
					ERROR(abstract->context, "Unexpected sample size.");
					return DC_STATUS_DATAFORMAT;
				}
				break;
			case PPO2:
				if (info[i].size != 3 && info[i].size != 9) {
					ERROR(abstract->context, "Unexpected sample size.");
					return DC_STATUS_DATAFORMAT;
				}
				break;
			case CNS:
				if (info[i].size != 1 && info[i].size != 2) {
					ERROR(abstract->context, "Unexpected sample size.");
					return DC_STATUS_DATAFORMAT;
				}
				break;
			default: // Not yet used.
				break;
			}
		}
	}

	// Get the firmware version.
	unsigned int firmware = 0;
	if (parser->model == OSTC4) {
		firmware = array_uint16_le (data + layout->firmware);
	} else {
		firmware = array_uint16_be (data + layout->firmware);
	}

	unsigned int time = 0;
	unsigned int nsamples = 0;
	unsigned int tank = parser->initial != UNDEFINED ? parser->initial : 0;

	unsigned int offset = header;
	if (version == 0x23 || version == 0x24)
		offset += 5 + 3 * nconfig;
	while (offset + 3 <= size) {
		dc_sample_value_t sample = {0};

		nsamples++;

		// Time (seconds).
		time += samplerate;
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Initial gas mix.
		if (time == samplerate && parser->initial != UNDEFINED) {
			sample.gasmix = parser->initial;
			if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
		}

		// Initial setpoint (mbar).
		if (time == samplerate && parser->initial_setpoint != UNDEFINED) {
			sample.setpoint = parser->initial_setpoint / 100.0;
			if (callback) callback (DC_SAMPLE_SETPOINT, sample, userdata);
		}

		// Initial CNS (%).
		if (time == samplerate && parser->initial_cns != UNDEFINED) {
			sample.cns = parser->initial_cns / 100.0;
			if (callback) callback (DC_SAMPLE_CNS, sample, userdata);
		}

		// Depth (1/100 m).
		unsigned int depth = array_uint16_le (data + offset);
		sample.depth = depth / 100.0;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);
		offset += 2;

		// Extended sample info.
		unsigned int length =  data[offset] & 0x7F;
		offset += 1;

		// Check for buffer overflows.
		if (offset + length > size) {
			ERROR (abstract->context, "Buffer overflow detected!");
			return DC_STATUS_DATAFORMAT;
		}

		// Get the event byte(s).
		unsigned int nbits = 0;
		unsigned int events = 0;
		while (data[offset - 1] & 0x80) {
			if (nbits && version != 0x23 && version != 0x24)
				break;
			if (length < 1) {
				ERROR (abstract->context, "Buffer overflow detected!");
				return DC_STATUS_DATAFORMAT;
			}
			events |= data[offset] << nbits;
			nbits += 8;
			offset++;
			length--;
		}

		// Alarms
		sample.event.type = 0;
		sample.event.time = 0;
		sample.event.flags = 0;
		sample.event.value = 0;
		switch (events & 0x0F) {
		case 0: // No Alarm
			break;
		case 1: // Slow
			sample.event.type = SAMPLE_EVENT_ASCENT;
			break;
		case 2: // Deco Stop missed
			sample.event.type = SAMPLE_EVENT_CEILING;
			break;
		case 3: // Deep Stop missed
			sample.event.type = SAMPLE_EVENT_CEILING;
			break;
		case 4: // ppO2 Low Warning
			sample.event.type = SAMPLE_EVENT_PO2;
			break;
		case 5: // ppO2 High Warning
			sample.event.type = SAMPLE_EVENT_PO2;
			break;
		case 6: // Manual Marker
			sample.event.type = SAMPLE_EVENT_BOOKMARK;
			break;
		case 7: // Low Battery
			break;
		}
		if (sample.event.type && callback)
			callback (DC_SAMPLE_EVENT, sample, userdata);

		// Manual Gas Set & Change
		if (events & 0x10) {
			if (length < 2) {
				ERROR (abstract->context, "Buffer overflow detected!");
				return DC_STATUS_DATAFORMAT;
			}
			unsigned int o2 = data[offset];
			unsigned int he = data[offset + 1];
			unsigned int idx = hw_ostc_find_gasmix (parser, o2, he, MANUAL);
			if (idx >= parser->ngasmixes) {
				if (idx >= NGASMIXES) {
					ERROR (abstract->context, "Maximum number of gas mixes reached.");
					return DC_STATUS_NOMEMORY;
				}
				parser->gasmix[idx].oxygen = o2;
				parser->gasmix[idx].helium = he;
				parser->ngasmixes = idx + 1;
			}

			sample.gasmix = idx;
			if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
			offset += 2;
			length -= 2;
		}

		// Gas Change
		if (events & 0x20) {
			if (length < 1) {
				ERROR (abstract->context, "Buffer overflow detected!");
				return DC_STATUS_DATAFORMAT;
			}
			unsigned int idx = data[offset];
			if (idx < 1 || idx > parser->ngasmixes) {
				ERROR(abstract->context, "Invalid gas mix.");
				return DC_STATUS_DATAFORMAT;
			}
			idx--; /* Convert to a zero based index. */
			sample.gasmix = idx;
			if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
			tank = idx;
			offset++;
			length--;
		}

		if (version == 0x23 || version == 0x24) {
			// SetPoint Change
			if (events & 0x40) {
				if (length < 1) {
					ERROR (abstract->context, "Buffer overflow detected!");
					return DC_STATUS_DATAFORMAT;
				}
				sample.setpoint = data[offset] / 100.0;
				if (callback) callback (DC_SAMPLE_SETPOINT, sample, userdata);
				offset++;
				length--;
			}

			// Bailout Event
			if (events & 0x0100) {
				if (length < 2) {
					ERROR (abstract->context, "Buffer overflow detected!");
					return DC_STATUS_DATAFORMAT;
				}

				unsigned int o2 = data[offset];
				unsigned int he = data[offset + 1];
				unsigned int idx = hw_ostc_find_gasmix (parser, o2, he, MANUAL);
				if (idx >= parser->ngasmixes) {
					if (idx >= NGASMIXES) {
						ERROR (abstract->context, "Maximum number of gas mixes reached.");
						return DC_STATUS_NOMEMORY;
					}
					parser->gasmix[idx].oxygen = o2;
					parser->gasmix[idx].helium = he;
					parser->ngasmixes = idx + 1;
				}

				sample.gasmix = idx;
				if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
				offset += 2;
				length -= 2;
			}
		}

		// Extended sample info.
		for (unsigned int i = 0; i < nconfig; ++i) {
			if (info[i].divisor && (nsamples % info[i].divisor) == 0) {
				if (length < info[i].size) {
					// Due to a bug in the hwOS Tech firmware v3.03 to v3.08, and
					// the hwOS Sport firmware v10.57 to v10.63, the ppO2 divisor
					// is sometimes not correctly reset to zero when no ppO2
					// samples are being recorded.
					if (info[i].type == PPO2 && parser->hwos && parser->model != OSTC4 &&
						((firmware >= OSTC3FW(3,3) && firmware <= OSTC3FW(3,8)) ||
						(firmware >= OSTC3FW(10,57) && firmware <= OSTC3FW(10,63)))) {
						WARNING (abstract->context, "Reset invalid ppO2 divisor to zero.");
						info[i].divisor = 0;
						continue;
					}
					ERROR (abstract->context, "Buffer overflow detected!");
					return DC_STATUS_DATAFORMAT;
				}

				unsigned int ppo2[3] = {0};
				unsigned int count = 0;
				unsigned int value = 0;
				switch (info[i].type) {
				case TEMPERATURE:
					value = array_uint16_le (data + offset);
					sample.temperature = value / 10.0;
					if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);
					break;
				case DECO:
					// Due to a firmware bug, the deco/ndl info is incorrect for
					// all OSTC4 dives with a firmware older than version 1.0.8.
					if (parser->model == OSTC4 && firmware < OSTC4FW(1,0,8,0))
						break;
					if (data[offset]) {
						sample.deco.type = DC_DECO_DECOSTOP;
						sample.deco.depth = data[offset];
					} else {
						sample.deco.type = DC_DECO_NDL;
						sample.deco.depth = 0.0;
					}
					sample.deco.time = data[offset + 1] * 60;
					if (callback) callback (DC_SAMPLE_DECO, sample, userdata);
					break;
				case PPO2:
					for (unsigned int j = 0; j < 3; ++j) {
						if (info[i].size == 3) {
							ppo2[j] = data[offset + j];
						} else {
							ppo2[j] = data[offset + j * 3];
						}
						if (ppo2[j] != 0)
							count++;
					}
					if (count) {
						for (unsigned int j = 0; j < 3; ++j) {
							sample.ppo2 = ppo2[j] / 100.0;
							if (callback) callback (DC_SAMPLE_PPO2, sample, userdata);
						}
					}
					break;
				case CNS:
					if (info[i].size == 2)
						sample.cns = array_uint16_le (data + offset) / 100.0;
					else
						sample.cns = data[offset] / 100.0;
					if (callback) callback (DC_SAMPLE_CNS, sample, userdata);
					break;
				case TANK:
					value = array_uint16_le (data + offset);
					if (value != 0) {
						sample.pressure.tank = tank;
						sample.pressure.value = value;
						// The hwOS Sport firmware used a resolution of
						// 0.1 bar between versions 10.40 and 10.50.
						if (parser->hwos && parser->model != OSTC4 &&
							(firmware >= OSTC3FW(10,40) && firmware <= OSTC3FW(10,50))) {
							sample.pressure.value /= 10.0;
						}
						if (callback) callback (DC_SAMPLE_PRESSURE, sample, userdata);
					}
					break;
				default: // Not yet used.
					break;
				}

				offset += info[i].size;
				length -= info[i].size;
			}
		}

		if (version != 0x23 && version != 0x24) {
			// SetPoint Change
			if (events & 0x40) {
				if (length < 1) {
					ERROR (abstract->context, "Buffer overflow detected!");
					return DC_STATUS_DATAFORMAT;
				}
				sample.setpoint = data[offset] / 100.0;
				if (callback) callback (DC_SAMPLE_SETPOINT, sample, userdata);
				offset++;
				length--;
			}

			// Bailout Event
			if (events & 0x80) {
				if (length < 2) {
					ERROR (abstract->context, "Buffer overflow detected!");
					return DC_STATUS_DATAFORMAT;
				}

				unsigned int o2 = data[offset];
				unsigned int he = data[offset + 1];
				unsigned int idx = hw_ostc_find_gasmix (parser, o2, he, MANUAL);
				if (idx >= parser->ngasmixes) {
					if (idx >= NGASMIXES) {
						ERROR (abstract->context, "Maximum number of gas mixes reached.");
						return DC_STATUS_NOMEMORY;
					}
					parser->gasmix[idx].oxygen = o2;
					parser->gasmix[idx].helium = he;
					parser->ngasmixes = idx + 1;
				}

				sample.gasmix = idx;
				if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
				offset += 2;
				length -= 2;
			}
		}

		// Skip remaining sample bytes (if any).
		if (length) {
			WARNING (abstract->context, "Remaining %u bytes skipped.", length);
		}
		offset += length;
	}

	if (offset + 2 > size || data[offset] != 0xFD || data[offset + 1] != 0xFD) {
		ERROR (abstract->context, "Invalid end marker found!");
		return DC_STATUS_DATAFORMAT;
	}

	parser->cached = PROFILE;

	return DC_STATUS_SUCCESS;
}
