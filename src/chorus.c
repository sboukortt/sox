/*
 * August 24, 1998
 * Copyright (C) 1998 Juergen Mueller And Sundry Contributors
 * This source code is freely redistributable and may be used for
 * any purpose.  This copyright notice must be maintained. 
 * Juergen Mueller And Sundry Contributors are not responsible for 
 * the consequences of using this software.
 */

/*
 * 	Chorus effect.
 * 
 * Flow diagram scheme for n delays ( 1 <= n <= MAX_CHORUS ):
 *
 *        * gain-in                                           ___
 * ibuff -----+--------------------------------------------->|   |
 *            |      _________                               |   |
 *            |     |         |                   * decay 1  |   |
 *            +---->| delay 1 |----------------------------->|   |
 *            |     |_________|                              |   |
 *            |        /|\                                   |   |
 *            :         |                                    |   |
 *            : +-----------------+   +--------------+       | + |
 *            : | Delay control 1 |<--| mod. speed 1 |       |   |
 *            : +-----------------+   +--------------+       |   |
 *            |      _________                               |   |
 *            |     |         |                   * decay n  |   |
 *            +---->| delay n |----------------------------->|   |
 *                  |_________|                              |   |
 *                     /|\                                   |___|
 *                      |                                      |  
 *              +-----------------+   +--------------+         | * gain-out
 *              | Delay control n |<--| mod. speed n |         |
 *              +-----------------+   +--------------+         +----->obuff
 *
 *
 * The delay i is controled by a sine or triangle modulation i ( 1 <= i <= n).
 *
 * Usage: 
 *   chorus gain-in gain-out delay-1 decay-1 speed-1 depth-1 -s1|t1 [
 *       delay-2 decay-2 speed-2 depth-2 -s2|-t2 ... ]
 *
 * Where:
 *   gain-in, decay-1 ... decay-n :  0.0 ... 1.0      volume
 *   gain-out :  0.0 ...      volume
 *   delay-1 ... delay-n :  20.0 ... 100.0 msec
 *   speed-1 ... speed-n :  0.1 ... 5.0 Hz       modulation 1 ... n
 *   depth-1 ... depth-n :  0.0 ... 10.0 msec    modulated delay 1 ... n
 *   -s1 ... -sn : modulation by sine 1 ... n
 *   -t1 ... -tn : modulation by triangle 1 ... n
 *
 * Note:
 *   when decay is close to 1.0, the samples can begin clipping and the output
 *   can saturate! 
 *
 * Hint:
 *   1 / out-gain < gain-in ( 1 + decay-1 + ... + decay-n )
 *
*/

/*
 * Sound Tools chorus effect file.
 */

#include <stdlib.h> /* Harmless, and prototypes atof() etc. --dgc */
#include <math.h>
#include <string.h>
#include "st.h"

#define MOD_SINE	0
#define MOD_TRIANGLE	1
#define MAX_CHORUS	7

/* Private data for SKEL file */
typedef struct chorusstuff {
	int	num_chorus;
	int	modulation[MAX_CHORUS];
	int	counter;			
	long	phase[MAX_CHORUS];
	float	*chorusbuf;
	float	in_gain, out_gain;
	float	delay[MAX_CHORUS], decay[MAX_CHORUS];
	float	speed[MAX_CHORUS], depth[MAX_CHORUS];
	long	length[MAX_CHORUS];
	int	*lookup_tab[MAX_CHORUS];
	int	depth_samples[MAX_CHORUS], samples[MAX_CHORUS];
	int	maxsamples, fade_out;
} *chorus_t;

/*
 * Process options
 */
void chorus_getopts(effp, n, argv) 
eff_t effp;
int n;
char **argv;
{
	chorus_t chorus = (chorus_t) effp->priv;
	int i;

	chorus->num_chorus = 0;
	i = 0;

	if ( ( n < 7 ) || (( n - 2 ) % 5 ) )
	    fail("Usage: chorus gain-in gain-out delay decay speed depth [ -s | -t ]");

	sscanf(argv[i++], "%f", &chorus->in_gain);
	sscanf(argv[i++], "%f", &chorus->out_gain);
	while ( i < n ) {
		if ( chorus->num_chorus > MAX_CHORUS )
			fail("chorus: to many delays, use less than %i delays", MAX_CHORUS);
		sscanf(argv[i++], "%f", &chorus->delay[chorus->num_chorus]);
		sscanf(argv[i++], "%f", &chorus->decay[chorus->num_chorus]);
		sscanf(argv[i++], "%f", &chorus->speed[chorus->num_chorus]);
		sscanf(argv[i++], "%f", &chorus->depth[chorus->num_chorus]);
		if ( !strcmp(argv[i], "-s"))
			chorus->modulation[chorus->num_chorus] = MOD_SINE;
		else if ( ! strcmp(argv[i], "-t"))
			chorus->modulation[chorus->num_chorus] = MOD_TRIANGLE;
		else
    			fail("Usage: chorus gain-in gain-out delay decay speed [ -s | -t ]");
		i++;
		chorus->num_chorus++;
	}
}

/*
 * Prepare for processing.
 */
void chorus_start(effp)
eff_t effp;
{
	chorus_t chorus = (chorus_t) effp->priv;
	int i;
	float sum_in_volume;

	chorus->maxsamples = 0;

	if ( chorus->in_gain < 0.0 )
		fail("chorus: gain-in must be positive!\n");
	if ( chorus->in_gain > 1.0 )
		fail("chorus: gain-in must be less than 1.0!\n");
	if ( chorus->out_gain < 0.0 )
		fail("chorus: gain-out must be positive!\n");
	for ( i = 0; i < chorus->num_chorus; i++ ) {
		chorus->samples[i] = (int) ( ( chorus->delay[i] + 
			chorus->depth[i] ) * effp->ininfo.rate / 1000.0);
		chorus->depth_samples[i] = (int) (chorus->depth[i] * 
			effp->ininfo.rate / 1000.0);

		if ( chorus->delay[i] < 20.0 )
	    		fail("chorus: delay must be more than 20.0 msec!\n");
		if ( chorus->delay[i] > 100.0 )
	    		fail("chorus: delay must be less than 100.0 msec!\n");
		if ( chorus->speed[i] < 0.1 )
	    		fail("chorus: speed must be more than 0.1 Hz!\n");
		if ( chorus->speed[i] > 5.0 )
	    		fail("chorus: speed must be less than 5.0 Hz!\n");
		if ( chorus->depth[i] < 0.0 )
	    		fail("chorus: delay must be more positive!\n");
		if ( chorus->depth[i] > 10.0 )
	    		fail("chorus: delay must be less than 10.0 msec!\n");
		if ( chorus->decay[i] < 0.0 )
	    		fail("chorus: decay must be positive!\n" );
		if ( chorus->decay[i] > 1.0 )
	    		fail("chorus: decay must be less that 1.0!\n" );
		chorus->length[i] = effp->ininfo.rate / chorus->speed[i];
		if (! (chorus->lookup_tab[i] = 
			(int *) malloc(sizeof (int) * chorus->length[i])))
			fail("chorus: Cannot malloc %d bytes!\n", 
				sizeof(int) * chorus->length[i]);
		if ( chorus->modulation[i] == MOD_SINE )
			st_sine(chorus->lookup_tab[i], chorus->length[i], 
				chorus->depth_samples[i] - 1,
				chorus->depth_samples[i]);
		else
			st_triangle(chorus->lookup_tab[i], chorus->length[i], 
				chorus->samples[i] - 1,
				chorus->depth_samples[i]);
		chorus->phase[i] = 0;

		if ( chorus->samples[i] > chorus->maxsamples )
			chorus->maxsamples = chorus->samples[i];
	}

	/* Be nice and check the hint with warning, if... */
	sum_in_volume = 1.0;
	for ( i = 0; i < chorus->num_chorus; i++ )
		sum_in_volume += chorus->decay[i];
	if ( chorus->in_gain * ( sum_in_volume ) > 1.0 / chorus->out_gain )
	warn("chorus: warning >>> gain-out can cause saturation or clipping of output <<<");


	if (! (chorus->chorusbuf = 
		(float *) malloc(sizeof (float) * chorus->maxsamples)))
		fail("chorus: Cannot malloc %d bytes!\n", 
			sizeof(float) * chorus->maxsamples);
	for ( i = 0; i < chorus->maxsamples; i++ )
		chorus->chorusbuf[i] = 0.0;

	chorus->counter = 0;
	chorus->fade_out = chorus->maxsamples;
}

/*
 * Processed signed long samples from ibuf to obuf.
 * Return number of samples processed.
 */

void chorus_flow(effp, ibuf, obuf, isamp, osamp)
eff_t effp;
LONG *ibuf, *obuf;
LONG *isamp, *osamp;
{
	chorus_t chorus = (chorus_t) effp->priv;
	int len, done;
	int i;
	
	float d_in, d_out;
	LONG out;

	len = ((*isamp > *osamp) ? *osamp : *isamp);
	for(done = 0; done < len; done++) {
		/* Store delays as 24-bit signed longs */
		d_in = (float) *ibuf++ / 256;
		/* Compute output first */
		d_out = d_in * chorus->in_gain;
		for ( i = 0; i < chorus->num_chorus; i++ )
			d_out += chorus->chorusbuf[(chorus->maxsamples + 
		        chorus->counter - chorus->lookup_tab[i][chorus->phase[i]]) % 
			chorus->maxsamples] * chorus->decay[i];
		/* Adjust the output volume and size to 24 bit */
		d_out = d_out * chorus->out_gain;
		out = st_clip24((LONG) d_out);
		*obuf++ = out * 256;
		/* Mix decay of delay and input */
		chorus->chorusbuf[chorus->counter] = d_in;
		chorus->counter = 
			( chorus->counter + 1 ) % chorus->maxsamples;
		for ( i = 0; i < chorus->num_chorus; i++ )
			chorus->phase[i]  = 
				( chorus->phase[i] + 1 ) % chorus->length[i];
	}
	/* processed all samples */
}

/*
 * Drain out reverb lines. 
 */
void chorus_drain(effp, obuf, osamp)
eff_t effp;
LONG *obuf;
LONG *osamp;
{
	chorus_t chorus = (chorus_t) effp->priv;
	int done;
	int i;
	
	float d_in, d_out;
	LONG out;

	done = 0;
	while ( ( done < *osamp ) && ( done < chorus->fade_out ) ) {
		d_in = 0;
		d_out = 0;
		/* Compute output first */
		for ( i = 0; i < chorus->num_chorus; i++ )
			d_out += chorus->chorusbuf[(chorus->maxsamples + 
		chorus->counter - chorus->lookup_tab[i][chorus->phase[i]]) % 
		chorus->maxsamples] * chorus->decay[i];
		/* Adjust the output volume and size to 24 bit */
		d_out = d_out * chorus->out_gain;
		out = st_clip24((LONG) d_out);
		*obuf++ = out * 256;
		/* Mix decay of delay and input */
		chorus->chorusbuf[chorus->counter] = d_in;
		chorus->counter = 
			( chorus->counter + 1 ) % chorus->maxsamples;
		for ( i = 0; i < chorus->num_chorus; i++ )
			chorus->phase[i]  = 
				( chorus->phase[i] + 1 ) % chorus->length[i];
		done++;
		chorus->fade_out--;
	}
	/* samples played, it remains */
	*osamp = done;
}

/*
 * Clean up chorus effect.
 */
void chorus_stop(effp)
eff_t effp;
{
	chorus_t chorus = (chorus_t) effp->priv;
	int i;

	free((char *) chorus->chorusbuf);
	chorus->chorusbuf = (float *) -1;   /* guaranteed core dump */
	for ( i = 0; i < chorus->num_chorus; i++ ) {
		free((char *) chorus->lookup_tab[i]);
		chorus->lookup_tab[i] = (int *) -1;   /* guaranteed core dump */
	}
}

