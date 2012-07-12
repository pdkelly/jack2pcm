/*
 *  jack2pcm.c
 *  Simple JACK raw PCM output client written by Paul Kelly for Radiomonitor Ltd.
 *  Copyright (C) 2008 Radiomonitor Ltd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>

#include <jack/jack.h>
#include <jack/ringbuffer.h>

#define PROG_NAME "jack2pcm"
#define PROG_VERSION "0.1"

#define DEFAULT_RINGBUFFER_SIZE 1048576 /* 1 MB */
#define READ_SAMPLES 128 /* Number of samples per port to read at a time 
                          * out of the JACK ringbuffer */

struct jack_params
{
    jack_client_t *client;
    int num_ports;
    char **input_ports; /* Names of ports the client should autoconnect to */
    jack_port_t **ports;
    jack_ringbuffer_t **ringbuffer;
    float **inbuff; /* Buffer to hold float audio data read from ringbuffer */
    short *outbuff; /* Buffer to hold converted signed 16-bit data */
    int ready; /* Set to true when all ports are connected and ready to process data */
    int swapbytes; /* 16-bit data should be byte-swapped before output */
};

int quiet;

static void init_jack(struct jack_params *, const char *, size_t);
static int capture_callback(jack_nframes_t, void *);
static void shutdown_callback(void *);
static void swap_bytes(char *, size_t);

int main(int argc, char **argv)
{
    char *client_name = NULL;
    size_t ringbuffer_size = DEFAULT_RINGBUFFER_SIZE;
    struct jack_params *params = calloc(1, sizeof(struct jack_params));
    int i;
   
    /* Parse command-line options */
    while ( (i = getopt(argc, argv, "n:b:xqh")) != -1 )
    {
        switch (i)
        {
            int len;

            case 'n': /* JACK client name */
                len = strlen(optarg) + 1;
                if( len > jack_client_name_size() )
                    len = jack_client_name_size();
                client_name = malloc(len);
                strncpy(client_name, optarg, len);
                break;
            case 'b': /* Ringbuffer size */
                if( strchr(optarg, 'k') || strchr(optarg, 'K') )
                    ringbuffer_size = atoi(optarg) * 1024; /* kB */
                else if( strchr(optarg, 'm') || strchr(optarg, 'M') )
                    ringbuffer_size = atoi(optarg) * 1048576; /* MB */
                else
                    ringbuffer_size = atoi(optarg);
                break;
            case 'x': /* Swap bytes */
                params->swapbytes = 1;
                break;
            case 'q': /* Quiet output */
                quiet = 1;
                break;
            case 'h': /* Help message */
            default:
                fprintf(stderr, "Usage: %s [-n <name>] [-b <size>] [-xqh] <port1> [<port2>]\n\n", PROG_NAME);
                fprintf(stderr, "   -n <name>        name for this JACK client (default %s-PID)\n", PROG_NAME);
                fprintf(stderr, "   -b <size>        set ringbuffer to this size in bytes per port (k or M\n");
                fprintf(stderr, "                    suffix accepted; default %d)\n", ringbuffer_size);
                fprintf(stderr, "   -x               swap byte order\n");
                fprintf(stderr, " <port1> & <port2>  autoconnect JACK client to these port(s)\n");
                fprintf(stderr, "   -q               Don't display progress information\n");
                fprintf(stderr, "   -h               display this usage message and exit\n");
                fprintf(stderr, "\n");
                exit(0);
        }
    }
   
    /* Set default JACK client name if not specified */
    if(!client_name)
    {
        client_name = malloc(strlen(PROG_NAME) + 50);
        sprintf(client_name, "%s-%d", PROG_NAME, getpid());
    }

    if(!quiet)
        fprintf(stderr, "Ringbuffer size is %d bytes\n", ringbuffer_size);

    if(optind >= argc)
    {
        fprintf(stderr, "ERROR: At least one port to connect to must be specified\n");
        exit(1);
    }
    /* Determine number and names of input ports from command-line
     * arguments remaining after getopt() parsing */
    for(i = optind; i < argc; i++)
    {
        params->input_ports = realloc(params->input_ports, 
                                      (params->num_ports + 1) * sizeof(char *));
        params->input_ports[params->num_ports] = malloc(strlen(argv[i] + 1));
        strcpy(params->input_ports[params->num_ports++], argv[i]);
    }

    /* Initialise JACK client and ringbuffer */
    init_jack(params, client_name, ringbuffer_size);

    /* Register callback functions */
    jack_set_process_callback(params->client, capture_callback, params);
    jack_on_shutdown(params->client, shutdown_callback, params);
   
    /* Allocate output buffers */
    params->inbuff = malloc(params->num_ports * sizeof(float *));
    for(i = 0; i < params->num_ports; i++)
        params->inbuff[i] = malloc(READ_SAMPLES * sizeof(float));
    params->outbuff = malloc(params->num_ports * READ_SAMPLES * sizeof(short));

    /* Activate JACK */
    if(jack_activate(params->client) != 0)
    {
        fprintf(stderr, "ERROR: Failed to activate JACK client.\n");
        exit(1);
    }

    /* Connect ports */
    for(i = 0; i < params->num_ports; ++i)
    {
        if( jack_connect(params->client, params->input_ports[i],
                         jack_port_name(params->ports[i])) != 0 )
        {
            fprintf(stderr, "ERROR: Failed to connect to input port %s\n",
                    params->input_ports[i]);
            exit(1);
        }
    }

    /* We're ready to process now all ports have been connected */
    params->ready = 1;
   
    /* Main Loop - read from ringbuffer and write to stdout */
    while(1)
    {
        size_t len;
        /* First of all read READ_SAMPLES samples from each port */
        for(i = 0; i < params->num_ports; i++)
        {
            static const size_t to_read = READ_SAMPLES * sizeof(float);

            while(jack_ringbuffer_read_space(params->ringbuffer[i]) < to_read)
            {
                /* Sleep for 1 millisecond */
                static const struct timespec tm = {0, 1000000};
                nanosleep(&tm, NULL);
            }

            len = jack_ringbuffer_read(params->ringbuffer[i], 
                                       (char *)params->inbuff[i], to_read);
            if(len != to_read)
            {
                fprintf(stderr, "ERROR: Only %d bytes read from ringbuffer %d."
                                "Should be %d\n", len, i, to_read);
                exit(1);
            }
        }

        /* Convert samples to signed 16-bit integers and interleave ports */
        for(i = 0; i < params->num_ports; i++)
        {
            int s;
            for(s = 0; s < READ_SAMPLES; s++)
                params->outbuff[s*params->num_ports + i] = params->inbuff[i][s] * 32768;
        }

        len = params->num_ports * READ_SAMPLES;

        /* Swap byte order if requested */
        if(params->swapbytes)
            swap_bytes((char *)params->outbuff, len * sizeof(short));
       
        /* Write converted samples to stdout */
        if( fwrite(params->outbuff, sizeof(short), len, stdout) != len )
        {
            fprintf(stderr, "ERROR writing to stdout: %s\n", strerror(errno));
            exit(1);
        }
    }       
   
    return 0;
}

/*
 * init_jack()
 * 
 * Register JACK client and ports, prepare audio ringbuffer.
 */
static void init_jack(struct jack_params *params, const char *client_name,
                      size_t ringbuffer_size)
{
    jack_status_t status;
    int i;

    /* Register JACK client */
    if( !(params->client = jack_client_open(client_name, JackNoStartServer, &status)) )
    {
        fprintf(stderr, "ERROR: Failed to register jack client: 0x%x\n", status);
        exit(1);
    }
    if(!quiet)
    {
        fprintf(stderr, "JACK client registered as '%s'\n",
                        jack_get_client_name(params->client));
        fprintf(stderr, "Sample rate is %d Hz\n",
                        jack_get_sample_rate(params->client));
    }

    /* Register ports */
    params->ports = malloc(params->num_ports * sizeof(jack_port_t *));
    for(i = 0; i < params->num_ports; i++)
    {
        char strnum[32];
        sprintf(strnum, "%d", i + 1);   
        if( !(params->ports[i] =  jack_port_register(params->client, strnum,
                                  JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) )
        {
            fprintf(stderr, "ERROR: Failed to register port %s.\n", strnum);
            exit(1);
        }
        if(!quiet)
            fprintf(stderr, "Port %s registered\n", jack_port_name(params->ports[i]));
    }   
    
    /* Create ring buffer(s) */
    params->ringbuffer = malloc(params->num_ports * sizeof(jack_ringbuffer_t *));
    for(i = 0; i < params->num_ports; i++)
    {
        if( !(params->ringbuffer[i] = jack_ringbuffer_create(ringbuffer_size)) )
        {
            fprintf(stderr, "ERROR: Failed to create ringbuffer.\n" );
            exit(1);
        }
    }

    return;   
}

/*
 * capture_callback()
 * 
 * Realtime callback function called by JACK when it has new audio data 
 * ready to process.
 * Does nothing if "ready" flag is not set to true, otherwise for each
 * port in turn reads audio data into a ringbuffer.
 */
static int capture_callback(jack_nframes_t nframes, void *data)
{
    struct jack_params *params = data;
    size_t to_write = sizeof(jack_default_audio_sample_t) * nframes;
    int p;
   
    if(!params->ready)
        /* Return straight away if we're not ready to process data */
        return 0;

    for (p = 0; p < params->num_ports; p++)
    {    
        size_t len = jack_ringbuffer_write_space(params->ringbuffer[p]);

        if(len > to_write)
        {
            char *buf = jack_port_get_buffer(params->ports[p], nframes);
            jack_ringbuffer_write(params->ringbuffer[p], buf, to_write);
        }
        else
        {
            fprintf(stderr, "ERROR: Ringbuffer %d has only %d bytes of free space.\n"
                            "Attempted to write %d bytes.\n", p, len, to_write);
            exit(1);
        }
    }

    return 0;
}

/*
 * shutdown_callback()
 * 
 * Called if JACK is exiting; simply exits program.
 */
static void shutdown_callback(void *data)
{
    if(!quiet)
        fprintf(stderr, "%s quitting because JACK is shutting down.\n", PROG_NAME);

    /* No need to free memory structures as we're exiting */

    exit(0);
}

/*
 * swap_bytes()
 * Swaps the least and most significant bytes in a contiguous block
 * of 16-bit data values.
 */
static void swap_bytes(char *buff, size_t nbytes)
{
   char tmp;
   size_t i;

   for(i = 0; i < nbytes; i += 2)
   {
      tmp = buff[i];
      buff[i] = buff[i + 1];
      buff[i + 1] = tmp;
   }

   return;
}
