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
#include <pthread.h>

#include <fftw3.h>

#define UNUSED(x) (void)(x)
#define auto __auto_type
#define let __auto_type const

#define SWAP(x,y){ let tmp = x; x = y; y = tmp;}
size_t utime(){
  struct timeval tv;
  gettimeofday(&tv,NULL);
  return tv.tv_sec * 1e6 + tv.tv_usec;
}

typedef struct{
  float * samples;
  size_t count;
  size_t capacity;
}sample_buffer;

void sample_buffer_push(sample_buffer * obj, float * samples, size_t sample_count){
  if(obj->capacity < obj->count + sample_count){
    obj->capacity = (sample_count + obj->count);
    obj->samples = realloc(obj->samples, obj->capacity * sizeof(obj->samples[0]));
  }
  memcpy(obj->samples + obj->count, samples, sample_count * sizeof(samples[0]));
  obj->count += sample_count;
}
void sample_buffer_clear(sample_buffer * obj){
  obj->count = 0;
}

typedef jack_default_audio_sample_t sample_t;
typedef struct{
  jack_client_t *client;
  jack_port_t *output_port;
  int sample_rate;
  size_t current_sample;

  fftwf_complex * resample_filter1;
  float * resample_filter2;

  pthread_mutex_t swap_buffer_mutex;
  sample_buffer front_buffer;
  sample_buffer back_buffer;
  
  size_t sample_count;
  size_t start_time;

  size_t proc_start_time;
  size_t samples_processed;
  double scaling;
}sink_context;

int
sink_process (jack_nframes_t nframes, void *arg)
{

  sink_context * ctx = arg;
  ctx->samples_processed += nframes;
  if(ctx->proc_start_time == 0)
    ctx->proc_start_time = utime();
  sample_t *buffer = (sample_t *) jack_port_get_buffer (ctx->output_port, nframes);
  sample_buffer buf = ctx->front_buffer;
  size_t needed_samples = 0;
  if(buf.count > 0){
    double time_spent = (double)(utime() - ctx->start_time) * 1e-6;
    double est_sample_rate = (double)ctx->sample_count / time_spent;
    
    double delta = (double)(utime() - ctx->proc_start_time) * 1e-6;
    double est_own_sample_rate = ctx->samples_processed / delta;
    double scaling = est_sample_rate / est_own_sample_rate;
    ctx->scaling = scaling;
    needed_samples = (size_t) nframes * scaling * 1.0;
    if(needed_samples < ctx->front_buffer.count){
      bool usefft = false;
      buf = ctx->front_buffer;
      if(usefft == false ){
	for(size_t i = 0; i < nframes; i++){
	  double j = i * scaling;
	  size_t j1 = (size_t) floor(j);
	  size_t j2 = (size_t) ceil(j);
	  double r = j - j1;
	  // interpolate
	  buffer[i] = buf.samples[j1] * (1 - r) + buf.samples[j2] * r;
	  //printf("%f\n", buffer[i]);
	}
      }else{
	
	memcpy(ctx->resample_filter2, buf.samples, needed_samples * sizeof(buf.samples[0]));
	//printf("%i %i %f\n", needed_samples, nframes, scaling);
	// resample the buffer using fft.
	fftwf_plan plan1 = fftwf_plan_dft_r2c_1d(needed_samples, ctx->resample_filter2, ctx->resample_filter1, FFTW_ESTIMATE);
	//for(size_t i = 0; i < nframes / 2; i++){
	//  ctx->resample_filter1[i][0] = 0;
	//  ctx->resample_filter1[i][1] = 0;
	//}
	fftwf_plan plan2 = fftwf_plan_dft_c2r_1d(nframes, ctx->resample_filter1, ctx->resample_filter2, FFTW_ESTIMATE);
	memcpy(buffer, ctx->resample_filter2, nframes * sizeof(float));
	fftwf_execute(plan1);
	fftwf_execute(plan2);
	fftwf_destroy_plan(plan1);
	fftwf_destroy_plan(plan2);
      }
      //
    }else{
      printf("this happens..\n");
      needed_samples = 0;
    }
  }
  pthread_mutex_lock(&ctx->swap_buffer_mutex);
  ssize_t remaining_samples = ctx->front_buffer.count - needed_samples;

  memcpy(ctx->front_buffer.samples, ctx->front_buffer.samples + needed_samples, remaining_samples * sizeof(float));
  ctx->front_buffer.count = remaining_samples;
  sample_buffer_push(&ctx->front_buffer, ctx->back_buffer.samples, ctx->back_buffer.count);
  sample_buffer_clear(&ctx->back_buffer);

//SWAP(ctx->front_buffer, ctx->back_buffer);
  pthread_mutex_unlock(&ctx->swap_buffer_mutex);
	 
  return 0;
}

void run_sink(struct addrinfo * addr){


  const char *client_name = "UDP Sink";
  jack_status_t status;
  jack_client_t *client;
  jack_port_t *output_port;
  sink_context ctx = {0};
  bool connect_jack = true;
  if(connect_jack){
    if ((client = jack_client_open (client_name, JackNoStartServer, &status)) == 0) {
      printf ("jack server not running?\n");
      return;
    }
    output_port = jack_port_register (client, "out1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

    ctx.client = client;
    ctx.output_port = output_port;
    ctx.sample_rate = jack_get_sample_rate(client);
    ctx.resample_filter1 = fftwf_malloc(sizeof(fftwf_complex) * 1024 * 1024);
    ctx.resample_filter2 = fftwf_malloc(sizeof(float) * 1024 * 1024);

    jack_set_process_callback (client, sink_process, &ctx);

    if (jack_activate (client)) {
      printf ("cannot activate client");
      return;
    }
    printf("Jack connected..\n");
  }
  printf("Playing...\n");


  int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
  bind(udp_socket, addr->ai_addr, addr->ai_addrlen);
  size_t buffer_size = 1024 * 1024;
  int * buffer = malloc(sizeof(int) * buffer_size);
  float * tmp_buffer = malloc(sizeof(float) *buffer_size);
  int every = 10;
  while(true){
    ssize_t rcv_len;
    if( (rcv_len = recv(udp_socket, buffer, sizeof(int) * buffer_size, MSG_WAITALL)) > 0){
      //printf("Recieved%i\n", rcv_len);
      if(rcv_len == 0){
	continue;
      }
      size_t rcv_samples = rcv_len / 4;
      
      for(size_t i = 0; i < rcv_samples; i++){
	float v = (float) buffer[i] / (1024 * 128);
	UNUSED(v);
	tmp_buffer[i] = v;
	//printf("%f\n", v);
      }

      pthread_mutex_lock(&ctx.swap_buffer_mutex);
      sample_buffer_push(&ctx.back_buffer, tmp_buffer, rcv_samples);
      pthread_mutex_unlock(&ctx.swap_buffer_mutex);
      
      if(ctx.start_time == 0){
	ctx.start_time = utime();
      }

      ctx.sample_count += rcv_samples;
      if(every == 0){
	every = 100;
	double time_spent = (double)(utime() - ctx.start_time) * 1e-6;;
	printf("%i %i %f\nEst sample rate: %f samples/second\n back buffer size: %i\n",rcv_samples, ctx.sample_count, time_spent , (double)ctx.sample_count / time_spent, ctx.back_buffer.count);

	
	double delta = (double)(utime() - ctx.proc_start_time) * 1e-6;
	printf("Sample processing speed:  %f hz\n", ctx.samples_processed / delta);
	printf("Scaling: %f\n",ctx.scaling);
      }
      every--;
    }
  }
  
}



void run_source(struct addrinfo * addr){

  size_t sample_rate = 48000;
  size_t buffer_size = 128;

  int udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
  connect(udp_socket, addr->ai_addr, addr->ai_addrlen);
  size_t sample = 0;
  int * buffer = calloc(sizeof(int), buffer_size);
  size_t buffer_time_us = (buffer_size * 1000000) / sample_rate ;
  size_t last_buf = 0;
  printf("Buffer time: %i\n", buffer_time_us);
  while(true){
    // soft sampling
    if(last_buf > 0){
      size_t next_frame = last_buf + buffer_time_us;
      size_t sleep = next_frame - utime();
      if(sleep > 100){
	if(sleep > 1000)
	  sleep = 1000;
	usleep(sleep);
      }
    }
    if(last_buf < utime() - buffer_time_us){
      //printf("send..\n");
      last_buf = utime();
      for(size_t i = 0; i < buffer_size; i++){
	size_t _sample = sample++;
	double t = (double)_sample / sample_rate;
	double v = sin(t * 3.14 * 880) * 0.5;
	buffer[i] = (int)(v * 1024 * 64);
      }
      send(udp_socket, buffer, buffer_size * sizeof(int), 0);
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
