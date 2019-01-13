#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <jack/jack.h>
#include <math.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>
#define UNUSED(x) (void)(x)
typedef jack_default_audio_sample_t sample_t;
typedef struct{
  jack_client_t *client;
  jack_port_t *output_port;
  int sample_rate;
  size_t current_sample;
  float * front_buffer;
  float * back_buffer;
}sink_context;

int
sink_process (jack_nframes_t nframes, void *arg)
{
  sink_context * ctx = arg;
  sample_t *buffer = (sample_t *) jack_port_get_buffer (ctx->output_port, nframes);
  
  for(jack_nframes_t i = 0; i < nframes; i++){
    memcpy(buffer, ctx->front_buffer, nframes * sizeof(float));
  }
  float * buf = ctx->front_buffer;
  ctx->front_buffer = ctx->back_buffer;
  ctx->back_buffer = buf;
  
  return 0;
}

void run_sink(struct addrinfo * addr){

  const char *client_name = "UDP Sink";
  jack_status_t status;
  jack_client_t *client;
  jack_port_t *output_port;
  if ((client = jack_client_open (client_name, JackNoStartServer, &status)) == 0) {
    printf ("jack server not running?\n");
    return;
  }
  output_port = jack_port_register (client, "out1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
  size_t buffer_size = jack_get_buffer_size(client);   
  sink_context ctx =
  {
    .client = client,
    .output_port = output_port,
    .sample_rate = jack_get_sample_rate(client),
    .front_buffer = calloc(sizeof(float), buffer_size),
    .back_buffer = calloc(sizeof(float), buffer_size),
    
  };
  jack_set_process_callback (client, sink_process, &ctx);

  if (jack_activate (client)) {
    printf ("cannot activate client");
    return;
  }
  printf("Playing...\n");


  int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
  bind(udp_socket, addr->ai_addr, addr->ai_addrlen);
  int * buffer = malloc(sizeof(int) * buffer_size);
  while(true){
    ssize_t rcv_len;
    if( (rcv_len = recv(udp_socket, buffer, sizeof(int) * buffer_size, MSG_WAITALL)) > 0){
      //printf("Recieved%i\n", rcv_len);
      if(rcv_len != (ssize_t)(buffer_size * sizeof(int))){
	printf("Unexpected error\n");
	return;
      }
      for(size_t i = 0; i < buffer_size; i++){
	float v = (float) buffer[i] / (1024 * 16);
	ctx.back_buffer[i] = v;
      }
    }
  }
  
}


size_t utime(){
  struct timeval tv;
  gettimeofday(&tv,NULL);
  return tv.tv_sec * 1e6 + tv.tv_usec;
}

void run_source(struct addrinfo * addr){

  const char *client_name = "UDP Sink";
  jack_status_t status;
  jack_client_t *client;
  if ((client = jack_client_open (client_name, JackNoStartServer, &status)) == 0) {
    printf ("jack server not running?\n");
    return;
  }
  // jack client just used to make sure we have the same sample rate and buffer size.
  size_t sample_rate = jack_get_sample_rate(client);
  size_t buffer_size = jack_get_buffer_size(client); 

  int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
  connect(udp_socket, addr->ai_addr, addr->ai_addrlen);
  size_t sample = 0;
  int * buffer = calloc(sizeof(int), buffer_size);
  size_t buffer_time_us = (buffer_size * 1000000) / sample_rate ;
  size_t last_buf = 0;
  while(true){
    // soft sampling
    
    if(last_buf < utime() - buffer_time_us){
      //printf("send..\n");
      last_buf = utime();
      for(size_t i = 0; i < buffer_size; i++){
	size_t _sample = sample++;
	double t = (double)_sample / sample_rate;
	double v = sin(t * 3.14 * 440 * sqrt(t)) * 0.5;
	buffer[i] = (int)(v * 1024 * 16); //very stupid encoding.
	send(udp_socket, buffer, buffer_size * sizeof(int), 0);
      }
    }
    
  }
}

int main(int argc, const char ** argv){
  bool sink = false;
  bool source = false;
  const char * addr;
  for(int i = 0; i < argc; i++){
    if(strcmp(argv[i], "--sink") == 0){
      sink =true;
    }else if(strcmp(argv[i], "--source") == 0){
      source = true;
    }
  }
  
  addr = argv[argc - 2];
  const char * port = argv[argc - 1];

   struct addrinfo hints;
   struct addrinfo *result;//, *rp;
   int s;
   //int sfd, s;
   //struct sockaddr_storage peer_addr;
   //socklen_t peer_addr_len;
   //ssize_t nread;
   
   memset(&hints, 0, sizeof(struct addrinfo));
   hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
   hints.ai_socktype = SOCK_DGRAM; /* Datagram socket */
   hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
   hints.ai_protocol = 0;          /* Any protocol */
   hints.ai_canonname = NULL;
   hints.ai_addr = NULL;
   hints.ai_next = NULL;
   
   s = getaddrinfo(addr,port , &hints, &result);
   if (s != 0) {
     fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
     exit(EXIT_FAILURE);
   }
  
  if(sink == source){
    printf("run [--sink|--source] addr\n");
    return 1;
  }
  if(sink){
    run_sink(result);
  }else{
    run_source(result);
  }

   
  
  return 0;
}
