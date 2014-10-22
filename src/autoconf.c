/* 
 * MuMuDVB - Stream a DVB transport stream.
 * File for Autoconfiguration
 *
 * (C) 2008-2010 Brice DUBOST <mumudvb@braice.net>
 *
 * Parts of this code come from libdvb, modified for mumudvb
 * by Brice DUBOST
 * Libdvb part : Copyright (C) 2000 Klaus Schmidinger
 *
 * The latest version can be found at http://mumudvb.braice.net
 *
 * Copyright notice:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/** @file
 *  @brief This file contain the code related to the autoconfiguration of mumudvb
 * 
 *  It contains the functions to extract the relevant informations from the PAT,PMT,SDT pids and from ATSC PSIP table
 * 
 *  The PAT contains the list of the channels in the actual stream, their service number and the PMT pid
 * 
 *  The SDT contains the name of the channels associated to a certain service number and the type of service
 *
 *  The PSIP (ATSC only) table contains the same kind of information as the SDT
 *
 *  The pmt contains the Pids (audio video etc ...) of the channels,
 *
 *  The idea is the following (for full autoconf),
 *  once we find a sdt, we add the service to a service list (ie we add the name and the service number)
 *  if we find a pat, we check if we have seen the services before, if no we skip, if yes we update the pmt pids
 *
 *  Once we updated all the services or reach the timeout we create a channel list from the services list and we go
 *  in the autoconf=partial mode (and we add the filters for the new pmt pids)
 *
 *  In partial autoconf, we read the pmt pids to find the other pids of the channel. We add only pids wich seems relevant
 *  ie : audio, video, pcr, teletext, subtitle
 *
 *  once it's finished, we add the new filters
 */

#include <sys/ioctl.h>
#include <errno.h>
#include <stdint.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/poll.h>


#include "errors.h"
#include "ts.h"
#include "mumudvb.h"
#include "dvb.h"
#include "network.h"
#include "autoconf.h"
#include "rtp.h"
#include "log.h"
#ifdef ENABLE_SCAM_SUPPORT
#include "scam_common.h"
#endif

static char *log_module="Autoconf: ";


mumudvb_service_t *autoconf_find_service_for_add(mumudvb_service_t *services,int service_id);
mumudvb_service_t *autoconf_find_service_for_modify(mumudvb_service_t *services,int service_id);
void autoconf_free_services(mumudvb_service_t *services);
int autoconf_read_sdt(unsigned char *buf,int len, mumudvb_service_t *services);
int autoconf_read_psip(auto_p_t *parameters);
int autoconf_read_pmt(mumudvb_ts_packet_t *pmt, mumudvb_channel_t *channel, char *card_base_path, int tuner, uint8_t *asked_pid, uint8_t *number_chan_asked_pid,fds_t *fds);
void autoconf_sort_services(mumudvb_service_t *services);
int autoconf_read_nit(auto_p_t *parameters, mumudvb_channel_t *channels, int number_of_channels);



/** Initialize Autoconf variables*/
void init_aconf_v(auto_p_t *aconf_p)
{
	*aconf_p=(auto_p_t){
		.lock=PTHREAD_MUTEX_INITIALIZER,
		.autoconfiguration=0,
		.autoconf_radios=0,
		.autoconf_scrambled=0,
		.autoconf_pid_update=1,
		.autoconf_ip4="239.100.%card.%number",
		.autoconf_ip6="FF15:4242::%server:%card:%number",
		.time_start_autoconfiguration=0,
		.transport_stream_id=-1,
		.autoconf_temp_pat=NULL,
		.autoconf_temp_sdt=NULL,
		.autoconf_temp_psip=NULL,
		.services=NULL,
		.autoconf_unicast_port="\0",
		.autoconf_multicast_port="\0",
		.num_service_id=0,
		.name_template="\0",
	};
}



/** @brief Read a line of the configuration file to check if there is a autoconf parameter
 *
 * @param auto_p the autoconfiguration parameters
 * @param substring The currrent line
 */
int read_autoconfiguration_configuration(auto_p_t *auto_p, char *substring)
{

	char delimiteurs[] = CONFIG_FILE_SEPARATOR;

	if (!strcmp (substring, "autoconf_scrambled"))
	{
		substring = strtok (NULL, delimiteurs);
		auto_p->autoconf_scrambled = atoi (substring);
	}
	else if (!strcmp (substring, "autoconf_pid_update"))
	{
		substring = strtok (NULL, delimiteurs);
		auto_p->autoconf_pid_update = atoi (substring);
	}
	else if (!strcmp (substring, "autoconfiguration"))
	{
		substring = strtok (NULL, delimiteurs);
		if(atoi (substring)==2)
			auto_p->autoconfiguration = AUTOCONF_MODE_FULL;
		else if(atoi (substring)==1)
			auto_p->autoconfiguration = AUTOCONF_MODE_PIDS;
		else if (!strcmp (substring, "full"))
			auto_p->autoconfiguration = AUTOCONF_MODE_FULL;
		else if (!strcmp (substring, "partial"))
			auto_p->autoconfiguration = AUTOCONF_MODE_PIDS;
		else if (!strcmp (substring, "none"))
			auto_p->autoconfiguration = AUTOCONF_MODE_NONE;

		if(!((auto_p->autoconfiguration==AUTOCONF_MODE_PIDS)||(auto_p->autoconfiguration==AUTOCONF_MODE_FULL)||(auto_p->autoconfiguration==AUTOCONF_MODE_NONE)))
		{
			log_message( log_module,  MSG_WARN,
					"Bad value for autoconfiguration, autoconfiguration will not be run\n");
			auto_p->autoconfiguration=AUTOCONF_MODE_NONE;
		}
	}
	else if (!strcmp (substring, "autoconf_radios"))
	{
		substring = strtok (NULL, delimiteurs);
		auto_p->autoconf_radios = atoi (substring);
		if(!(auto_p->autoconfiguration==AUTOCONF_MODE_FULL))
		{
			log_message( log_module,  MSG_INFO,
					"You have to set autoconfiguration in full mode to use autoconf of the radios\n");
		}
	}
	else if ((!strcmp (substring, "autoconf_ip4")))
	{
		substring = strtok (NULL, delimiteurs);
		if(strlen(substring)>79)
		{
			log_message( log_module,  MSG_ERROR,
					"The autoconf ip v4 is too long\n");
			return -1;
		}
		sscanf (substring, "%s\n", auto_p->autoconf_ip4);
	}
	else if (!strcmp (substring, "autoconf_ip6"))
	{
		substring = strtok (NULL, delimiteurs);
		if(strlen(substring)>79)
		{
			log_message( log_module,  MSG_ERROR,
					"The autoconf ip v6 is too long\n");
			return -1;
		}
		sscanf (substring, "%s\n", auto_p->autoconf_ip6);
	}
	/**  option for the starting http unicast port (for autoconf full)*/
	else if (!strcmp (substring, "autoconf_unicast_start_port"))
	{
		substring = strtok (NULL, delimiteurs);
		sprintf(auto_p->autoconf_unicast_port,"%d +%%number",atoi (substring));
	}
	/**  option for the http unicast port (for autoconf full) parsed version*/
	else if (!strcmp (substring, "autoconf_unicast_port"))
	{
		substring = strtok (NULL, "=");
		if(strlen(substring)>255)
		{
			log_message( log_module,  MSG_ERROR,
					"The autoconf_unicast_port is too long\n");
			return -1;
		}
		strcpy(auto_p->autoconf_unicast_port,substring);
	}
	/**  option for the http multicast port (for autoconf full) parsed version*/
	else if (!strcmp (substring, "autoconf_multicast_port"))
	{
		substring = strtok (NULL, "=");
		if(strlen(substring)>255)
		{
			log_message( log_module,  MSG_ERROR,
					"The autoconf_multicast_port is too long\n");
			return -1;
		}
		strcpy(auto_p->autoconf_multicast_port,substring);
	}
	else if (!strcmp (substring, "autoconf_sid_list"))
	{
		while ((substring = strtok (NULL, delimiteurs)) != NULL)
		{
			if (auto_p->num_service_id >= MAX_CHANNELS)
			{
				log_message( log_module,  MSG_ERROR,
						"Autoconfiguration : Too many ts id : %d\n",
						auto_p->num_service_id);
				return -1;
			}
			auto_p->service_id_list[auto_p->num_service_id] = atoi (substring);
			auto_p->num_service_id++;
		}
	}
	else if (!strcmp (substring, "autoconf_name_template"))
	{
		// other substring extraction method in order to keep spaces
		substring = strtok (NULL, "=");
		strncpy(auto_p->name_template,strtok(substring,"\n"),MAX_NAME_LEN-1);
		auto_p->name_template[MAX_NAME_LEN-1]='\0';
		if (strlen (substring) >= MAX_NAME_LEN - 1)
			log_message( log_module,  MSG_WARN,"Autoconfiguration: Channel name template too long\n");
	}
	else
		return 0; //Nothing concerning autoconfiguration, we return 0 to explore the other possibilities

	return 1;//We found something for autoconfiguration, we tell main to go for the next line
}


/** @brief initialize the autoconfiguration : alloc the memory etc...
 *
 */
int autoconf_init(auto_p_t *auto_p, mumudvb_channel_t *channels,int number_of_channels)
{
	int ichan;

	if(auto_p->autoconfiguration==AUTOCONF_MODE_FULL)
	{
		auto_p->autoconf_temp_pat=malloc(sizeof(mumudvb_ts_packet_t));
		if(auto_p->autoconf_temp_pat==NULL)
		{
			log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
			set_interrupted(ERROR_MEMORY<<8);
			return -1;
		}
		memset (auto_p->autoconf_temp_pat, 0, sizeof( mumudvb_ts_packet_t));//we clear it
		pthread_mutex_init(&auto_p->autoconf_temp_pat->packetmutex,NULL);
		auto_p->autoconf_temp_sdt=malloc(sizeof(mumudvb_ts_packet_t));
		if(auto_p->autoconf_temp_sdt==NULL)
		{
			log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
			set_interrupted(ERROR_MEMORY<<8);
			return -1;
		}
		memset (auto_p->autoconf_temp_sdt, 0, sizeof( mumudvb_ts_packet_t));//we clear it
		pthread_mutex_init(&auto_p->autoconf_temp_sdt->packetmutex,NULL);

		auto_p->autoconf_temp_psip=malloc(sizeof(mumudvb_ts_packet_t));
		if(auto_p->autoconf_temp_psip==NULL)
		{
			log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
			set_interrupted(ERROR_MEMORY<<8);
			return -1;
		}
		memset (auto_p->autoconf_temp_psip, 0, sizeof( mumudvb_ts_packet_t));//we clear it
		pthread_mutex_init(&auto_p->autoconf_temp_psip->packetmutex,NULL);

		auto_p->services=malloc(sizeof(mumudvb_service_t));
		if(auto_p->services==NULL)
		{
			log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
			set_interrupted(ERROR_MEMORY<<8);
			return -1;
		}
		memset (auto_p->services, 0, sizeof( mumudvb_service_t));//we clear it

	}

	if (auto_p->autoconfiguration==AUTOCONF_MODE_PIDS)
		for (ichan = 0; ichan < number_of_channels; ichan++)
		{
			//If there is more than one pid in one channel we mark it
			//For no autoconfiguration
			if(channels[ichan].num_pids>1)
			{
				log_message( log_module,  MSG_DETAIL, "Autoconfiguration deactivated for channel \"%s\" \n", channels[ichan].name);
				channels[ichan].autoconfigurated=1;
			}
			else if (channels[ichan].num_pids==1)
			{
				//Only one pid with autoconfiguration=partial, it's the PMT pid
				channels[ichan].pmt_pid=channels[ichan].pids[0];
				channels[ichan].pids_type[0]=PID_PMT;
				snprintf(channels[ichan].pids_language[0],4,"%s","---");
			}
		}
	if (auto_p->autoconfiguration)
	{
		auto_p->autoconf_temp_nit=malloc(sizeof(mumudvb_ts_packet_t));
		if(auto_p->autoconf_temp_nit==NULL)
		{
			log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
			set_interrupted(ERROR_MEMORY<<8);
			return -1;
		}
		memset (auto_p->autoconf_temp_nit, 0, sizeof( mumudvb_ts_packet_t));//we clear it
		pthread_mutex_init(&auto_p->autoconf_temp_nit->packetmutex,NULL);
	}
	return 0;

}


/****************************************************************************/
//Parts of this code (read of the pmt and read of the pat)
// from libdvb, strongly modified, with commentaries added
/****************************************************************************/

/** @brief read the PAT for autoconfiguration
 * This function extract the pmt from the pat 
 * before doing so it checks if the service is already initialised (sdt packet)
 *
 * @param auto_p The autoconfiguration structure, containing all we need
 */
int autoconf_read_pat(auto_p_t *auto_p)
{

	mumudvb_ts_packet_t *pat_mumu;
	mumudvb_service_t *services;
	unsigned char *buf=NULL;
	mumudvb_service_t *a_service=NULL;
	pat_mumu=auto_p->autoconf_temp_pat;
	services=auto_p->services;
	buf=pat_mumu->data_full;
	pat_t       *pat=(pat_t*)(buf);
	pat_prog_t  *prog;
	int delta=PAT_LEN;
	int section_length=0;
	int number_of_services=0;
	int channels_missing=0;
	int new_services=0;

	log_message( log_module, MSG_DEBUG,"---- New PAT ----\n");
	//we display the contents
	ts_display_pat(log_module,buf);
	//PAT reading
	section_length=HILO(pat->section_length);


	/*current_next_indicator – A 1-bit indicator, which when set to '1' indicates that the Program Association Table
  sent is currently applicable. When the bit is set to '0', it indicates that the table sent is not yet applicable
  and shall be the next table to become valid.*/
	if(pat->current_next_indicator == 0)
	{
		log_message( log_module, MSG_DEBUG,"The current_next_indicator is set to 0, this PAT is not valid for the current stream\n");
		return 0;
	}

	//We store the transport stream ID
	auto_p->transport_stream_id=HILO(pat->transport_stream_id);

	//We loop over the different programs included in the pat
	while((delta+PAT_PROG_LEN)<(section_length))
	{
		prog=(pat_prog_t*)((char*)buf+delta);
		if(HILO(prog->program_number)!=0)
		{
			//Do we have already this program in the service list ?
			//Ie : do we already know the channel name/type ?
			a_service=autoconf_find_service_for_modify(services,HILO(prog->program_number));
			if(a_service)
			{
				if(!a_service->pmt_pid)
				{
					//We found a new service without the PMT, pid, we update this service
					new_services=1;
					a_service->pmt_pid=HILO(prog->network_pid);
					log_message( log_module, MSG_DETAIL,"service updated  PMT PID : %d\t id 0x%x\t name \"%s\"\n",
							a_service->pmt_pid,
							a_service->id,
							a_service->name);
				}
			}
			else
			{
				log_message( log_module, MSG_DEBUG,"service missing  PMT PID : %d\t id 0x%x %d\n",
						HILO(prog->network_pid),
						HILO(prog->program_number),
						HILO(prog->program_number));
				channels_missing++;
			}
			number_of_services++;
		}
		delta+=PAT_PROG_LEN;
	}

	if(channels_missing)
	{
		if(new_services)
			log_message( log_module, MSG_DETAIL,"PAT read %d channels on %d are missing, we wait for others SDT/PSIP for the moment.\n",channels_missing,number_of_services);
		return 0;
	}

	return 1;
}


/** @brief Try to find the service specified by id, if not found create a new one.
 * if the service is not foud, it returns a pointer to the new service, and NULL if 
 * the service is found or run out of memory.
 * 
 * @param services the chained list of services
 * @param service_id the identifier/program number of the searched service
 */
mumudvb_service_t *autoconf_find_service_for_add(mumudvb_service_t *services,int service_id)
{
	int found=0;
	mumudvb_service_t *a_service;

	a_service=services;

	if(a_service->id==service_id)
		found=1;

	while(found==0 && a_service->next!=NULL)
	{
		a_service=a_service->next;
		if(a_service->id==service_id)
			found=1;
	}

	if(found)
		return NULL;

	a_service->next=malloc(sizeof(mumudvb_service_t));
	if(a_service->next==NULL)
	{
		log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
		return NULL;
	}
	memset (a_service->next, 0, sizeof( mumudvb_service_t));//we clear it
	return a_service->next;

}

/** @brief try to find the service specified by id
 * if not found return NULL, otherwise return the service
 *
 * @param services the chained list of services
 * @param service_id the identifier of the searched service
 */
mumudvb_service_t *autoconf_find_service_for_modify(mumudvb_service_t *services,int service_id)
{
	mumudvb_service_t *found=NULL;
	mumudvb_service_t *a_service;

	a_service=services;

	while(found==NULL && a_service!=NULL)
	{
		if(a_service->id==service_id)
			found=a_service;
		a_service=a_service->next;
	}

	if(found)
		return found;

	return NULL;

}


/**@brief Free the autoconf parameters.
 *
 * @param auto_p pointer to the autoconf structure
 */
void autoconf_freeing(auto_p_t *auto_p)
{
	if(auto_p->autoconf_temp_sdt)
	{
		free(auto_p->autoconf_temp_sdt);
		auto_p->autoconf_temp_sdt=NULL;
	}
	if(auto_p->autoconf_temp_psip)
	{
		free(auto_p->autoconf_temp_psip);
		auto_p->autoconf_temp_psip=NULL;
	}
	if(auto_p->autoconf_temp_pat)
	{
		free(auto_p->autoconf_temp_pat);
		auto_p->autoconf_temp_pat=NULL;
	}
	if(auto_p->services)
	{
		autoconf_free_services(auto_p->services);
		auto_p->services=NULL;
	}
}

/**@brief Free the chained list of services.
 *
 * @param services the chained list of services
 */
void autoconf_free_services(mumudvb_service_t *services)
{

	mumudvb_service_t *a_service;
	mumudvb_service_t *n_service;

	for(a_service=services;a_service != NULL; a_service=n_service)
	{
		n_service= a_service->next;
		free(a_service);
	}
}

/**@brief Sort the chained list of services.
 *
 * This function sort the services using their service_id, this service doesn't sort the first one :( (but I think it's empty)
 * Unefficient sorting : O(n^2) but the number of services is never big and this function is called once
 * @param services the chained list of services
 */
void autoconf_sort_services(mumudvb_service_t *services)
{

	mumudvb_service_t *a_service;
	mumudvb_service_t *n_service;
	mumudvb_service_t *a_service_int;
	mumudvb_service_t *n_service_int;
	mumudvb_service_t *p_service_int;
	mumudvb_service_t *temp_service_int;
	p_service_int=NULL;
	log_message( log_module, MSG_DEBUG,"Service sorting\n");
	log_message( log_module, MSG_FLOOD,"Service sorting BEFORE\n");
	for(a_service=services;a_service != NULL; a_service=n_service)
	{
		log_message( log_module, MSG_FLOOD,"Service sorting, id %d\t service : %s \n", a_service->id, a_service->name);
		n_service= a_service->next;
	}
	for(a_service=services;a_service != NULL; a_service=n_service)
	{
		for(a_service_int=services;a_service_int != NULL; a_service_int=n_service_int)
		{
			n_service_int= a_service_int->next;
			if((p_service_int != NULL) &&(n_service_int != NULL) &&(n_service_int->id)&&(a_service_int->id) && n_service_int->id < a_service_int->id)
			{
				p_service_int->next=n_service_int;
				a_service_int->next=n_service_int->next;
				n_service_int->next=a_service_int;
				if(a_service_int==a_service)
					a_service=n_service_int;
				temp_service_int=n_service_int;
				n_service_int=a_service_int;
				a_service_int=temp_service_int;
			}
			p_service_int=a_service_int;
		}
		n_service= a_service->next;
	}
	log_message( log_module, MSG_FLOOD,"Service sorting AFTER\n");
	for(a_service=services;a_service != NULL; a_service=n_service)
	{
		log_message( log_module, MSG_FLOOD,"Service sorting, id %d\t service : %s \n", a_service->id, a_service->name);
		n_service= a_service->next;
	}
}

/** @brief Convert the chained list of services into channels
 *
 * This function is called when We've got all the services, we now fill the channels structure
 * After that we go in AUTOCONF_MODE_PIDS to get audio and video pids
 * @param parameters The autoconf parameters
 * @param channels Chained list of channels
 * @param port The mulicast port
 * @param card The card number for the ip address
 * @param unicast_vars The unicast parameters
 * @param fds The file descriptors (for filters and unicast)
 */
int autoconf_services_to_channels(const auto_p_t *parameters, mumudvb_channel_t *channels, int port, int card, int tuner, unicast_parameters_t *unicast_vars, multi_p_t *multi_p, int server_id, void *scam_vars_v)
{
	mumudvb_service_t *service;
	int iChan=0;
	int found_in_service_id_list;
	int unicast_port_per_channel;
	char tempstring[256];
	service=parameters->services;
	unicast_port_per_channel=strlen(parameters->autoconf_unicast_port)?1:0;

#ifndef ENABLE_SCAM_SUPPORT
	(void) scam_vars_v; //to make compiler happy
#else
	scam_parameters_t *scam_vars=(scam_parameters_t *)scam_vars_v;
#endif

	do
	{
		if(parameters->autoconf_scrambled && service->free_ca_mode)
			log_message( log_module, MSG_DETAIL,"Service scrambled. Name \"%s\"\n", service->name);

		//If there is a service_id list we look if we find it (option autoconf_sid_list)
		if(parameters->num_service_id)
		{
			int service_id;
			found_in_service_id_list=0;
			for(service_id=0;service_id<parameters->num_service_id && !found_in_service_id_list;service_id++)
			{
				if(parameters->service_id_list[service_id]==service->id)
				{
					found_in_service_id_list=1;
					log_message( log_module, MSG_DEBUG,"Service found in the service_id list. Name \"%s\"\n", service->name);
				}
			}
		}
		else //No ts id list so it is found
			found_in_service_id_list=1;

		if(!parameters->autoconf_scrambled && service->free_ca_mode)
			log_message( log_module, MSG_DETAIL,"Service scrambled, no CAM support and no autoconf_scrambled, we skip. Name \"%s\"\n", service->name);
		else if(!service->pmt_pid)
			log_message( log_module, MSG_DETAIL,"Service without a PMT pid, we skip. Name \"%s\"\n", service->name);
		else if(!found_in_service_id_list)
			log_message( log_module, MSG_DETAIL,"Service NOT in the service_id list, we skip. Name \"%s\", id %d\n", service->name, service->id);
		else //service is ok, we make it a channel
		{
			//Cf EN 300 468 v1.9.1 Table 81
			if((service->type==0x01||
					service->type==0x11||
					service->type==0x16||
					service->type==0x19||
					service->type==0xc0)||
					((service->type==0x02||
							service->type==0x0a)&&parameters->autoconf_radios))
			{
				log_message( log_module, MSG_DETAIL,"We convert a new service into a channel, sid %d pmt_pid %d name \"%s\" \n",
						service->id, service->pmt_pid, service->name);
				display_service_type(service->type, MSG_DETAIL, log_module);
				channels[iChan].channel_type=service->type;
				channels[iChan].num_packet = 0;
				channels[iChan].num_scrambled_packets = 0;
				channels[iChan].scrambled_channel = 0;
				channels[iChan].streamed_channel = 1;
				channels[iChan].nb_bytes=0;
				channels[iChan].pids[0]=service->pmt_pid;
				channels[iChan].pids_type[0]=PID_PMT;
				channels[iChan].num_pids=1;
				snprintf(channels[iChan].pids_language[0],4,"%s","---");
				if(strlen(parameters->name_template))
				{
					strcpy(channels[iChan].name,parameters->name_template);
					int len=MAX_NAME_LEN;
					char number[10];
					mumu_string_replace(channels[iChan].name,&len,0,"%name",service->name);
					sprintf(number,"%d",iChan+1);
					mumu_string_replace(channels[iChan].name,&len,0,"%number",number);
					//put LCN here
				}
				else
					strcpy(channels[iChan].name,service->name);
				if(multi_p->multicast)
				{
					char number[10];
					char ip[80];
					int len=80;

					if(strlen(parameters->autoconf_multicast_port))
					{
						strcpy(tempstring,parameters->autoconf_multicast_port);
						sprintf(number,"%d",iChan);
						mumu_string_replace(tempstring,&len,0,"%number",number);
						sprintf(number,"%d",card);
						mumu_string_replace(tempstring,&len,0,"%card",number);
						sprintf(number,"%d",tuner);
						mumu_string_replace(tempstring,&len,0,"%tuner",number);
						sprintf(number,"%d",server_id);
						mumu_string_replace(tempstring,&len,0,"%server",number);
						//SID
						sprintf(number,"%d",service->id);
						mumu_string_replace(tempstring,&len,0,"%sid",number);
						channels[iChan].portOut=string_comput(tempstring);
					}
					else
					{
						channels[iChan].portOut=port;//do here the job for evaluating the string
					}
					if(multi_p->multicast_ipv4)
					{
						strcpy(ip,parameters->autoconf_ip4);
						sprintf(number,"%d",iChan);
						mumu_string_replace(ip,&len,0,"%number",number);
						sprintf(number,"%d",card);
						mumu_string_replace(ip,&len,0,"%card",number);
						sprintf(number,"%d",tuner);
						mumu_string_replace(ip,&len,0,"%tuner",number);
						sprintf(number,"%d",server_id);
						mumu_string_replace(ip,&len,0,"%server",number);
						//SID
						sprintf(number,"%d",(service->id&0xFF00)>>8);
						mumu_string_replace(ip,&len,0,"%sid_hi",number);
						sprintf(number,"%d",service->id&0x00FF);
						mumu_string_replace(ip,&len,0,"%sid_lo",number);
						// Compute the string, ex: 239.255.130+0*10+2.1
						log_message( log_module, MSG_DEBUG,"Computing expressions in string \"%s\"\n",ip);
						//Splitting and computing. use of strtok_r because it's safer
						int tn[4];
						char *sptr;
						tn[0]=string_comput(strtok_r (ip,".",&sptr));
						tn[1]=string_comput(strtok_r (NULL,".",&sptr));
						tn[2]=string_comput(strtok_r (NULL,".",&sptr));
						tn[3]=string_comput(strtok_r (NULL,".",&sptr));
						sprintf(channels[iChan].ip4Out,"%d.%d.%d.%d",tn[0],tn[1],tn[2],tn[3]); // In C the evalutation order of arguments in a fct  is undefined, no more easy factoring
						log_message( log_module, MSG_DEBUG,"Channel IPv4 : \"%s\" port : %d\n",channels[iChan].ip4Out,channels[iChan].portOut);
					}
					if(multi_p->multicast_ipv6)
					{
						strcpy(ip,parameters->autoconf_ip6);
						sprintf(number,"%d",iChan);
						mumu_string_replace(ip,&len,0,"%number",number);
						sprintf(number,"%d",card);
						mumu_string_replace(ip,&len,0,"%card",number);
						sprintf(number,"%d",tuner);
						mumu_string_replace(ip,&len,0,"%tuner",number);
						sprintf(number,"%d",server_id);
						mumu_string_replace(ip,&len,0,"%server",number);
						//SID
						sprintf(number,"%04x",service->id);
						mumu_string_replace(ip,&len,0,"%sid",number);
						strncpy(channels[iChan].ip6Out,ip,IPV6_CHAR_LEN);
						channels[iChan].ip6Out[IPV6_CHAR_LEN-1]='\0';
						log_message( log_module, MSG_DEBUG,"Channel IPv6 : \"%s\" port : %d\n",channels[iChan].ip6Out,channels[iChan].portOut);
					}
				}

				//This is a scrambled channel, we will have to ask the cam for descrambling it
				if(parameters->autoconf_scrambled && service->free_ca_mode)
					channels[iChan].need_cam_ask=CAM_NEED_ASK;

				//We store the PMT and the service id in the channel
				channels[iChan].pmt_pid=service->pmt_pid;
				channels[iChan].service_id=service->id;
				init_rtp_header(&channels[iChan]); //We init the rtp header in all cases

				if(channels[iChan].pmt_packet==NULL)
				{
					channels[iChan].pmt_packet=malloc(sizeof(mumudvb_ts_packet_t));
					if(channels[iChan].pmt_packet==NULL)
					{
						log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
						set_interrupted(ERROR_MEMORY<<8);
						return -1;
					}
					memset (channels[iChan].pmt_packet, 0, sizeof( mumudvb_ts_packet_t));//we clear it
					pthread_mutex_init(&channels[iChan].pmt_packet->packetmutex,NULL);
				}
#ifdef ENABLE_CAM_SUPPORT
				//We allocate the packet for storing the PMT for CAM purposes
				if(channels[iChan].cam_pmt_packet==NULL)
				{
					channels[iChan].cam_pmt_packet=malloc(sizeof(mumudvb_ts_packet_t));
					if(channels[iChan].cam_pmt_packet==NULL)
					{
						log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
						set_interrupted(ERROR_MEMORY<<8);
						return -1;
					}
					memset (channels[iChan].cam_pmt_packet, 0, sizeof( mumudvb_ts_packet_t));//we clear it
					pthread_mutex_init(&channels[iChan].cam_pmt_packet->packetmutex,NULL);
				}
#endif
				//We update the unicast port, the connection will be created in autoconf_finish_full
				if(unicast_port_per_channel && unicast_vars->unicast)
				{
					strcpy(tempstring,parameters->autoconf_unicast_port);
					int len;len=256;
					char number[10];
					sprintf(number,"%d",iChan);
					mumu_string_replace(tempstring,&len,0,"%number",number);
					sprintf(number,"%d",card);
					mumu_string_replace(tempstring,&len,0,"%card",number);
					sprintf(number,"%d",tuner);
					mumu_string_replace(tempstring,&len,0,"%tuner",number);
					sprintf(number,"%d",server_id);
					mumu_string_replace(tempstring,&len,0,"%server",number);
					//SID
					sprintf(number,"%d",service->id);
					mumu_string_replace(tempstring,&len,0,"%sid",number);
					channels[iChan].unicast_port=string_comput(tempstring);
					log_message( log_module, MSG_DEBUG,"Channel (direct) unicast port  %d\n",channels[iChan].unicast_port);
				}
#ifdef ENABLE_SCAM_SUPPORT
                                if(channels[iChan].scam_pmt_packet==NULL && scam_vars->scam_support)
                                {
                                        channels[iChan].scam_pmt_packet=malloc(sizeof(mumudvb_ts_packet_t));
                                        if(channels[iChan].scam_pmt_packet==NULL)
                                        {
                                                log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
                                                set_interrupted(ERROR_MEMORY<<8);
                                                return -1;
                                        }
                                        memset (channels[iChan].scam_pmt_packet, 0, sizeof( mumudvb_ts_packet_t));//we clear it
                                        pthread_mutex_init(&channels[iChan].scam_pmt_packet->packetmutex,NULL);
                                }

				if (service->free_ca_mode && scam_vars->scam_support) {
					channels[iChan].scam_support=1;
					channels[iChan].need_scam_ask=CAM_NEED_ASK;
#ifdef ENABLE_SCAM_DESCRAMBLER_SUPPORT
					channels[iChan].ring_buffer_size=scam_vars->ring_buffer_default_size;
					channels[iChan].decsa_delay=scam_vars->decsa_default_delay;
					channels[iChan].send_delay=scam_vars->send_default_delay;
#endif
				}
#endif
				iChan++;
			}
			else if(service->type==0x02||service->type==0x0a) //service_type digital radio sound service
				log_message( log_module, MSG_DETAIL,"Service type digital radio sound service, no autoconfigure. (if you want add autoconf_radios=1 to your configuration file) Name \"%s\"\n",service->name);
			else if(service->type!=0) //0 is an empty service
			{
				//We show the service type
				log_message( log_module, MSG_DETAIL,"No autoconfigure due to service type : %s. Name \"%s\"\n",service_type_to_str(service->type),service->name);
			}
		}
		service=service->next;
	}
	while(service && iChan<MAX_CHANNELS);

	if(iChan==MAX_CHANNELS)
		log_message( log_module, MSG_WARN,"Warning : We reached the maximum channel number, we drop other possible channels !\n");

	return iChan;
}

/** @brief Finish full autoconfiguration (set everything needed to go to partial autoconf)
 * This function is called when FULL autoconfiguration is finished
 * It fill the asked pid array
 * It open the file descriptors for the new filters, and set the filters
 * It open the new sockets
 * It free autoconfiguration memory which will be not used anymore
 *
 * @param card the card number
 * @param number_of_channels the number of channels
 * @param channels the array of channels
 * @param fds the file descriptors
 */
int autoconf_finish_full(mumu_chan_p_t *chan_p, auto_p_t *auto_p, multi_p_t *multi_p, tune_p_t *tune_p, fds_t *fds, unicast_parameters_t *unicast_vars, int server_id, void *scam_vars)
{
	pthread_mutex_lock(&chan_p->lock);
	int ichan,ipid;
	//We sort the services
	autoconf_sort_services(auto_p->services);
	chan_p->number_of_channels=autoconf_services_to_channels(auto_p, chan_p->channels, multi_p->common_port, tune_p->card, tune_p->tuner, unicast_vars, multi_p, server_id, scam_vars); //Convert the list of services into channels
	//we got the pmt pids for the channels, we open the filters
	for (ichan = 0; ichan < chan_p->number_of_channels; ichan++)
	{
		for (ipid = 0; ipid < chan_p->channels[ichan].num_pids; ipid++)
		{
			if(chan_p->asked_pid[chan_p->channels[ichan].pids[ipid]]==PID_NOT_ASKED)
				chan_p->asked_pid[chan_p->channels[ichan].pids[ipid]]=PID_ASKED;
			chan_p->number_chan_asked_pid[chan_p->channels[ichan].pids[ipid]]++;
		}
	}

	// we open the file descriptors
	if (create_card_fd (tune_p->card_dev_path, tune_p->tuner, chan_p->asked_pid, fds) < 0)
	{
		log_message( log_module, MSG_ERROR,"ERROR : CANNOT open the new descriptors. Some channels will probably not work\n");
	}
	// we set the new filters
	set_filters( chan_p->asked_pid, fds);


	//Networking
	for (ichan = 0; ichan < chan_p->number_of_channels; ichan++)
	{

		/** open the unicast listening connections for the channels */
		if(chan_p->channels[ichan].unicast_port && unicast_vars->unicast)
		{
			log_message( log_module, MSG_INFO,"Unicast : We open the channel %d http socket address %s:%d\n",
					ichan,
					unicast_vars->ipOut,
					chan_p->channels[ichan].unicast_port);
			unicast_create_listening_socket(UNICAST_LISTEN_CHANNEL,
					ichan,
					unicast_vars->ipOut,
					chan_p->channels[ichan].unicast_port,
					&chan_p->channels[ichan].sIn,
					&chan_p->channels[ichan].socketIn,
					fds,
					unicast_vars);
		}

		//Open the multicast socket for the new channel
		if(multi_p->multicast_ipv4)
		{
			if(multi_p->multicast && multi_p->auto_join) //See the README for the reason of this option
				chan_p->channels[ichan].socketOut4 =
						makeclientsocket (chan_p->channels[ichan].ip4Out,
								chan_p->channels[ichan].portOut,
								multi_p->ttl,
								multi_p->iface4,
								&chan_p->channels[ichan].sOut4);
			else if(multi_p->multicast)
				chan_p->channels[ichan].socketOut4 =
						makesocket (chan_p->channels[ichan].ip4Out,
								chan_p->channels[ichan].portOut,
								multi_p->ttl,
								multi_p->iface4,
								&chan_p->channels[ichan].sOut4);
		}
		if(multi_p->multicast_ipv6)
		{
			if(multi_p->multicast && multi_p->auto_join) //See the README for the reason of this option
				chan_p->channels[ichan].socketOut6 =
						makeclientsocket6 (chan_p->channels[ichan].ip6Out,
								chan_p->channels[ichan].portOut,
								multi_p->ttl,
								multi_p->iface6,
								&chan_p->channels[ichan].sOut6);
			else if(multi_p->multicast)
				chan_p->channels[ichan].socketOut6 =
						makesocket6 (chan_p->channels[ichan].ip6Out,
								chan_p->channels[ichan].portOut,
								multi_p->ttl,
								multi_p->iface6,
								&chan_p->channels[ichan].sOut6);
		}
	}

	log_message( log_module, MSG_DEBUG,"Step TWO, we get the video and audio PIDs\n");
	//We free autoconf memory
	autoconf_freeing(auto_p);

	auto_p->autoconfiguration=AUTOCONF_MODE_PIDS; //Next step add video and audio pids
	pthread_mutex_unlock(&chan_p->lock);

	return 0;
}

/** @brief Finish autoconf
 * This function is called when autoconfiguration is finished
 * It opens what is needed to stream the new channels
 * It creates the file descriptors for the filters, set the filters
 * It also generates a config file with the data obtained during autoconfiguration
 *
 * @param card the card number
 * @param number_of_channels the number of channels
 * @param channels the array of channels
 * @param asked_pid the array containing the pids already asked
 * @param number_chan_asked_pid the number of channels who want this pid
 * @param fds the file descriptors
 */
void autoconf_set_channel_filt(char *card_base_path, int tuner, mumu_chan_p_t *chan_p, fds_t *fds)
{
	int ichan;
	int ipid;


	log_message( log_module, MSG_DETAIL,"Autoconfiguration almost done\n");
	log_message( log_module, MSG_DETAIL,"We open the new file descriptors\n");
	for (ichan = 0; ichan < chan_p->number_of_channels; ichan++)
	{
		for (ipid = 0; ipid < chan_p->channels[ichan].num_pids; ipid++)
		{
			if(chan_p->asked_pid[chan_p->channels[ichan].pids[ipid]]==PID_NOT_ASKED)
				chan_p->asked_pid[chan_p->channels[ichan].pids[ipid]]=PID_ASKED;
			chan_p->number_chan_asked_pid[chan_p->channels[ichan].pids[ipid]]++;
		}
	}
	// we open the file descriptors
	if (create_card_fd (card_base_path, tuner, chan_p->asked_pid, fds) < 0)
	{
		log_message( log_module, MSG_ERROR,"ERROR : CANNOT open the new descriptors. Some channels will probably not work\n");
	}

	log_message( log_module, MSG_DETAIL,"Add the new filters\n");
	set_filters(chan_p->asked_pid, fds);
}

void autoconf_definite_end(mumu_chan_p_t *chan_p, multi_p_t *multi_p, unicast_parameters_t *unicast_vars)
{
	log_message( log_module, MSG_INFO,"Autoconfiguration done\n");

	log_streamed_channels(log_module,chan_p->number_of_channels, chan_p->channels, multi_p->multicast_ipv4, multi_p->multicast_ipv6, unicast_vars->unicast, unicast_vars->portOut, unicast_vars->ipOut);

}

/********************************************************************
 * Autoconfiguration new packet and poll functions
 ********************************************************************/
/** @brief This function is called when a new packet is there and the autoconf is not finished*/
int autoconf_new_packet(int pid, unsigned char *ts_packet, auto_p_t *auto_p, fds_t *fds, mumu_chan_p_t *chan_p, tune_p_t *tune_p, multi_p_t *multi_p,  unicast_parameters_t *unicast_vars, int server_id, void *scam_vars)
{
	pthread_mutex_lock(&auto_p->lock);
	if(auto_p->autoconfiguration==AUTOCONF_MODE_FULL) //Full autoconfiguration, we search the channels and their names
	{
		if(pid==0) //PAT : contains the services identifiers and the PMT PID for each service
		{
			while((auto_p->autoconfiguration==AUTOCONF_MODE_FULL)&&(get_ts_packet(ts_packet,auto_p->autoconf_temp_pat)))
			{
				ts_packet=NULL; // next call we only POP packets from the stack
				if(autoconf_read_pat(auto_p))
				{
					log_message( log_module, MSG_DEBUG,"It seems that we have finished to get the services list\n");
					//we finish full autoconfiguration
					autoconf_finish_full(chan_p, auto_p, multi_p, tune_p, fds, unicast_vars, server_id, scam_vars);
				}
			}
		}
		else if(pid==17) //SDT : contains the names of the services
		{
			while(get_ts_packet(ts_packet,auto_p->autoconf_temp_sdt))
			{
				ts_packet=NULL; // next call we only POP packets from the stack
				autoconf_read_sdt(auto_p->autoconf_temp_sdt->data_full,auto_p->autoconf_temp_sdt->len_full,auto_p->services);
			}
		}
		else if(pid==PSIP_PID && tune_p->fe_type==FE_ATSC) //PSIP : contains the names of the services
		{
			while(get_ts_packet(ts_packet,auto_p->autoconf_temp_psip))
			{
				ts_packet=NULL; // next call we only POP packets from the stack
				autoconf_read_psip(auto_p);
			}
		}
	}
	else if(auto_p->autoconfiguration==AUTOCONF_MODE_PIDS) //We have the channels and their PMT, we search the other pids
	{
		int ichan;
		for(ichan=0;ichan<MAX_CHANNELS;ichan++)
		{
			if((!chan_p->channels[ichan].autoconfigurated) &&(chan_p->channels[ichan].pmt_pid==pid)&& pid)
			{
				while((auto_p->autoconfiguration==AUTOCONF_MODE_PIDS)&&(chan_p->channels[ichan].pmt_packet)&&(get_ts_packet(ts_packet,chan_p->channels[ichan].pmt_packet)))
				{
					ts_packet=NULL; // next call we only POP packets from the stack
					//Now we have the PMT, we parse it
					if(autoconf_read_pmt(chan_p->channels[ichan].pmt_packet, &chan_p->channels[ichan], tune_p->card_dev_path, tune_p->tuner, chan_p->asked_pid, chan_p->number_chan_asked_pid, fds)==0)
					{
						log_pids(log_module,&chan_p->channels[ichan],ichan);

						chan_p->channels[ichan].autoconfigurated=1;

						//We parse the NIT before finishing autoconfiguration
						auto_p->autoconfiguration=AUTOCONF_MODE_NIT;
						for (ichan = 0; ichan < chan_p->number_of_channels; ichan++)
							if(!chan_p->channels[ichan].autoconfigurated)
								auto_p->autoconfiguration=AUTOCONF_MODE_PIDS;  //not finished we continue

						//if it's finished, we open the new descriptors and add the new filters
						if(auto_p->autoconfiguration!=AUTOCONF_MODE_PIDS)
						{
							autoconf_set_channel_filt(tune_p->card_dev_path, tune_p->tuner, chan_p, fds);
							//We free autoconf memory
							autoconf_freeing(auto_p);
							if(auto_p->autoconfiguration==AUTOCONF_MODE_NIT)
								log_message( log_module, MSG_DETAIL,"We search for the NIT\n");
							else
								autoconf_definite_end(chan_p, multi_p, unicast_vars);
						}
					}
				}
			}
		}
	}
	else if(auto_p->autoconfiguration==AUTOCONF_MODE_NIT) //We search the NIT
	{
		if(pid==16) //NIT : Network Information Table
		{
			while((auto_p->autoconfiguration==AUTOCONF_MODE_NIT)&&(get_ts_packet(ts_packet,auto_p->autoconf_temp_nit)))
			{
				ts_packet=NULL; // next call we only POP packets from the stack
				log_message( log_module, MSG_FLOOD,"New NIT\n");
				if(autoconf_read_nit(auto_p, chan_p->channels, chan_p->number_of_channels)==0)
				{
					auto_p->autoconfiguration=0;
					int ichan;
					char lcn[4];
					int len=MAX_NAME_LEN;
					for(ichan=0;ichan<MAX_CHANNELS;ichan++)
					{
						if(chan_p->channels[ichan].logical_channel_number)
						{
							sprintf(lcn,"%03d",chan_p->channels[ichan].logical_channel_number);
							mumu_string_replace(chan_p->channels[ichan].name,&len,0,"%lcn",lcn);
							sprintf(lcn,"%02d",chan_p->channels[ichan].logical_channel_number);
							mumu_string_replace(chan_p->channels[ichan].name,&len,0,"%2lcn",lcn);
						}
						else
						{
							mumu_string_replace(chan_p->channels[ichan].name,&len,0,"%lcn","");
							mumu_string_replace(chan_p->channels[ichan].name,&len,0,"%2lcn","");
						}
					}
					free(auto_p->autoconf_temp_nit);
					auto_p->autoconf_temp_nit=NULL;
					autoconf_definite_end(chan_p, multi_p, unicast_vars);
				}
			}
		}
	}
	pthread_mutex_unlock(&auto_p->lock);
	return get_interrupted();
}


/** @brief Autoconf function called periodically
 * This function check if we reached the timeout
 * if it's finished, we open the new descriptors and add the new filters
 */
int autoconf_poll(long now, auto_p_t *auto_p, mumu_chan_p_t *chan_p, tune_p_t *tune_p, multi_p_t *multi_p, fds_t *fds, unicast_parameters_t *unicast_vars, int server_id, void *scam_vars)
{
	int iRet=0;
	if(!auto_p->time_start_autoconfiguration)
		auto_p->time_start_autoconfiguration=now;
	else if (now-auto_p->time_start_autoconfiguration>AUTOCONFIGURE_TIME)
	{
		if(auto_p->autoconfiguration==AUTOCONF_MODE_PIDS)
		{
			log_message( log_module, MSG_WARN,"Not all the channels were configured before timeout\n");
			autoconf_set_channel_filt(tune_p->card_dev_path, tune_p->tuner, chan_p, fds);
			//We free autoconf memory
			autoconf_freeing(auto_p);
			auto_p->autoconfiguration=AUTOCONF_MODE_NIT;
			auto_p->time_start_autoconfiguration=now;
		}
		else if(auto_p->autoconfiguration==AUTOCONF_MODE_FULL)
		{
			log_message( log_module, MSG_WARN,"We were not able to get all the services, we continue with the partial service list\n");
			//This happend when we are not able to get all the services of the PAT,
			//We continue with the partial list of services
			auto_p->time_start_autoconfiguration=now;
			iRet = autoconf_finish_full(chan_p, auto_p, multi_p, tune_p, fds, unicast_vars, server_id, scam_vars);
		}
		else if(auto_p->autoconfiguration==AUTOCONF_MODE_NIT)
		{
			log_message( log_module, MSG_WARN,"Warning : No NIT found before timeout\n");
			autoconf_definite_end(chan_p, multi_p, unicast_vars);
			if(auto_p->autoconf_temp_nit)
			{
				free(auto_p->autoconf_temp_nit);
				auto_p->autoconf_temp_nit=NULL;
			}
			auto_p->autoconfiguration=0;
		}
	}
	return iRet;
}

