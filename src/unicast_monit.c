/*
 * MuMuDVB - Stream a DVB transport stream.
 *
 * (C) 2013 Brice DUBOST
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
 * @brief File for HTTP unicast monitoring functions
 * @author Brice DUBOST
 * @date 2013
 */


#include "unicast_http.h"
#include "unicast_queue.h"
#include "mumudvb.h"
#include "errors.h"
#include "log.h"
#include "dvb.h"
#include "tune.h"
#include "autoconf.h"
#ifdef ENABLE_CAM_SUPPORT
#include "cam.h"
#endif
#ifdef ENABLE_SCAM_SUPPORT
#include "scam_common.h"
#endif

static char *log_module="Unicast : ";


/** @brief Send a basic JSON file containig the list of streamed channels
 *
 * @param number_of_channels the number of channels
 * @param channels the channels array
 * @param Socket the socket on wich the information have to be sent
 */
int unicast_send_streamed_channels_list_js (int number_of_channels, mumudvb_channel_t *channels, int Socket)
{
	int curr_channel;
	unicast_client_t *unicast_client=NULL;
	int clients=0;

	struct unicast_reply* reply = unicast_reply_init();
	if (NULL == reply) {
		log_message( log_module, MSG_INFO,"Error when creating the HTTP reply\n");
		return -1;
	}

	unicast_reply_write(reply, "[");
	for (curr_channel = 0; curr_channel < number_of_channels; curr_channel++)
	{
		clients=0;
		unicast_client=channels[curr_channel].clients;
		while(unicast_client!=NULL)
		{
			unicast_client=unicast_client->chan_next;
			clients++;
		}
		unicast_reply_write(reply, "{\"number\":%d, \"lcn\":%d, \"name\":\"%s\", \"sap_group\":\"%s\", \"ip_multicast\":\"%s\", \"port_multicast\":%d, \"num_clients\":%d, \"scrambling_ratio\":%d, \"is_up\":%d, \"pcr_pid\":%d, \"pmt_version\":%d, ",
				curr_channel+1,
				channels[curr_channel].logical_channel_number,
				channels[curr_channel].name,
				channels[curr_channel].sap_group,
				channels[curr_channel].ip4Out,
				channels[curr_channel].portOut,
				clients,
				channels[curr_channel].ratio_scrambled,
				channels[curr_channel].streamed_channel,
				channels[curr_channel].pcr_pid,
				channels[curr_channel].pmt_version );

		unicast_reply_write(reply, "\"unicast_port\":%d, \"service_id\":%d, \"service_type\":\"%s\", \"pids_num\":%d, \n",
				channels[curr_channel].unicast_port,
				channels[curr_channel].service_id,
				service_type_to_str(channels[curr_channel].channel_type),
				channels[curr_channel].num_pids);
		unicast_reply_write(reply, "\"pids\":[");
		for(int i=0;i<channels[curr_channel].num_pids;i++)
			unicast_reply_write(reply, "{\"number\":%d, \"type\":\"%s\", \"language\":\"%s\"},\n",
					channels[curr_channel].pids[i],
					pid_type_to_str(channels[curr_channel].pids_type[i]),
					channels[curr_channel].pids_language[i]);
		reply->used_body -= 2; // dirty hack to erase the last comma
		unicast_reply_write(reply, "]");
		unicast_reply_write(reply, "},\n");

	}
	reply->used_body -= 2; // dirty hack to erase the last comma
	unicast_reply_write(reply, "]\n");

	unicast_reply_send(reply, Socket, 200, "application/json");

	if (0 != unicast_reply_free(reply)) {
		log_message( log_module, MSG_INFO,"Error when releasing the HTTP reply after sendinf it\n");
		return -1;
	}
	return 0;
}


/** @brief Send a basic JSON file containig the reception power
 *
 * @param Socket the socket on wich the information have to be sent
 */
int
unicast_send_signal_power_js (int Socket, strength_parameters_t *strengthparams)
{
	struct unicast_reply* reply = unicast_reply_init();
	if (NULL == reply)
	{
		log_message( log_module, MSG_INFO,"Error when creating the HTTP reply\n");
		return -1;
	}

	unicast_reply_write(reply, "{\"ber\":%d, \"strength\":%d, \"snr\":%d, \"ub\":%d}\n", strengthparams->ber,strengthparams->strength,strengthparams->snr,strengthparams->ub);

	unicast_reply_send(reply, Socket, 200, "application/json");

	if (0 != unicast_reply_free(reply)) {
		log_message( log_module, MSG_INFO,"Error when releasing the HTTP reply after sendinf it\n");
		return -1;
	}
	return 0;
}



/** @brief Send a basic JSON file containig the channel traffic
 *
 * @param number_of_channels the number of channels
 * @param channels the channels array
 * @param Socket the socket on wich the information have to be sent
 */
int
unicast_send_channel_traffic_js (int number_of_channels, mumudvb_channel_t *channels, int Socket)
{
	int curr_channel;
	extern long real_start_time;

	struct unicast_reply* reply = unicast_reply_init();
	if (NULL == reply) {
		log_message( log_module, MSG_INFO,"Error when creating the HTTP reply\n");
		return -1;
	}

	if ((time((time_t*)0L) - real_start_time) >= 10) //10 seconds for the traffic calculation to be done
	{
		unicast_reply_write(reply, "[");
		for (curr_channel = 0; curr_channel < number_of_channels; curr_channel++)
			unicast_reply_write(reply, "{\"number\":%d, \"name\":\"%s\", \"traffic\":%.2f},\n", curr_channel+1, channels[curr_channel].name, channels[curr_channel].traffic);
		reply->used_body -= 2; // dirty hack to erase the last comma
		unicast_reply_write(reply, "]\n");
	}

	unicast_reply_send(reply, Socket, 200, "application/json");

	if (0 != unicast_reply_free(reply)) {
		log_message( log_module, MSG_INFO,"Error when releasing the HTTP reply after sendinf it\n");
		return -1;
	}
	return 0;
}


/** @brief Send a full XML state of the mumudvb instance
 *
 * @param number_of_channels the number of channels
 * @param channels the channels array
 * @param Socket the socket on wich the information have to be sent
 * @param fds the frontend device structure
 */
int
unicast_send_xml_state (unicast_parameters_t* unicast_vars, int number_of_channels, mumudvb_channel_t* channels, int Socket, strength_parameters_t* strengthparams, auto_p_t* auto_p, void* cam_p_v, void* scam_vars_v)
{
#ifndef ENABLE_CAM_SUPPORT
	(void) cam_p_v; //to make compiler happy
#else
	cam_p_t *cam_p=(cam_p_t *)cam_p_v;
#endif

#ifndef ENABLE_SCAM_SUPPORT
	(void) scam_vars_v; //to make compiler happy
#else
	scam_parameters_t *scam_vars=(scam_parameters_t *)scam_vars_v;
#endif
	// Prepare the HTTP reply
	struct unicast_reply* reply = unicast_reply_init();
	if (NULL == reply) {
		log_message( log_module, MSG_INFO,"Error when creating the HTTP reply\n");
		return -1;
	}

	// Date time formatting
	time_t rawtime;
	time (&rawtime);
	char sdatetime[25];
	snprintf(sdatetime,25,"%s",ctime(&rawtime));

	// XML header
	unicast_reply_write(reply, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");

	// Starting XML content
	unicast_reply_write(reply, "<mumudvb card=\"%d\" frontend=\"%d\">\n",strengthparams->tune_p->card,strengthparams->tune_p->tuner);

	// Mumudvb information
	unicast_reply_write(reply, "\t<global_version><![CDATA[%s]]></global_version>\n",VERSION);
	unicast_reply_write(reply, "\t<global_pid>%d</global_pid>\n",getpid ());

	// Uptime
	extern long real_start_time;
	struct timeval tv;
	gettimeofday (&tv, (struct timezone *) NULL);
	unicast_reply_write(reply, "\t<global_uptime>%d</global_uptime>\n",(tv.tv_sec - real_start_time));

	// Frontend setup
	unicast_reply_write(reply, "\t<frontend_name><![CDATA[%s]]></frontend_name>\n",strengthparams->tune_p->fe_name);
	unicast_reply_write(reply, "\t<frontend_tuned>%d</frontend_tuned>\n",strengthparams->tune_p->card_tuned);
	if (strengthparams->tune_p->fe_type==FE_QPSK) // Do some test for always showing frequency in kHz
	{
		unicast_reply_write(reply, "\t<frontend_frequency>%d</frontend_frequency>\n",strengthparams->tune_p->freq);
		unicast_reply_write(reply, "\t<frontend_satnumber>%d</frontend_satnumber>\n",strengthparams->tune_p->sat_number);
	}
	else
		unicast_reply_write(reply, "\t<frontend_frequency>%d</frontend_frequency>\n",(strengthparams->tune_p->freq)/1000);
	if (strengthparams->tune_p->pol==0)
		unicast_reply_write(reply, "\t<frontend_polarization><![CDATA[-]]></frontend_polarization>\n");
	else
		unicast_reply_write(reply, "\t<frontend_polarization><![CDATA[%c]]></frontend_polarization>\n",strengthparams->tune_p->pol);
	unicast_reply_write(reply, "\t<frontend_symbolrate>%d</frontend_symbolrate>\n",strengthparams->tune_p->srate);

	// Frontend type
	char fetype[10]="Unkonwn";
	if (strengthparams->tune_p->fe_type==FE_OFDM)
#ifdef DVBT2
		if (strengthparams->tune_p->delivery_system==SYS_DVBT2)
			snprintf(fetype,10,"DVB-T2");
		else
			snprintf(fetype,10,"DVB-T");
#else
	snprintf(fetype,10,"DVB-T");
#endif
	if (strengthparams->tune_p->fe_type==FE_QAM)  snprintf(fetype,10,"DVB-C");
	if (strengthparams->tune_p->fe_type==FE_ATSC) snprintf(fetype,10,"ATSC");
	if (strengthparams->tune_p->fe_type==FE_QPSK)
	{
#if DVB_API_VERSION >= 5
		if (strengthparams->tune_p->delivery_system==SYS_DVBS2)
			snprintf(fetype,10,"DVB-S2");
#ifdef DVBT2
		else if (strengthparams->tune_p->delivery_system==SYS_DVBT2)
			snprintf(fetype,10,"DVB-T2");
#endif
		else
			snprintf(fetype,10,"DVB-S");
#else
		snprintf(fetype,10,"DVB-S");
#endif
	}
	unicast_reply_write(reply, "\t<frontend_system><![CDATA[%s]]></frontend_system>\n",fetype);

	// Frontend status
	char SCVYL[6]="-----";
	if (strengthparams->festatus & FE_HAS_SIGNAL)  SCVYL[0]=83; // S
	if (strengthparams->festatus & FE_HAS_CARRIER) SCVYL[1]=67; // C
	if (strengthparams->festatus & FE_HAS_VITERBI) SCVYL[2]=86; // V
	if (strengthparams->festatus & FE_HAS_SYNC)    SCVYL[3]=89; // Y
	if (strengthparams->festatus & FE_HAS_LOCK)    SCVYL[4]=76; // L
	SCVYL[5]=0;
	unicast_reply_write(reply, "\t<frontend_status><![CDATA[%s]]></frontend_status>\n",SCVYL);

	// Frontend signal
	unicast_reply_write(reply, "\t<frontend_ber>%d</frontend_ber>\n",strengthparams->ber);
	unicast_reply_write(reply, "\t<frontend_signal>%d</frontend_signal>\n",strengthparams->strength);
	unicast_reply_write(reply, "\t<frontend_snr>%d</frontend_snr>\n",strengthparams->snr);
	unicast_reply_write(reply, "\t<frontend_ub>%d</frontend_ub>\n",strengthparams->ub);
	unicast_reply_write(reply, "\t<ts_discontinuities>%d</ts_discontinuities>\n",strengthparams->ts_discontinuities);


	// Autoconfiguration state
	if (auto_p->autoconfiguration!=0)
		unicast_reply_write(reply, "\t<autoconf_end>%d</autoconf_end>\n",0);
	else
		unicast_reply_write(reply, "\t<autoconf_end>%d</autoconf_end>\n",1);

	// CAM information
#ifdef ENABLE_CAM_SUPPORT
	unicast_reply_write(reply, "\t<cam_support>%d</cam_support>\n",cam_p->cam_support);
	unicast_reply_write(reply, "\t<cam_number>%d</cam_number>\n",cam_p->cam_number);
	unicast_reply_write(reply, "\t<cam_menustring><![CDATA[%s]]></cam_menustring>\n",cam_p->cam_menu_string.string);
	unicast_reply_write(reply, "\t<cam_initialized>%d</cam_initialized>\n",cam_p->ca_resource_connected);
#else
	unicast_reply_write(reply, "\t<cam_support>%d</cam_support>\n",0);
	unicast_reply_write(reply, "\t<cam_number>%d</cam_number>\n",0);
	unicast_reply_write(reply, "\t<cam_menustring><![CDATA[No CAM support]]></cam_menustring>\n");
	unicast_reply_write(reply, "\t<cam_initialized>%d</cam_initialized>\n",0);
#endif

	// SCAM information
#ifdef ENABLE_SCAM_SUPPORT
	unicast_reply_write(reply, "\t<scam_support>%d</scam_support>\n",scam_vars->scam_support);
#else
        unicast_reply_write(reply, "\t<scam_support>%d</scam_support>\n",0);
#endif
#ifdef ENABLE_SCAM_DESCRAMBLER_SUPPORT
	if (scam_vars->scam_support) {
		unicast_reply_write(reply, "\t<ring_buffer_default_size>%u</ring_buffer_default_size>\n",scam_vars->ring_buffer_default_size);
		unicast_reply_write(reply, "\t<decsa_default_delay>%u</decsa_default_delay>\n",scam_vars->decsa_default_delay);
		unicast_reply_write(reply, "\t<send_default_delay>%u</send_default_delay>\n",scam_vars->send_default_delay);
	}
	else {
		unicast_reply_write(reply, "\t<ring_buffer_default_size>%u</ring_buffer_default_size>\n",0);
		unicast_reply_write(reply, "\t<decsa_default_delay>%u</decsa_default_delay>\n",0);
		unicast_reply_write(reply, "\t<send_default_delay>%u</send_default_delay>\n",0);
	}
#else
	unicast_reply_write(reply, "\t<ring_buffer_default_size>%u</ring_buffer_default_size>\n",0);
	unicast_reply_write(reply, "\t<decsa_default_delay>%u</decsa_default_delay>\n",0);
	unicast_reply_write(reply, "\t<send_default_delay>%u</send_default_delay>\n",0);
#endif

	// Channels list
	int curr_channel;
	for (curr_channel = 0; curr_channel < number_of_channels; curr_channel++)
	{
		unicast_reply_write(reply, "\t<channel number=\"%d\" is_up=\"%d\">\n",curr_channel+1,channels[curr_channel].streamed_channel);
		unicast_reply_write(reply, "\t\t<lcn>%d</lcn>\n",channels[curr_channel].logical_channel_number);
		unicast_reply_write(reply, "\t\t<name><![CDATA[%s]]></name>\n",channels[curr_channel].name);
		unicast_reply_write(reply, "\t\t<service_type type=\"%d\"><![CDATA[%s]]></service_type>\n",channels[curr_channel].channel_type,service_type_to_str(channels[curr_channel].channel_type));
		if (channels[curr_channel].portOut==0)
			unicast_reply_write(reply, "\t\t<ip_multicast><![CDATA[0.0.0.0]]></ip_multicast>\n");
		else
			unicast_reply_write(reply, "\t\t<ip_multicast><![CDATA[%s]]></ip_multicast>\n",channels[curr_channel].ip4Out);
		unicast_reply_write(reply, "\t\t<port_multicast>%d</port_multicast>\n",channels[curr_channel].portOut);
		unicast_reply_write(reply, "\t\t<traffic>%.0f</traffic>\n",channels[curr_channel].traffic);
		unicast_reply_write(reply, "\t\t<ratio_scrambled>%d</ratio_scrambled>\n",channels[curr_channel].ratio_scrambled);
		unicast_reply_write(reply, "\t\t<service_id>%d</service_id>\n",channels[curr_channel].service_id);
		unicast_reply_write(reply, "\t\t<pmt_pid>%d</pmt_pid>\n",channels[curr_channel].pmt_pid);
		unicast_reply_write(reply, "\t\t<pmt_version>%d</pmt_version>\n",channels[curr_channel].pmt_version);
		unicast_reply_write(reply, "\t\t<pcr_pid>%d</pcr_pid>\n",channels[curr_channel].pcr_pid);
		unicast_reply_write(reply, "\t\t<unicast_port>%d</unicast_port>\n",channels[curr_channel].unicast_port);
		// SCAM information
#ifdef ENABLE_SCAM_SUPPORT
		if (scam_vars->scam_support) {
			unicast_reply_write(reply, "\t\t<scam descrambled=\"%d\">\n",channels[curr_channel].scam_support);
#ifdef ENABLE_SCAM_DESCRAMBLER_SUPPORT
			if (channels[curr_channel].scam_support) {
				unsigned int ring_buffer_num_packets = 0;

				if (channels[curr_channel].ring_buf) {
					pthread_mutex_lock(&channels[curr_channel].ring_buf->lock);
					ring_buffer_num_packets = channels[curr_channel].ring_buf->to_descramble + channels[curr_channel].ring_buf->to_send;
					pthread_mutex_unlock(&channels[curr_channel].ring_buf->lock);
				}

				unicast_reply_write(reply, "\t\t\t<ring_buffer_size>%u</ring_buffer_size>\n",channels[curr_channel].ring_buffer_size);
				unicast_reply_write(reply, "\t\t\t<decsa_delay>%u</decsa_delay>\n",channels[curr_channel].decsa_delay);
				unicast_reply_write(reply, "\t\t\t<send_delay>%u</send_delay>\n",channels[curr_channel].send_delay);
				unicast_reply_write(reply, "\t\t\t<num_packets>%u</num_packets>\n",ring_buffer_num_packets);
			}
#endif
			unicast_reply_write(reply, "\t\t</scam>\n");
		}
#endif
		unicast_reply_write(reply, "\t\t<ca_sys>\n");
		for(int i=0;i<32;i++)
			if(channels[curr_channel].ca_sys_id[i]!=0)
				unicast_reply_write(reply, "\t\t\t<ca num=\"%d\"><![CDATA[%s]]></ca>\n",channels[curr_channel].ca_sys_id[i],ca_sys_id_to_str(channels[curr_channel].ca_sys_id[i]));
		unicast_reply_write(reply, "\t\t</ca_sys>\n");
		unicast_reply_write(reply, "\t\t<pids>\n");
		for(int i=0;i<channels[curr_channel].num_pids;i++)
			unicast_reply_write(reply, "\t\t\t<pid number=\"%d\" language=\"%s\" scrambled=\"%d\"><![CDATA[%s]]></pid>\n", channels[curr_channel].pids[i], channels[curr_channel].pids_language[i], channels[curr_channel].pids_scrambled[i], pid_type_to_str(channels[curr_channel].pids_type[i]));
		unicast_reply_write(reply, "\t\t</pids>\n");
		unicast_reply_write(reply, "\t</channel>\n");
	}


  unicast_reply_write(reply, "\t<users count=\"%d\">\n", (unicast_vars?unicast_vars->client_number:0));
  unicast_client_t *client=unicast_vars->clients;
  while(client!=NULL) {
      unicast_reply_write(reply, "\t<user socket=\"%d\" ip=\"%s:%d\" asked_channel=\"%d\" sid=\"%d\" channel_name=\"%s\">\n", client->Socket, inet_ntoa(client->SocketAddr.sin_addr), client->SocketAddr.sin_port, client->askedChannel, (client->chan_ptr?client->chan_ptr->service_id:-1), (client->chan_ptr?client->chan_ptr->name:"NA"));
      unicast_reply_write(reply, "\t</user>\n");
      client=client->next;
  }
  unicast_reply_write(reply, "\t</users>\n");

	// Ending XML content
	unicast_reply_write(reply, "</mumudvb>\n");

	unicast_reply_send(reply, Socket, 200, "application/xml; charset=UTF-8");

	// End of HTTP reply
	if (0 != unicast_reply_free(reply)) {
		log_message( log_module, MSG_INFO,"Error when releasing the HTTP reply after sendinf it\n");
		return -1;
	}
	return 0;
}

/** @brief Return the last MMI menu sent by CAM
 *
 * @param Socket the socket on wich the information have to be sent
 */
int
unicast_send_cam_menu (int Socket, void *cam_p_v)
{
#ifndef ENABLE_CAM_SUPPORT
	(void) cam_p_v; //to make compiler happy
#else
	cam_p_t *cam_p=(cam_p_t *)cam_p_v;
#endif
	struct unicast_reply* reply = unicast_reply_init();
	if (NULL == reply)
	{
		log_message( log_module, MSG_INFO,"Unicast : Error when creating the HTTP reply\n");
		return -1;
	}

	// UTF-8 Byte Order Mark (BOM)
	unicast_reply_write(reply, "\xef\xbb\xbf");

	// Date time formatting
	time_t rawtime;
	time (&rawtime);
	char sdatetime[25];
	snprintf(sdatetime,25,"%s",ctime(&rawtime));

	// XML header
	unicast_reply_write(reply, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");

	// Starting XML content
	unicast_reply_write(reply, "<menu>\n");

#ifdef ENABLE_CAM_SUPPORT
	// Sending the last menu if existing
	if (cam_p->ca_resource_connected!=0)
	{
		if (cam_p->cam_menulist_str.length>0)
		{
			unicast_reply_write(reply, "%s",cam_p->cam_menulist_str.string);
		}
		else
		{
			unicast_reply_write(reply, "\t<datetime><![CDATA[%s]]></datetime>\n",sdatetime);
			unicast_reply_write(reply, "\t<cammenustring><![CDATA[%s]]></cammenustring>\n",cam_p->cam_menu_string.string);
			unicast_reply_write(reply, "\t<object><![CDATA[NONE]]></object>\n");
			unicast_reply_write(reply, "\t<title><![CDATA[No menu to display]]></title>\n");
		}
	}
	else
	{
		unicast_reply_write(reply, "\t<datetime><![CDATA[%s]]></datetime>\n",sdatetime);
		unicast_reply_write(reply, "\t<object><![CDATA[NONE]]></object>\n");
		unicast_reply_write(reply, "\t<title><![CDATA[CAM not initialized!]]></title>\n");
	}
#else
	unicast_reply_write(reply, "\t<datetime><![CDATA[%s]]></datetime>\n",sdatetime);
	unicast_reply_write(reply, "\t<object><![CDATA[NONE]]></object>\n");
	unicast_reply_write(reply, "\t<title><![CDATA[Compiled without CAM support]]></title>\n");
#endif

	// Ending XML content
	unicast_reply_write(reply, "</menu>\n");

	// Cleaning all non acceptable characters for pseudo UTF-8 (in fact, US-ASCII) - Skipping BOM and last zero character
	unsigned char c;
	int j;
	for (j=3; j<reply->used_body; j++)
	{
		c=reply->buffer_body[j];
		if ((c<32 || c>127) && c!=9 && c!=10 && c!=13)
			reply->buffer_body[j]=32;
	}
	unicast_reply_send(reply, Socket, 200, "application/xml; charset=UTF-8");

	// End of HTTP reply
	if (0 != unicast_reply_free(reply)) {
		log_message( log_module, MSG_INFO,"Unicast : Error when releasing the HTTP reply after sendinf it\n");
		return -1;
	}
	return 0;
}

/** @brief Send an action to the CAM MMI menu
 *
 * @param Socket the socket on wich the information have to be sent
 */
int
unicast_send_cam_action (int Socket, char *Key, void *cam_p_v)
{
#ifndef ENABLE_CAM_SUPPORT
	(void) cam_p_v; //to make compiler happy
#else
	cam_p_t *cam_p=(cam_p_t *)cam_p_v;
#endif
	struct unicast_reply* reply = unicast_reply_init();
	if (NULL == reply)
	{
		log_message( log_module, MSG_INFO,"Unicast : Error when creating the HTTP reply\n");
		return -1;
	}

	// UTF-8 Byte Order Mark (BOM)
	unicast_reply_write(reply, "\xef\xbb\xbf");

	// Date time formatting
	time_t rawtime;
	time (&rawtime);
	char sdatetime[25];
	snprintf(sdatetime,25,"%s",ctime(&rawtime));

	// XML header
	unicast_reply_write(reply, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");

	// Starting XML content
	unicast_reply_write(reply, "<action>\n");
	unicast_reply_write(reply, "\t<datetime><![CDATA[%.24s]]></datetime>\n",ctime(&rawtime));
	unicast_reply_write(reply, "\t<key><![CDATA[%c]]></key>\n",*Key);

#ifdef ENABLE_CAM_SUPPORT
	// Check if valid action to be done [0-9] and 'M' and 'C' and 'O'
	int iKey=(int)*Key;
	if ((iKey>=48 && iKey<=57) || iKey==77 || iKey==67 || iKey==79)
	{
		// Check if CAM is initialized
		if (cam_p->ca_resource_connected!=0)
		{
			// Disable auto response from now (as a manual action is asked)
			cam_p->cam_mmi_autoresponse=0;
			// Numbers for MENU/LIST answer
			if (cam_p->mmi_state==MMI_STATE_MENU && iKey>=48 && iKey<=57)
			{
				log_message( log_module,  MSG_INFO, "Send CAM MENU key number %d\n",iKey-48);
				en50221_app_mmi_menu_answ(cam_p->stdcam->mmi_resource, cam_p->stdcam->mmi_session_number, iKey-48);
				cam_p->mmi_state=MMI_STATE_OPEN;
			}
			// 'M' = ask the menu - Always possible
			if (iKey==77)
			{
				log_message( log_module,  MSG_INFO, "Ask CAM to enter MENU\n");
				en50221_app_ai_entermenu(cam_p->stdcam->ai_resource, cam_p->stdcam->ai_session_number);
				cam_p->mmi_state=MMI_STATE_OPEN;
			}
			// Numbers for ENQUIRY answer
			if (cam_p->mmi_state==MMI_STATE_ENQ && iKey>=48 && iKey<=57)
			{
				// We store the new key
				cam_p->mmi_enq_answer[cam_p->mmi_enq_entered]=iKey;
				cam_p->mmi_enq_entered++;
				log_message( log_module,  MSG_INFO, "Received CAM ENQUIRY key number %d (%d of %d expected)\n", iKey-48, cam_p->mmi_enq_entered, cam_p->mmi_enq_length);
				// Test if the expected length is received
				if (cam_p->mmi_enq_entered == cam_p->mmi_enq_length)
				{
					// We send the anwser
					log_message( log_module,  MSG_INFO, "Sending ENQUIRY answer to CAM (answer has the expected length of %d)\n",cam_p->mmi_enq_entered);
					en50221_app_mmi_answ(cam_p->stdcam->mmi_resource, cam_p->stdcam->mmi_session_number, MMI_ANSW_ID_ANSWER, (uint8_t*)cam_p->mmi_enq_answer, cam_p->mmi_enq_entered);
					cam_p->mmi_state=MMI_STATE_OPEN;
				}
			}
			// 'C' = send CANCEL as an ENQUIRY answer
			if (cam_p->mmi_state==MMI_STATE_ENQ && iKey==67)
			{
				log_message( log_module,  MSG_INFO, "Send CAM ENQUIRY key CANCEL\n");
				en50221_app_mmi_answ(cam_p->stdcam->mmi_resource, cam_p->stdcam->mmi_session_number, MMI_ANSW_ID_CANCEL, NULL, 0);
				cam_p->mmi_state=MMI_STATE_OPEN;
			}
			// OK
			unicast_reply_write(reply, "\t<result><![CDATA[OK]]></result>\n");
		}
		else
		{
			unicast_reply_write(reply, "\t<result><![CDATA[ERROR: CAM not initialized!]]></result>\n");
		}
	}
	else
	{
		unicast_reply_write(reply, "\t<result><![CDATA[ERROR: Unknown key!]]></result>\n");
	}
#else
	unicast_reply_write(reply, "\t<result><![CDATA[Compiled without CAM support]]></result>\n");
#endif

	// Ending XML content
	unicast_reply_write(reply, "</action>\n");

	// Cleaning all non acceptable characters for pseudo UTF-8 (in fact, US-ASCII) - Skipping BOM and last zero character
	unsigned char c;
	int j;
	for (j=3; j<reply->used_body; j++)
	{
		c=reply->buffer_body[j];
		if ((c<32 || c>127) && c!=9 && c!=10 && c!=13)
			reply->buffer_body[j]=32;
	}
	unicast_reply_send(reply, Socket, 200, "application/xml; charset=UTF-8");

	// End of HTTP reply
	if (0 != unicast_reply_free(reply)) {
		log_message( log_module, MSG_INFO,"Unicast : Error when releasing the HTTP reply after sendinf it\n");
		return -1;
	}
	return 0;
}

