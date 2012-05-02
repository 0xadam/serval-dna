/*
Copyright (C) 2010-2012 Paul Gardner-Stephen, Serval Project.
 
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
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/*
  Android does unix domain sockets, but only stream sockets, not datagram sockets.
  So we need a separate monitor interface for Android. A bit of a pain, but in
  fact it lets us make a very Android/Java-friendly interface, without any binary
  data structures (except for a binary extent for an audio sample block).
*/

#include "serval.h"
#include <sys/stat.h>

/* really shouldn't need more than 2:
   1 for rhizome
   1 for VoMP call 
   but spares never hurt, and the cost is low
*/
#define MONITOR_LINE_LENGTH 80
#define MONITOR_DATA_SIZE MAX_AUDIO_BYTES
struct monitor_context {
#define MONITOR_VOMP (1<<0)
#define MONITOR_RHIZOME (1<<1)
  int flags;
  int socket;
  char line[MONITOR_LINE_LENGTH];
  int line_length;
#define MONITOR_STATE_COMMAND 1
#define MONITOR_STATE_DATA 2
  int state;
  unsigned char buffer[MONITOR_DATA_SIZE];
  int data_expected;
  int data_offset;
  int sample_codec;
  int sample_call_session_token;
};

#define MAX_MONITOR_SOCKETS 8
int monitor_socket_count=0;
struct monitor_context monitor_sockets[MAX_MONITOR_SOCKETS];

int monitor_process_command(int index,char *cmd);
int monitor_process_data(int index);

int monitor_named_socket=-1;
int monitor_setup_sockets()
{
  struct sockaddr_un name;
  int len;
  
  name.sun_family = AF_UNIX;
  
  if (monitor_named_socket==-1) {
    if (!form_serval_instance_path(&name.sun_path[0], 100, "monitor.socket")) {
      return WHY("Cannot construct name of unix domain socket.");
    }
    unlink(&name.sun_path[0]);
    len = 0+strlen(&name.sun_path[0]) + sizeof(name.sun_family)+1;
    monitor_named_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (monitor_named_socket>-1) {
      int dud=0;
      int r=bind(monitor_named_socket, (struct sockaddr *)&name, len);
      if (r) { dud=1; r=0; WHY("bind() of named unix domain socket failed"); }
      if (dud) {
	close(monitor_named_socket);
	monitor_named_socket=-1;
	WHY("Could not open named unix domain socket.");
      }

      int send_buffer_size=64*1024;    
      int res = setsockopt(monitor_named_socket, SOL_SOCKET, SO_RCVBUF, 
		       &send_buffer_size, sizeof(send_buffer_size));
      if (res) WHYF("setsockopt() failed: errno=%d",errno);
      res = fcntl(monitor_named_socket,F_SETFL, O_NONBLOCK);
      if (res) WHYF("fcntl(NONBLOCK) on monitor accept socket failed: errno=%d",
		    errno);
    }
  }

  return 0;
  
}

int monitor_get_fds(struct pollfd *fds,int *fdcount,int fdmax)
{
  /* Make sure sockets are open */
  monitor_setup_sockets();

  if ((*fdcount)>=fdmax) return -1;
  if (monitor_named_socket>-1)
    {
      if (debug&DEBUG_IO) {
	fprintf(stderr,
		"Monitor named unix domain socket is poll() slot #%d (fd %d)\n",
		*fdcount,monitor_named_socket);
      }
      fds[*fdcount].fd=monitor_named_socket;
      fds[*fdcount].events=POLLIN;
      (*fdcount)++;
    }

  int i;
  for(i=0;i<monitor_socket_count;i++) {
    if ((*fdcount)>=fdmax) return -1;
    if (debug&DEBUG_IO) {
      fprintf(stderr,
	      "Monitor named unix domain client socket is poll() slot #%d (fd %d)\n",
	      *fdcount,monitor_sockets[i].socket);
      }
      fds[*fdcount].fd=monitor_sockets[i].socket;
      fds[*fdcount].events=POLLIN;
      (*fdcount)++;
  }

  return 0;
}

int monitor_poll()
{
  int s;
  struct sockaddr ignored_address;
  socklen_t ignored_length=sizeof(ignored_address);

  /* Check for new connections */
  while((s=accept(monitor_named_socket,&ignored_address,&ignored_length))>-1) {
    int res = fcntl(s,F_SETFL, O_NONBLOCK);
    if (res) close(s); 
    else if (monitor_socket_count>=MAX_MONITOR_SOCKETS) {
      write(s,"CLOSE:All sockets busy\n",strlen("CLOSE:All sockets busy\n"));
      close(s);
    } else {
      struct monitor_context *c=&monitor_sockets[monitor_socket_count];
      c->line_length=0;
      c->state=MONITOR_STATE_COMMAND;
      monitor_socket_count++;
      write(s,"MONITOR:You are talking to servald\n",
	    strlen("MONITOR:You are talking to servald\n"));
    }

    ignored_length=sizeof(ignored_address);
  }

  /* Read from any open connections */
  int i;
  for(i=0;i<monitor_socket_count;i++) {
  nextInSameSlot:
    errno=0;
    int bytes;
    struct monitor_context *c=&monitor_sockets[monitor_socket_count];

    switch(c->state) {
    case MONITOR_STATE_COMMAND:
      bytes=1;
      while(bytes==1) {
	if (c->line_length>=MONITOR_LINE_LENGTH) {
	  /* line too long */
	  c->line[MONITOR_LINE_LENGTH-1]=0;
	  monitor_process_command(i,c->line);
	  break;
	}
	bytes=read(c->socket,&c->line[c->line_length],1);
	if (bytes==-1) {
	  switch(errno) {
	  case EAGAIN: case EINTR: 
	    /* transient errors */
	  default:
	    /* all other errors; close socket */
	    close(c->socket);
	    if (i==monitor_socket_count-1) {
	      monitor_socket_count--;
	      continue;
	    } else {
	      bcopy(&monitor_sockets[monitor_socket_count-1],
		    &monitor_sockets[i],
		    sizeof(struct monitor_context));
	      monitor_socket_count--;
	      goto nextInSameSlot;
	    }
	  }
	}
	c->line_length+=bytes;
	if (c->line[c->line_length-1]=='\n') {
	  /* got command */
	  c->line[c->line_length]=0; /* trim new line for easier parsing */
	  monitor_process_command(i,c->line);
	  break;
	}
      }
      break;
    case MONITOR_STATE_DATA:
      bytes=read(c->socket,
		 &c->buffer[c->data_offset],
		 c->data_expected-c->data_offset);
      if (bytes==-1) {
	switch(errno) {
	case EAGAIN: case EINTR: 
	  /* transient errors */
	default:
	  /* all other errors; close socket */
	    close(c->socket);
	    if (i==monitor_socket_count-1) {
	      monitor_socket_count--;
	      continue;
	    } else {
	      bcopy(&monitor_sockets[monitor_socket_count-1],
		    &monitor_sockets[i],
		    sizeof(struct monitor_context));
	      monitor_socket_count--;
	      goto nextInSameSlot;
	    }
	}
      } else {
	c->data_offset+=bytes;
	if (c->data_offset>=c->data_expected)
	  {
	    /* we have the binary data we were expecting. */
	    monitor_process_data(i);
	    c->state=MONITOR_STATE_COMMAND;
	  }
      }
      break;
    }
      
  }
  return 0;
}

int monitor_process_command(int index,char *cmd) 
{
  int callSessionToken,sampleType,bytes;

  struct monitor_context *c=&monitor_sockets[index];
  c->line_length=0;

  if (sscanf(cmd,cmd,"AUDIO:%x:%d:%d",
	     &callSessionToken,&sampleType,&bytes)==3)
    {
      /* Start getting sample */
      c->state=MONITOR_STATE_DATA;
      c->sample_call_session_token=callSessionToken;
      c->sample_codec=sampleType;
      c->data_expected=bytes;
      c->data_offset=0;
      return 0;
    }
  else if (!strcasecmp(cmd,"monitor vomp"))
    c->flags|=MONITOR_VOMP;
  else if (!strcasecmp(cmd,"ignore vomp"))
    c->flags&=~MONITOR_VOMP;
  else if (!strcasecmp(cmd,"monitor rhizome"))
    c->flags|=MONITOR_RHIZOME;
  else if (!strcasecmp(cmd,"ignore rhizome"))
    c->flags&=~MONITOR_RHIZOME;

  char msg[1024];
  snprintf(msg,1024,"MONITORSTATUS:%d\n",c->flags);
  write(c->socket,msg,strlen(msg));

  return 0;
}

int monitor_process_data(int index) 
{
  /* Called when we have received an entire data sample */
  struct monitor_context *c=&monitor_sockets[index];
  c->state=MONITOR_STATE_COMMAND;

  if (vomp_sample_size(c->sample_codec)!=c->data_offset)
    return WHY("Ignoring sample block of incorrect size");

  vomp_call_state *call=vomp_find_call_by_session(c->sample_call_session_token);
  if (!call) {
    write(c->socket,"ERROR:No such call\n",strlen("ERROR:No such call\n"));
    return -1;
  }

  overlay_mdp_frame mdp;
  mdp.packetTypeAndFlags=MDP_VOMPEVENT;
  mdp.vompevent.flags=VOMPEVENT_AUDIOPACKET;
  mdp.vompevent.call_session_token=c->sample_call_session_token;
  mdp.vompevent.audio_sample_codec=c->sample_codec;
  bcopy(&c->buffer[0],&mdp.vompevent.audio_bytes[0],
	vomp_sample_size(c->sample_codec));

  vomp_send_status(call,VOMP_TELLREMOTE|VOMP_SENDAUDIO,&mdp);

  return 0;
}