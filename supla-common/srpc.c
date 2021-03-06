/*
 Copyright (C) AC SOFTWARE SP. Z O.O.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "srpc.h"
#include "proto.h"
#include "log.h"
#include "lck.h"
#include <string.h>
#include <stdlib.h>

#ifdef ESP8266

	#include <osapi.h>
        #ifdef ARDUINO_ARCH_ESP8266
           #include <ets_sys.h>
 	   #define __EH_DISABLED
	#endif
	#include <mem.h>
	
	#define srpc_BUFFER_SIZE      1024
	#define srpc_QUEUE_MAX_SIZE   2

	#include <user_interface.h>
	#include "espmissingincludes.h"

#elif defined(__AVR__)

	#define srpc_BUFFER_SIZE      1024
	#define srpc_QUEUE_MAX_SIZE   2
    #define __EH_DISABLED

#else

	#include <assert.h>
	
	#define srpc_BUFFER_SIZE      32768
	#define srpc_QUEUE_MAX_SIZE   10

#endif

typedef struct {

   void *proto;
   TsrpcParams params;

   TSuplaDataPacket sdp;

   TSuplaDataPacket **in_queue;
   unsigned char in_queue_size;

   TSuplaDataPacket **out_queue;
   unsigned char out_queue_size;

   void *lck;

}Tsrpc;

void SRPC_ICACHE_FLASH  srpc_params_init(TsrpcParams *params) {
    memset(params, 0, sizeof(TsrpcParams));
}

void* SRPC_ICACHE_FLASH srpc_init(TsrpcParams *params) {

	Tsrpc *srpc = (Tsrpc *)malloc(sizeof(Tsrpc));

	if ( srpc == NULL )
		return NULL;

	memset(srpc, 0, sizeof(Tsrpc));
	srpc->proto = sproto_init();

#ifndef ESP8266
#ifndef __AVR__
	assert(params != 0);
	assert(params->data_read != 0);
	assert(params->data_write != 0);
#endif
#endif

	memcpy(&srpc->params, params, sizeof(TsrpcParams));

	srpc->lck = lck_init();

return srpc;
}

void SRPC_ICACHE_FLASH srpc_queue_free(TSuplaDataPacket ***queue, unsigned char *size) {

	_supla_int_t a;
    for(a=0;a<(*size);a++)
    	free((*queue)[a]);

    free(*queue);
    *queue = 0;
    *size = 0;

}

void SRPC_ICACHE_FLASH srpc_free(void *_srpc) {

	if ( _srpc ) {

		Tsrpc *srpc = (Tsrpc*)_srpc;

        sproto_free(srpc->proto);
        srpc_queue_free(&srpc->in_queue, &srpc->in_queue_size);
        srpc_queue_free(&srpc->out_queue, &srpc->out_queue_size);

        lck_free(srpc->lck);

		free(srpc);
	}

}

char SRPC_ICACHE_FLASH srpc_queue_push(TSuplaDataPacket ***queue, unsigned char *size, TSuplaDataPacket *sdp) {

	if ( *size >= srpc_QUEUE_MAX_SIZE )
		return SUPLA_RESULT_FALSE;

	TSuplaDataPacket *sdp_new = (TSuplaDataPacket *)malloc(sizeof(TSuplaDataPacket));

	if ( sdp_new == 0 )
		return SUPLA_RESULT_FALSE;

	(*size)++;

    TSuplaDataPacket **queue_new = (TSuplaDataPacket **)realloc(*queue, sizeof(TSuplaDataPacket *)*(*size));

	if ( queue_new == 0 ) {
		(*size)--;
		free(sdp_new);
		return SUPLA_RESULT_FALSE;
	} else {
		*queue = queue_new;
	}

	memcpy(sdp_new, sdp, sizeof(TSuplaDataPacket));

	(*queue)[(*size)-1] = sdp_new;

	return SUPLA_RESULT_TRUE;
}

char SRPC_ICACHE_FLASH srpc_queue_pop(TSuplaDataPacket ***queue, unsigned char *size, TSuplaDataPacket *sdp, unsigned _supla_int_t rr_id) {

	_supla_int_t a, b;

	for(a=0;a<(*size);a++)
		if ( rr_id == 0 || ((*queue)[a])->rr_id == rr_id ) {

			memcpy(sdp, (*queue)[a], sizeof(TSuplaDataPacket));

			if ( *size == 1 ) {
				srpc_queue_free(queue, size);
			} else {

				free((*queue)[a]);
				(*queue)[a] = NULL;

                for(b=a;b<((*size)-1);b++)
                	(*queue)[b] = (*queue)[b+1];

                // before "--" size is always > 1
                (*size)--;

                TSuplaDataPacket **queue_new = (TSuplaDataPacket **)realloc(*queue, sizeof(TSuplaDataPacket *)*(*size));

            	if ( *queue_new == 0 ) {
            		return SUPLA_RESULT_FALSE;
            	} else {
                    *queue = queue_new;
            	}
			}

			return SUPLA_RESULT_TRUE;
		}

	return SUPLA_RESULT_FALSE;
}

char SRPC_ICACHE_FLASH srpc_in_queue_push(Tsrpc *srpc, TSuplaDataPacket *sdp) {
	return srpc_queue_push(&srpc->in_queue, &srpc->in_queue_size, sdp);
}

char SRPC_ICACHE_FLASH srpc_in_queue_pop(Tsrpc *srpc, TSuplaDataPacket *sdp, unsigned _supla_int_t rr_id) {

	return srpc_queue_pop(&srpc->in_queue, &srpc->in_queue_size, sdp, rr_id);
}

char SRPC_ICACHE_FLASH srpc_out_queue_push(Tsrpc *srpc, TSuplaDataPacket *sdp) {
	return srpc_queue_push(&srpc->out_queue, &srpc->out_queue_size, sdp);
}

char SRPC_ICACHE_FLASH srpc_out_queue_pop(Tsrpc *srpc, TSuplaDataPacket *sdp, unsigned _supla_int_t rr_id) {
	return srpc_queue_pop(&srpc->out_queue, &srpc->out_queue_size, sdp, rr_id);
}

char SRPC_ICACHE_FLASH srpc_iterate(void *_srpc) {

	Tsrpc *srpc = (Tsrpc*)_srpc;
	char data_buffer[srpc_BUFFER_SIZE];
	char result;
	unsigned char version;

	// --------- IN ---------------
	_supla_int_t data_size = srpc->params.data_read(data_buffer, srpc_BUFFER_SIZE, srpc->params.user_params);

	if ( data_size == 0 )
		return SUPLA_RESULT_FALSE;


	lck_lock(srpc->lck);

    if ( data_size > 0
    	 && SUPLA_RESULT_TRUE != ( result = sproto_in_buffer_append(srpc->proto, data_buffer, data_size) ) ) {

    	supla_log(LOG_DEBUG, "sproto_in_buffer_append: %i, datasize: %i", result, data_size);
    	return lck_unlock_r(srpc->lck, SUPLA_RESULT_FALSE);
    }

    if ( SUPLA_RESULT_TRUE == (result = sproto_pop_in_sdp(srpc->proto, &srpc->sdp) ) ) {

    	if ( SUPLA_RESULT_TRUE == srpc_in_queue_push(srpc, &srpc->sdp) ) {

    		if ( srpc->params.on_remote_call_received ) {

    			lck_unlock(srpc->lck);
    			srpc->params.on_remote_call_received(srpc, srpc->sdp.rr_id, srpc->sdp.call_type, srpc->params.user_params, srpc->sdp.version);
    			lck_lock(srpc->lck);
    		}

    	} else {
        	supla_log(LOG_DEBUG, "ssrpc_in_queue_push error");
        	return lck_unlock_r(srpc->lck, SUPLA_RESULT_FALSE);
    	}


    } else if ( result != SUPLA_RESULT_FALSE ) {

    	if ( result == (char)SUPLA_RESULT_VERSION_ERROR ) {

    		if ( srpc->params.on_version_error ) {

    			version = srpc->sdp.version;
    			lck_unlock(srpc->lck);

    			srpc->params.on_version_error(srpc, version, srpc->params.user_params);
    			return SUPLA_RESULT_FALSE;
    		}
    	} else {
    		supla_log(LOG_DEBUG, "sproto_pop_in_sdp error: %i", result);
    	}

    	return lck_unlock_r(srpc->lck, SUPLA_RESULT_FALSE);
    }

    // --------- OUT ---------------


    if ( srpc_out_queue_pop(srpc, &srpc->sdp, 0) == SUPLA_RESULT_TRUE
    	 && SUPLA_RESULT_TRUE != (result = sproto_out_buffer_append(srpc->proto, &srpc->sdp) )
    	 && result != SUPLA_RESULT_FALSE ) {

    		supla_log(LOG_DEBUG, "sproto_out_buffer_append error: %i", result);
    		return lck_unlock_r(srpc->lck, SUPLA_RESULT_FALSE);
    	}


    data_size = sproto_pop_out_data(srpc->proto, data_buffer, srpc_BUFFER_SIZE);

    if ( data_size != 0 ) {
    	lck_unlock(srpc->lck);
    	srpc->params.data_write(data_buffer, data_size, srpc->params.user_params);
    	lck_lock(srpc->lck);

        #ifndef __EH_DISABLED
    	if ( srpc->params.eh != 0
    		 && sproto_out_dataexists(srpc->proto) == 1 ) {

    		eh_raise_event(srpc->params.eh);
    	}
        #endif

    }


    return lck_unlock_r(srpc->lck, SUPLA_RESULT_TRUE);
}

typedef unsigned _supla_int_t (*_func_srpc_pack_get_caption_size)(void *pack, _supla_int_t idx);
typedef void* (*_func_srpc_pack_get_item_ptr)(void *pack, _supla_int_t idx);
typedef _supla_int_t (*_func_srpc_pack_get_pack_count)(void *pack);
typedef void (*_func_srpc_pack_set_pack_count)(void *pack, _supla_int_t count, unsigned char increment);
typedef unsigned _supla_int_t (*_func_srpc_pack_get_item_caption_size)(void *item);
typedef unsigned _supla_int_t (*_func_srpc_pack_get_caption_size)(void *pack, _supla_int_t idx);

void SRPC_ICACHE_FLASH srpc_getpack(Tsrpc *srpc, TsrpcReceivedData *rd, unsigned _supla_int_t pack_sizeof
		, unsigned _supla_int_t item_sizeof, unsigned _supla_int_t pack_max_count, unsigned _supla_int_t caption_max_size,
		_func_srpc_pack_get_pack_count pack_get_count,
		_func_srpc_pack_set_pack_count pack_set_count,
		_func_srpc_pack_get_item_ptr get_item_ptr,
		_func_srpc_pack_get_item_caption_size get_item_caption_size) {

	_supla_int_t header_size = pack_sizeof-(item_sizeof*pack_max_count);
	_supla_int_t c_header_size = item_sizeof-caption_max_size;
	_supla_int_t a, count, size, offset, pack_size;
	void *pack = NULL;

	if ( srpc->sdp.data_size < header_size
		 || srpc->sdp.data_size > pack_sizeof ) {
		return;
	};

	count = pack_get_count(srpc->sdp.data);

    if ( count < 0 || count > pack_max_count ) {
    	return;
    };

	pack_size = header_size+(item_sizeof*count);
	pack = (TSC_SuplaChannelPack *)malloc(pack_size);

	if ( pack == NULL )
		return;

	memset(pack, 0, pack_size);
	memcpy(pack, srpc->sdp.data, header_size);

	offset = header_size;
	pack_set_count(pack, 0, 0);

	for(a=0;a<count;a++)
		if ( srpc->sdp.data_size-offset >= c_header_size ) {

			size = get_item_caption_size(&srpc->sdp.data[offset]);

			if ( size >= 0
				 && size <= caption_max_size
				 && srpc->sdp.data_size-offset >= c_header_size+size ) {

				memcpy(get_item_ptr(pack, a), &srpc->sdp.data[offset], c_header_size+size);
				offset += c_header_size+size;
				pack_set_count(pack, 1, 1);

			} else {
				break;
			}
    	}

	if ( count == pack_get_count(pack) ) {

		srpc->sdp.data_size = 0;
		// dcs_ping is 1st variable in union
		rd->data.dcs_ping = pack;

	} else {
		free(pack);
	}
}


void* srpc_channelpack_get_item_ptr(void *pack, _supla_int_t idx) {
	return &((TSC_SuplaChannelPack *)pack)->channels[idx];
};

_supla_int_t srpc_channelpack_get_pack_count(void *pack) {
	return ((TSC_SuplaChannelPack*)pack)->count;
}

void srpc_channelpack_set_pack_count(void *pack, _supla_int_t count, unsigned char increment) {
	if ( increment == 0 ) { ((TSC_SuplaChannelPack *)pack)->count = count; } else { ((TSC_SuplaChannelPack *)pack)->count+=count; };
}

unsigned _supla_int_t srpc_channelpack_get_item_caption_size(void *item) {
	return ((TSC_SuplaChannel*)item)->CaptionSize;
}


void SRPC_ICACHE_FLASH srpc_getchannelpack(Tsrpc *srpc, TsrpcReceivedData *rd) {

	srpc_getpack(srpc, rd, sizeof(TSC_SuplaChannelPack)
			, sizeof(TSC_SuplaChannel), SUPLA_CHANNELPACK_MAXCOUNT, SUPLA_CHANNEL_CAPTION_MAXSIZE,
			&srpc_channelpack_get_pack_count,
			&srpc_channelpack_set_pack_count,
			&srpc_channelpack_get_item_ptr,
			&srpc_channelpack_get_item_caption_size);

}

void* srpc_channelpack_get_item_ptr_b(void *pack, _supla_int_t idx) {
	return &((TSC_SuplaChannelPack_B *)pack)->channels[idx];
};

_supla_int_t srpc_channelpack_get_pack_count_b(void *pack) {
	return ((TSC_SuplaChannelPack_B*)pack)->count;
}

void srpc_channelpack_set_pack_count_b(void *pack, _supla_int_t count, unsigned char increment) {
	if ( increment == 0 ) { ((TSC_SuplaChannelPack_B *)pack)->count = count; } else { ((TSC_SuplaChannelPack_B *)pack)->count+=count; };
}

unsigned _supla_int_t srpc_channelpack_get_item_caption_size_b(void *item) {
	return ((TSC_SuplaChannel_B*)item)->CaptionSize;
}

void SRPC_ICACHE_FLASH srpc_getchannelpack_b(Tsrpc *srpc, TsrpcReceivedData *rd) {

	srpc_getpack(srpc, rd, sizeof(TSC_SuplaChannelPack_B)
			, sizeof(TSC_SuplaChannel_B), SUPLA_CHANNELPACK_MAXCOUNT, SUPLA_CHANNEL_CAPTION_MAXSIZE,
			&srpc_channelpack_get_pack_count_b,
			&srpc_channelpack_set_pack_count_b,
			&srpc_channelpack_get_item_ptr_b,
			&srpc_channelpack_get_item_caption_size_b);

}

void* srpc_locationpack_get_item_ptr(void *pack, _supla_int_t idx) {
	return &((TSC_SuplaLocationPack *)pack)->locations[idx];
};

_supla_int_t srpc_locationpack_get_pack_count(void *pack) {
	return ((TSC_SuplaLocationPack*)pack)->count;
}

void srpc_locationpack_set_pack_count(void *pack, _supla_int_t count, unsigned char increment) {
	if ( increment == 0 ) { ((TSC_SuplaLocationPack *)pack)->count = count; } else { ((TSC_SuplaLocationPack *)pack)->count+=count; };
}

unsigned _supla_int_t srpc_locationpack_get_item_caption_size(void *item) {
	return ((TSC_SuplaLocation*)item)->CaptionSize;
}

void SRPC_ICACHE_FLASH srpc_getlocationpack(Tsrpc *srpc, TsrpcReceivedData *rd) {

	srpc_getpack(srpc, rd, sizeof(TSC_SuplaLocationPack)
			, sizeof(TSC_SuplaLocation), SUPLA_LOCATIONPACK_MAXCOUNT, SUPLA_LOCATION_CAPTION_MAXSIZE,
			&srpc_locationpack_get_pack_count,
			&srpc_locationpack_set_pack_count,
			&srpc_locationpack_get_item_ptr,
			&srpc_locationpack_get_item_caption_size);

}

char SRPC_ICACHE_FLASH srpc_getdata(void *_srpc, TsrpcReceivedData *rd, unsigned _supla_int_t rr_id) {

	Tsrpc *srpc = (Tsrpc*)_srpc;
	char call_with_no_data = 0;
	rd->call_type = 0;

	lck_lock(srpc->lck);

	if ( SUPLA_RESULT_TRUE == srpc_in_queue_pop(srpc, &srpc->sdp, rr_id) ) {

		rd->call_type = srpc->sdp.call_type;
		rd->rr_id = srpc->sdp.rr_id;

		// first one
		rd->data.dcs_ping = NULL;

		switch(srpc->sdp.call_type) {

		   case SUPLA_DCS_CALL_GETVERSION:
		   case SUPLA_CS_CALL_GET_NEXT:
		   case SUPLA_DCS_CALL_GET_REGISTRATION_ENABLED:
			   call_with_no_data = 1;
			   break;

		   case SUPLA_SDC_CALL_GETVERSION_RESULT:

			   if ( srpc->sdp.data_size == sizeof(TSDC_SuplaGetVersionResult) )
                 rd->data.sdc_getversion_result = (TSDC_SuplaGetVersionResult *)malloc(sizeof(TSDC_SuplaGetVersionResult));

			   break;

	       case SUPLA_SDC_CALL_VERSIONERROR:

			   if ( srpc->sdp.data_size == sizeof(TSDC_SuplaVersionError) )
                 rd->data.sdc_version_error = (TSDC_SuplaVersionError*)malloc(sizeof(TSDC_SuplaVersionError));

			   break;

		   case SUPLA_DCS_CALL_PING_SERVER:

			   if ( srpc->sdp.data_size == sizeof(TDCS_SuplaPingServer) )
                  rd->data.dcs_ping = (TDCS_SuplaPingServer*)malloc(sizeof(TDCS_SuplaPingServer));

			   break;

		   case SUPLA_SDC_CALL_PING_SERVER_RESULT:

			   if ( srpc->sdp.data_size == sizeof(TSDC_SuplaPingServerResult) )
                  rd->data.sdc_ping_result = (TSDC_SuplaPingServerResult*)malloc(sizeof(TSDC_SuplaPingServerResult));

			   break;

		   case SUPLA_DCS_CALL_SET_ACTIVITY_TIMEOUT:

			   if ( srpc->sdp.data_size == sizeof(TDCS_SuplaSetActivityTimeout) )
                  rd->data.dcs_set_activity_timeout = (TDCS_SuplaSetActivityTimeout*)malloc(sizeof(TDCS_SuplaSetActivityTimeout));

			   break;

		   case SUPLA_SDC_CALL_SET_ACTIVITY_TIMEOUT_RESULT:

			   if ( srpc->sdp.data_size == sizeof(TSDC_SuplaSetActivityTimeoutResult) )
                  rd->data.sdc_set_activity_timeout_result = (TSDC_SuplaSetActivityTimeoutResult*)malloc(sizeof(TSDC_SuplaSetActivityTimeoutResult));

			   break;

		   case SUPLA_SDC_CALL_GET_REGISTRATION_ENABLED_RESULT:

			   if ( srpc->sdp.data_size == sizeof(TSDC_RegistrationEnabled) )
                  rd->data.sdc_reg_enabled = (TSDC_RegistrationEnabled*)malloc(sizeof(TSDC_RegistrationEnabled));

			   break;

		   case SUPLA_DS_CALL_REGISTER_DEVICE:

			   if ( srpc->sdp.data_size >= (sizeof(TDS_SuplaRegisterDevice)-(sizeof(TDS_SuplaDeviceChannel)*SUPLA_CHANNELMAXCOUNT))
					&& srpc->sdp.data_size <= sizeof(TDS_SuplaRegisterDevice) ) {

				   rd->data.ds_register_device = (TDS_SuplaRegisterDevice*)malloc(sizeof(TDS_SuplaRegisterDevice));
			   }

			   break;

		   case SUPLA_DS_CALL_REGISTER_DEVICE_B: // ver. >= 2
			   
			   if ( srpc->sdp.data_size >= (sizeof(TDS_SuplaRegisterDevice_B)-(sizeof(TDS_SuplaDeviceChannel_B)*SUPLA_CHANNELMAXCOUNT))
					&& srpc->sdp.data_size <= sizeof(TDS_SuplaRegisterDevice_B) ) {

				   rd->data.ds_register_device_b = (TDS_SuplaRegisterDevice_B*)malloc(sizeof(TDS_SuplaRegisterDevice_B));
				   
			   }

			   break;

		   case SUPLA_DS_CALL_REGISTER_DEVICE_C: // ver. >= 6

			   if ( srpc->sdp.data_size >= (sizeof(TDS_SuplaRegisterDevice_C)-(sizeof(TDS_SuplaDeviceChannel_B)*SUPLA_CHANNELMAXCOUNT))
					&& srpc->sdp.data_size <= sizeof(TDS_SuplaRegisterDevice_C) ) {

				   rd->data.ds_register_device_c = (TDS_SuplaRegisterDevice_C*)malloc(sizeof(TDS_SuplaRegisterDevice_C));

			   }

			   break;

		   case SUPLA_DS_CALL_REGISTER_DEVICE_D: // ver. >= 7

			   if ( srpc->sdp.data_size >= (sizeof(TDS_SuplaRegisterDevice_D)-(sizeof(TDS_SuplaDeviceChannel_B)*SUPLA_CHANNELMAXCOUNT))
					&& srpc->sdp.data_size <= sizeof(TDS_SuplaRegisterDevice_D) ) {

				   rd->data.ds_register_device_d = (TDS_SuplaRegisterDevice_D*)malloc(sizeof(TDS_SuplaRegisterDevice_D));

			   }

			   break;

		   case SUPLA_SD_CALL_REGISTER_DEVICE_RESULT:

			   if ( srpc->sdp.data_size == sizeof(TSD_SuplaRegisterDeviceResult) )
                  rd->data.sd_register_device_result = (TSD_SuplaRegisterDeviceResult*)malloc(sizeof(TSD_SuplaRegisterDeviceResult));
			   break;

		   case SUPLA_CS_CALL_REGISTER_CLIENT:

			   if ( srpc->sdp.data_size == sizeof(TCS_SuplaRegisterClient) )
				   rd->data.cs_register_client = (TCS_SuplaRegisterClient*)malloc(sizeof(TCS_SuplaRegisterClient));

			   break;

		   case SUPLA_CS_CALL_REGISTER_CLIENT_B: // ver. >= 6

			   if ( srpc->sdp.data_size == sizeof(TCS_SuplaRegisterClient_B) )
				   rd->data.cs_register_client_b = (TCS_SuplaRegisterClient_B*)malloc(sizeof(TCS_SuplaRegisterClient_B));

			   break;

		   case SUPLA_CS_CALL_REGISTER_CLIENT_C: // ver. >= 7

			   if ( srpc->sdp.data_size == sizeof(TCS_SuplaRegisterClient_C) )
				   rd->data.cs_register_client_c = (TCS_SuplaRegisterClient_C*)malloc(sizeof(TCS_SuplaRegisterClient_C));

			   break;

		   case SUPLA_SC_CALL_REGISTER_CLIENT_RESULT:

			   if ( srpc->sdp.data_size == sizeof(TSC_SuplaRegisterClientResult) )
                  rd->data.sc_register_client_result = (TSC_SuplaRegisterClientResult*)malloc(sizeof(TSC_SuplaRegisterClientResult));

			   break;

		   case SUPLA_DS_CALL_DEVICE_CHANNEL_VALUE_CHANGED:

			   if ( srpc->sdp.data_size == sizeof(TDS_SuplaDeviceChannelValue) )
				   rd->data.ds_device_channel_value = (TDS_SuplaDeviceChannelValue*)malloc(sizeof(TDS_SuplaDeviceChannelValue));

			   break;

		   case SUPLA_SD_CALL_CHANNEL_SET_VALUE:

			   if ( srpc->sdp.data_size == sizeof(TSD_SuplaChannelNewValue) )
				   rd->data.sd_channel_new_value = (TSD_SuplaChannelNewValue*)malloc(sizeof(TSD_SuplaChannelNewValue));

			   break;

		   case SUPLA_DS_CALL_CHANNEL_SET_VALUE_RESULT:

			   if ( srpc->sdp.data_size == sizeof(TDS_SuplaChannelNewValueResult) )
				   rd->data.ds_channel_new_value_result = (TDS_SuplaChannelNewValueResult*)malloc(sizeof(TDS_SuplaChannelNewValueResult));

			   break;

		   case SUPLA_SC_CALL_LOCATION_UPDATE:

			   if ( srpc->sdp.data_size >= (sizeof(TSC_SuplaLocation)-SUPLA_LOCATION_CAPTION_MAXSIZE)
					&& srpc->sdp.data_size <= sizeof(TSC_SuplaLocation) ) {
				   rd->data.sc_location = (TSC_SuplaLocation*)malloc(sizeof(TSC_SuplaLocation));
			   }

			   break;

		   case SUPLA_SC_CALL_LOCATIONPACK_UPDATE:
			   srpc_getlocationpack(srpc, rd);
			   break;

		   case SUPLA_SC_CALL_CHANNEL_UPDATE:

			   if ( srpc->sdp.data_size >= (sizeof(TSC_SuplaChannel)-SUPLA_CHANNEL_CAPTION_MAXSIZE)
					&& srpc->sdp.data_size <= sizeof(TSC_SuplaChannel) ) {
				   rd->data.sc_channel = (TSC_SuplaChannel*)malloc(sizeof(TSC_SuplaChannel));
			   }

			   break;

		   case SUPLA_SC_CALL_CHANNEL_UPDATE_B:

			   if ( srpc->sdp.data_size >= (sizeof(TSC_SuplaChannel_B)-SUPLA_CHANNEL_CAPTION_MAXSIZE)
					&& srpc->sdp.data_size <= sizeof(TSC_SuplaChannel_B) ) {
				   rd->data.sc_channel_b = (TSC_SuplaChannel_B*)malloc(sizeof(TSC_SuplaChannel_B));
			   }

			   break;

		   case SUPLA_SC_CALL_CHANNELPACK_UPDATE:
			   srpc_getchannelpack(srpc, rd);
			   break;

		   case SUPLA_SC_CALL_CHANNELPACK_UPDATE_B:
			   srpc_getchannelpack_b(srpc, rd);
			   break;

		   case SUPLA_SC_CALL_CHANNEL_VALUE_UPDATE:

			   if ( srpc->sdp.data_size == sizeof(TSC_SuplaChannelValue) )
				   rd->data.sc_channel_value = (TSC_SuplaChannelValue*)malloc(sizeof(TSC_SuplaChannelValue));

			   break;

		   case SUPLA_CS_CALL_CHANNEL_SET_VALUE:

			   if ( srpc->sdp.data_size == sizeof(TCS_SuplaChannelNewValue) )
				   rd->data.cs_channel_new_value = (TCS_SuplaChannelNewValue*)malloc(sizeof(TCS_SuplaChannelNewValue));

			   break;

		   case SUPLA_CS_CALL_CHANNEL_SET_VALUE_B:

			   if ( srpc->sdp.data_size == sizeof(TCS_SuplaChannelNewValue_B) )
				   rd->data.cs_channel_new_value_b = (TCS_SuplaChannelNewValue_B*)malloc(sizeof(TCS_SuplaChannelNewValue_B));

			   break;

		   case SUPLA_SC_CALL_EVENT:

			   if ( srpc->sdp.data_size >= (sizeof(TSC_SuplaEvent)-SUPLA_SENDER_NAME_MAXSIZE)
					&& srpc->sdp.data_size <= sizeof(TSC_SuplaEvent) ) {
				   rd->data.sc_event = (TSC_SuplaEvent*)malloc(sizeof(TSC_SuplaEvent));
			   }

			   break;

		   case SUPLA_DS_CALL_GET_FIRMWARE_UPDATE_URL:

			   if ( srpc->sdp.data_size == sizeof(TDS_FirmwareUpdateParams) )
				   rd->data.ds_firmware_update_params = (TDS_FirmwareUpdateParams*)malloc(sizeof(TDS_FirmwareUpdateParams));

			   break;



		   case SUPLA_SD_CALL_GET_FIRMWARE_UPDATE_URL_RESULT:

			   if ( srpc->sdp.data_size == sizeof(TSD_FirmwareUpdate_UrlResult)
				    || srpc->sdp.data_size == sizeof(char) ) {

				   rd->data.sc_firmware_update_url_result = (TSD_FirmwareUpdate_UrlResult*)malloc(sizeof(TSD_FirmwareUpdate_UrlResult));

				   if ( srpc->sdp.data_size == sizeof(char)
						&& rd->data.sc_firmware_update_url_result != NULL )
					   memset(rd->data.sc_firmware_update_url_result, 0, sizeof(TSD_FirmwareUpdate_UrlResult));
			   }


			   break;

		   case SUPLA_CS_CALL_GET_OAUTH_PARAMETERS:

			   if ( srpc->sdp.data_size == sizeof(TCS_OAuthParametersRequest) )
				   rd->data.cs_oauth_parameters_request = (TCS_OAuthParametersRequest*)malloc(sizeof(TCS_OAuthParametersRequest));

			   break;

		   case SUPLA_SC_CALL_GET_OAUTH_PARAMETERS_RESULT:

			   if ( srpc->sdp.data_size == sizeof(TSC_OAuthParameters) )
				   rd->data.sc_oauth_parameters = (TSC_OAuthParameters*)malloc(sizeof(TSC_OAuthParameters));

			   break;
		}

		if ( call_with_no_data == 1 ) {
			return lck_unlock_r(srpc->lck, SUPLA_RESULT_TRUE);
		}

		if ( rd->data.dcs_ping != NULL ) {

			if ( srpc->sdp.data_size > 0 )
				memcpy(rd->data.dcs_ping, srpc->sdp.data, srpc->sdp.data_size);

			return lck_unlock_r(srpc->lck, SUPLA_RESULT_TRUE);
		}


		return lck_unlock_r(srpc->lck, SUPLA_RESULT_DATA_ERROR);
	}

	return lck_unlock_r(srpc->lck, SUPLA_RESULT_FALSE);
}

void SRPC_ICACHE_FLASH srpc_rd_free(TsrpcReceivedData *rd) {
    if ( rd->call_type > 0 ) {
    	// first one

    	if ( rd->data.dcs_ping != NULL )
    	  free(rd->data.dcs_ping);

    	rd->call_type = 0;
    }
}

unsigned char SRPC_ICACHE_FLASH srpc_call_min_version_required(void *_srpc, unsigned _supla_int_t call_type) {

	switch(call_type) {


		case SUPLA_DS_CALL_REGISTER_DEVICE_B:
		case SUPLA_DCS_CALL_SET_ACTIVITY_TIMEOUT:
		case SUPLA_SDC_CALL_SET_ACTIVITY_TIMEOUT_RESULT:
		return 2;

		case SUPLA_CS_CALL_CHANNEL_SET_VALUE_B:
		return 3;

		case SUPLA_DS_CALL_GET_FIRMWARE_UPDATE_URL:
		case SUPLA_SD_CALL_GET_FIRMWARE_UPDATE_URL_RESULT:
		return 5;

		case SUPLA_DS_CALL_REGISTER_DEVICE_C:
		case SUPLA_CS_CALL_REGISTER_CLIENT_B:
		return 6;

		case SUPLA_CS_CALL_REGISTER_CLIENT_C:
		case SUPLA_DS_CALL_REGISTER_DEVICE_D:
		case SUPLA_DCS_CALL_GET_REGISTRATION_ENABLED:
		case SUPLA_SDC_CALL_GET_REGISTRATION_ENABLED_RESULT:
		case SUPLA_CS_CALL_GET_OAUTH_PARAMETERS:
		case SUPLA_SC_CALL_GET_OAUTH_PARAMETERS_RESULT:
		return 7;

		case SUPLA_SC_CALL_CHANNELPACK_UPDATE_B:
		case SUPLA_SC_CALL_CHANNEL_UPDATE_B:
		return 8;

	}

	return 0;
}

unsigned char SRPC_ICACHE_FLASH srpc_call_allowed(void *_srpc, unsigned _supla_int_t call_type) {

	unsigned char min_ver = srpc_call_min_version_required(_srpc, call_type);

	if ( min_ver == 0 || srpc_get_proto_version(_srpc) >= min_ver ) {
		return 1;
	}

	return 0;
}

_supla_int_t SRPC_ICACHE_FLASH srpc_async__call(void *_srpc, unsigned _supla_int_t call_type, char *data, unsigned _supla_int_t data_size, unsigned char *version) {

	Tsrpc *srpc = (Tsrpc*)_srpc;

	if ( !srpc_call_allowed(_srpc, call_type) ) {

		if ( srpc->params.on_min_version_required != NULL ) {
			srpc->params.on_min_version_required(_srpc, call_type, srpc_call_min_version_required(_srpc, call_type), srpc->params.user_params);
		}

		return SUPLA_RESULT_CALL_NOT_ALLOWED;
	}

	if ( srpc->params.before_async_call != NULL ) {
		srpc->params.before_async_call(_srpc, call_type, srpc->params.user_params);
	};

	lck_lock(srpc->lck);

	sproto_sdp_init(srpc->proto, &srpc->sdp);

	if ( version != NULL )
		srpc->sdp.version = *version;

	if ( SUPLA_RESULT_TRUE == sproto_set_data(&srpc->sdp, data, data_size, call_type)
		 && srpc_out_queue_push(srpc, &srpc->sdp) ) {

        #ifndef __EH_DISABLED
    	if ( srpc->params.eh != 0 ) {
    		eh_raise_event(srpc->params.eh);
    	}
        #endif

    	return lck_unlock_r(srpc->lck, srpc->sdp.rr_id);
	}

	return lck_unlock_r(srpc->lck, SUPLA_RESULT_FALSE);
}

_supla_int_t SRPC_ICACHE_FLASH srpc_async_call(void *_srpc, unsigned _supla_int_t call_type, char *data, unsigned _supla_int_t data_size) {

	return srpc_async__call(_srpc, call_type, data, data_size, NULL);
}

unsigned char SRPC_ICACHE_FLASH srpc_get_proto_version(void *_srpc) {

	unsigned char version;

	Tsrpc *srpc = (Tsrpc*)_srpc;
	lck_lock(srpc->lck);
	version = sproto_get_version(srpc->proto);
	lck_unlock(srpc->lck);

	return version;
}


void SRPC_ICACHE_FLASH srpc_set_proto_version(void *_srpc, unsigned char version) {

	Tsrpc *srpc = (Tsrpc*)_srpc;

	lck_lock(srpc->lck);
	sproto_set_version(srpc->proto, version);
	lck_unlock(srpc->lck);
}

_supla_int_t SRPC_ICACHE_FLASH srpc_dcs_async_getversion(void *_srpc) {
	return srpc_async_call(_srpc, SUPLA_DCS_CALL_GETVERSION, NULL, 0);
}

_supla_int_t SRPC_ICACHE_FLASH srpc_sdc_async_getversion_result(void *_srpc, char *SoftVer) {

	TSDC_SuplaGetVersionResult gvr;

	gvr.proto_version = SUPLA_PROTO_VERSION;
	gvr.proto_version_min = SUPLA_PROTO_VERSION_MIN;

	memcpy(gvr.SoftVer, SoftVer, SUPLA_SOFTVER_MAXSIZE);

	return srpc_async_call(_srpc, SUPLA_SDC_CALL_GETVERSION_RESULT, (char*)&gvr, sizeof(TSDC_SuplaGetVersionResult));

}

_supla_int_t SRPC_ICACHE_FLASH srpc_sdc_async_versionerror(void *_srpc, unsigned char remote_version) {
	TSDC_SuplaVersionError ve;
	ve.server_version = SUPLA_PROTO_VERSION;
	ve.server_version_min = SUPLA_PROTO_VERSION_MIN;

	return srpc_async__call(_srpc, SUPLA_SDC_CALL_VERSIONERROR, (char*)&ve, sizeof(TSDC_SuplaVersionError), &remote_version);
}

_supla_int_t SRPC_ICACHE_FLASH srpc_dcs_async_ping_server(void *_srpc) {
	TDCS_SuplaPingServer ps;

    #if defined(ESP8266) 
	ps.now.tv_sec = 0;
	ps.now.tv_usec = 0;
    #elif defined(__AVR__) 
	ps.now.tv_sec[0] = 0;
	ps.now.tv_sec[1] = 0;
	ps.now.tv_usec[0] = 0;
	ps.now.tv_usec[1] = 0;
    #else
	gettimeofday(&ps.now, NULL);
    #endif

    return srpc_async_call(_srpc, SUPLA_DCS_CALL_PING_SERVER, (char*)&ps, sizeof(TDCS_SuplaPingServer));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_sdc_async_ping_server_result(void *_srpc) {
	TSDC_SuplaPingServerResult ps;

	#ifdef ESP8266
	ps.now.tv_sec = 0;
	ps.now.tv_usec = 0;
	#else
	gettimeofday(&ps.now, NULL);
	#endif

	return srpc_async_call(_srpc, SUPLA_SDC_CALL_PING_SERVER_RESULT, (char*)&ps, sizeof(TSDC_SuplaPingServerResult));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_dcs_async_set_activity_timeout(void *_srpc, TDCS_SuplaSetActivityTimeout *dcs_set_activity_timeout) {
	return srpc_async_call(_srpc, SUPLA_DCS_CALL_SET_ACTIVITY_TIMEOUT, (char*)dcs_set_activity_timeout, sizeof(TDCS_SuplaSetActivityTimeout));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_dcs_async_set_activity_timeout_result(void *_srpc, TSDC_SuplaSetActivityTimeoutResult *sdc_set_activity_timeout_result) {
	return srpc_async_call(_srpc, SUPLA_SDC_CALL_SET_ACTIVITY_TIMEOUT_RESULT, (char*)sdc_set_activity_timeout_result, sizeof(TSDC_SuplaSetActivityTimeoutResult));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_dcs_async_get_registration_enabled(void *_srpc) {

	return srpc_async_call(_srpc, SUPLA_DCS_CALL_GET_REGISTRATION_ENABLED, NULL, 0);

}

_supla_int_t SRPC_ICACHE_FLASH srpc_sdc_async_get_registration_enabled_result(void *_srpc, TSDC_RegistrationEnabled *reg_enabled) {

	return srpc_async_call(_srpc, SUPLA_SDC_CALL_GET_REGISTRATION_ENABLED_RESULT, (char*)reg_enabled, sizeof(TSDC_RegistrationEnabled));

}

_supla_int_t SRPC_ICACHE_FLASH srpc_sd_async_get_firmware_update_url(void *_srpc, TDS_FirmwareUpdateParams *result) {

	return srpc_async_call(_srpc, SUPLA_DS_CALL_GET_FIRMWARE_UPDATE_URL, (char*)result, sizeof(TDS_FirmwareUpdateParams));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_ds_async_registerdevice(void *_srpc, TDS_SuplaRegisterDevice *registerdevice) {

	_supla_int_t size = sizeof(TDS_SuplaRegisterDevice)
			- ( sizeof(TDS_SuplaDeviceChannel) * SUPLA_CHANNELMAXCOUNT )
			+ ( sizeof(TDS_SuplaDeviceChannel) * registerdevice->channel_count );

	if ( size > sizeof(TDS_SuplaRegisterDevice) )
		return 0;

	return srpc_async_call(_srpc, SUPLA_DS_CALL_REGISTER_DEVICE, (char*)registerdevice, size);
}

_supla_int_t SRPC_ICACHE_FLASH srpc_ds_async_registerdevice_b(void *_srpc, TDS_SuplaRegisterDevice_B *registerdevice) {

	_supla_int_t size = sizeof(TDS_SuplaRegisterDevice_B)
			- ( sizeof(TDS_SuplaDeviceChannel_B) * SUPLA_CHANNELMAXCOUNT )
			+ ( sizeof(TDS_SuplaDeviceChannel_B) * registerdevice->channel_count );


	if ( size > sizeof(TDS_SuplaRegisterDevice_B) )
		return 0;
	

	return srpc_async_call(_srpc, SUPLA_DS_CALL_REGISTER_DEVICE_B, (char*)registerdevice, size);
}

_supla_int_t SRPC_ICACHE_FLASH srpc_ds_async_registerdevice_c(void *_srpc, TDS_SuplaRegisterDevice_C *registerdevice) {

	_supla_int_t size = sizeof(TDS_SuplaRegisterDevice_C)
			- ( sizeof(TDS_SuplaDeviceChannel_B) * SUPLA_CHANNELMAXCOUNT )
			+ ( sizeof(TDS_SuplaDeviceChannel_B) * registerdevice->channel_count );


	if ( size > sizeof(TDS_SuplaRegisterDevice_C) )
		return 0;


	return srpc_async_call(_srpc, SUPLA_DS_CALL_REGISTER_DEVICE_C, (char*)registerdevice, size);
}

_supla_int_t SRPC_ICACHE_FLASH srpc_ds_async_registerdevice_d(void *_srpc, TDS_SuplaRegisterDevice_D *registerdevice) {

	_supla_int_t size = sizeof(TDS_SuplaRegisterDevice_D)
			- ( sizeof(TDS_SuplaDeviceChannel_B) * SUPLA_CHANNELMAXCOUNT )
			+ ( sizeof(TDS_SuplaDeviceChannel_B) * registerdevice->channel_count );


	if ( size > sizeof(TDS_SuplaRegisterDevice_D) )
		return 0;


	return srpc_async_call(_srpc, SUPLA_DS_CALL_REGISTER_DEVICE_D, (char*)registerdevice, size);
}

_supla_int_t SRPC_ICACHE_FLASH srpc_sd_async_registerdevice_result(void *_srpc, TSD_SuplaRegisterDeviceResult *registerdevice_result) {
	return srpc_async_call(_srpc, SUPLA_SD_CALL_REGISTER_DEVICE_RESULT, (char*)registerdevice_result, sizeof(TSD_SuplaRegisterDeviceResult));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_cs_async_registerclient(void *_srpc, TCS_SuplaRegisterClient *registerclient) {
	return srpc_async_call(_srpc, SUPLA_CS_CALL_REGISTER_CLIENT, (char*)registerclient, sizeof(TCS_SuplaRegisterClient));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_cs_async_registerclient_b(void *_srpc, TCS_SuplaRegisterClient_B *registerclient) {
	return srpc_async_call(_srpc, SUPLA_CS_CALL_REGISTER_CLIENT_B, (char*)registerclient, sizeof(TCS_SuplaRegisterClient_B));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_cs_async_registerclient_c(void *_srpc, TCS_SuplaRegisterClient_C *registerclient) {
	return srpc_async_call(_srpc, SUPLA_CS_CALL_REGISTER_CLIENT_C, (char*)registerclient, sizeof(TCS_SuplaRegisterClient_C));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_sc_async_registerclient_result(void *_srpc, TSC_SuplaRegisterClientResult *registerclient_result) {
	return srpc_async_call(_srpc, SUPLA_SC_CALL_REGISTER_CLIENT_RESULT, (char*)registerclient_result, sizeof(TSC_SuplaRegisterClientResult));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_ds_async_channel_value_changed(void *_srpc, unsigned char channel_number, char *value) {
	TDS_SuplaDeviceChannelValue ncsc;
	ncsc.ChannelNumber = channel_number;
	memcpy(ncsc.value, value, SUPLA_CHANNELVALUE_SIZE);

	return srpc_async_call(_srpc, SUPLA_DS_CALL_DEVICE_CHANNEL_VALUE_CHANGED, (char*)&ncsc, sizeof(TDS_SuplaDeviceChannelValue));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_sd_async_set_channel_value(void *_srpc, TSD_SuplaChannelNewValue *value) {

	return srpc_async_call(_srpc, SUPLA_SD_CALL_CHANNEL_SET_VALUE, (char*)value, sizeof(TSD_SuplaChannelNewValue));

}

_supla_int_t SRPC_ICACHE_FLASH srpc_ds_async_set_channel_result(void *_srpc, unsigned char ChannelNumber, _supla_int_t SenderID, char Success) {

	TDS_SuplaChannelNewValueResult result;
	result.ChannelNumber = ChannelNumber;
	result.SenderID = SenderID;
	result.Success = Success;

	return srpc_async_call(_srpc, SUPLA_DS_CALL_CHANNEL_SET_VALUE_RESULT, (char*)&result, sizeof(TDS_SuplaChannelNewValueResult));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_sd_async_get_firmware_update_url_result(void *_srpc, TSD_FirmwareUpdate_UrlResult *result) {

	return srpc_async_call(_srpc, SUPLA_SD_CALL_GET_FIRMWARE_UPDATE_URL_RESULT, (char*)result, result->exists == 1 ? sizeof(TSD_FirmwareUpdate_UrlResult) : sizeof(char));

}

_supla_int_t SRPC_ICACHE_FLASH srpc_sc_async_location_update(void *_srpc, TSC_SuplaLocation *location) {

	_supla_int_t size = sizeof(TSC_SuplaLocation)-SUPLA_LOCATION_CAPTION_MAXSIZE+location->CaptionSize;

	if ( size > sizeof(TSC_SuplaLocation) )
		return 0;

	return srpc_async_call(_srpc, SUPLA_SC_CALL_LOCATION_UPDATE, (char*)location, size);
}

_supla_int_t SRPC_ICACHE_FLASH srpc_set_pack(void *_srpc, void *pack, _supla_int_t count,
														_func_srpc_pack_get_caption_size get_caption_size,
														_func_srpc_pack_get_item_ptr get_item_ptr,
														_func_srpc_pack_set_pack_count set_pack_count,
		                                                 unsigned _supla_int_t pack_sizeof,
		                                                 unsigned _supla_int_t pack_max_count,
		                                                 unsigned _supla_int_t caption_max_size,
		                                                 unsigned _supla_int_t item_sizeof,
		                                                 unsigned _supla_int_t call_type) {

	_supla_int_t result = 0;
	_supla_int_t a;
	_supla_int_t n=0;
	_supla_int_t size = 0;
	_supla_int_t offset = 0;

	if ( count > pack_max_count )
		return 0;

	size = pack_sizeof-(item_sizeof*pack_max_count);
	offset = size;

	char *buffer = malloc(size);

	if ( buffer == NULL )
		return 0;

	memcpy(buffer, pack, size);

	for(a=0;a<count;a++) {

		if ( get_caption_size(pack, a) <= caption_max_size ) {
			size+=item_sizeof-caption_max_size+get_caption_size(pack, a);

			char *new_buffer = (char *)realloc(buffer, size);

			if ( new_buffer == NULL ) {
				free(buffer);
				return 0;
			}

			buffer = new_buffer;
			memcpy(&buffer[offset], get_item_ptr(pack, a), size-offset);
			offset+=size-offset;
			n++;
		}

	}

	set_pack_count(buffer, n, 0);

	result = srpc_async_call(_srpc, call_type, buffer, size);

	free(buffer);
	return result;
}

unsigned _supla_int_t srpc_locationpack_get_caption_size(void *pack, _supla_int_t idx) {
	return ((TSC_SuplaLocationPack *)pack)->locations[idx].CaptionSize;
};

_supla_int_t SRPC_ICACHE_FLASH srpc_sc_async_locationpack_update(void *_srpc, TSC_SuplaLocationPack *location_pack) {

	return SRPC_ICACHE_FLASH srpc_set_pack(_srpc, location_pack, location_pack->count,
			&srpc_locationpack_get_caption_size,
			&srpc_locationpack_get_item_ptr,
			&srpc_locationpack_set_pack_count,
			sizeof(TSC_SuplaLocationPack),
			SUPLA_LOCATIONPACK_MAXCOUNT,
			SUPLA_LOCATION_CAPTION_MAXSIZE,
			sizeof(TSC_SuplaLocation),
			SUPLA_SC_CALL_LOCATIONPACK_UPDATE);

}

_supla_int_t SRPC_ICACHE_FLASH srpc_sc_async_channel_update(void *_srpc, TSC_SuplaChannel *channel) {

	_supla_int_t size = sizeof(TSC_SuplaChannel)-SUPLA_CHANNEL_CAPTION_MAXSIZE+channel->CaptionSize;

	if ( size > sizeof(TSC_SuplaChannel) )
		return 0;

	return srpc_async_call(_srpc, SUPLA_SC_CALL_CHANNEL_UPDATE, (char*)channel, size);

}

_supla_int_t SRPC_ICACHE_FLASH srpc_sc_async_channel_update_b(void *_srpc, TSC_SuplaChannel_B *channel_b) {

	_supla_int_t size = sizeof(TSC_SuplaChannel_B)-SUPLA_CHANNEL_CAPTION_MAXSIZE+channel_b->CaptionSize;

	if ( size > sizeof(TSC_SuplaChannel_B) )
		return 0;

	return srpc_async_call(_srpc, SUPLA_SC_CALL_CHANNEL_UPDATE_B, (char*)channel_b, size);

}

unsigned _supla_int_t srpc_channelpack_get_caption_size(void *pack, _supla_int_t idx) {
	return ((TSC_SuplaChannelPack *)pack)->channels[idx].CaptionSize;
};

unsigned _supla_int_t srpc_channelpack_get_caption_size_b(void *pack, _supla_int_t idx) {
	return ((TSC_SuplaChannelPack_B *)pack)->channels[idx].CaptionSize;
};

_supla_int_t SRPC_ICACHE_FLASH srpc_sc_async_channelpack_update(void *_srpc, TSC_SuplaChannelPack *channel_pack) {


	return SRPC_ICACHE_FLASH srpc_set_pack(_srpc, channel_pack, channel_pack->count,
			&srpc_channelpack_get_caption_size,
			&srpc_channelpack_get_item_ptr,
			&srpc_channelpack_set_pack_count,
			sizeof(TSC_SuplaChannelPack),
			SUPLA_CHANNELPACK_MAXCOUNT,
			SUPLA_CHANNEL_CAPTION_MAXSIZE,
			sizeof(TSC_SuplaChannel),
			SUPLA_SC_CALL_CHANNELPACK_UPDATE);
}

_supla_int_t SRPC_ICACHE_FLASH srpc_sc_async_channelpack_update_b(void *_srpc, TSC_SuplaChannelPack_B *channel_pack) {

	return SRPC_ICACHE_FLASH srpc_set_pack(_srpc, channel_pack, channel_pack->count,
			&srpc_channelpack_get_caption_size_b,
			&srpc_channelpack_get_item_ptr_b,
			&srpc_channelpack_set_pack_count_b,
			sizeof(TSC_SuplaChannelPack_B),
			SUPLA_CHANNELPACK_MAXCOUNT,
			SUPLA_CHANNEL_CAPTION_MAXSIZE,
			sizeof(TSC_SuplaChannel_B),
			SUPLA_SC_CALL_CHANNELPACK_UPDATE_B);
}


_supla_int_t SRPC_ICACHE_FLASH srpc_sc_async_channel_value_update(void *_srpc, TSC_SuplaChannelValue *channel_value) {
	return srpc_async_call(_srpc, SUPLA_SC_CALL_CHANNEL_VALUE_UPDATE, (char*)channel_value, sizeof(TSC_SuplaChannelValue));
}


_supla_int_t SRPC_ICACHE_FLASH srpc_cs_async_get_next(void *_srpc) {
	return srpc_async_call(_srpc, SUPLA_CS_CALL_GET_NEXT, NULL, 0);
}

_supla_int_t SRPC_ICACHE_FLASH srpc_sc_async_event(void *_srpc, TSC_SuplaEvent *event) {
	_supla_int_t size = sizeof(TSC_SuplaEvent)-SUPLA_SENDER_NAME_MAXSIZE+event->SenderNameSize;

	if ( size > sizeof(TSC_SuplaEvent) )
		return 0;

	return srpc_async_call(_srpc, SUPLA_SC_CALL_EVENT, (char*)event, size);
}


_supla_int_t SRPC_ICACHE_FLASH srpc_cs_async_set_channel_value(void *_srpc, TCS_SuplaChannelNewValue *value) {
	return srpc_async_call(_srpc, SUPLA_CS_CALL_CHANNEL_SET_VALUE, (char*)value, sizeof(TCS_SuplaChannelNewValue));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_cs_async_set_channel_value_b(void *_srpc, TCS_SuplaChannelNewValue_B *value) {
	return srpc_async_call(_srpc, SUPLA_CS_CALL_CHANNEL_SET_VALUE_B, (char*)value, sizeof(TCS_SuplaChannelNewValue_B));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_cs_async_get_oauth_parameters(void *_srpc, TCS_OAuthParametersRequest *req) {
	return srpc_async_call(_srpc, SUPLA_CS_CALL_GET_OAUTH_PARAMETERS, (char*)req, sizeof(TCS_OAuthParametersRequest));
}

_supla_int_t SRPC_ICACHE_FLASH srpc_sc_async_get_oauth_parameters_result(void *_srpc, TSC_OAuthParameters *params) {
	return srpc_async_call(_srpc, SUPLA_SC_CALL_GET_OAUTH_PARAMETERS_RESULT, (char*)params, sizeof(TSC_OAuthParameters));
}

