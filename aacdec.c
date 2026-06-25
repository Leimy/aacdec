/*
 * AAC (ADTS) decoder.
 *
 * Reads AAC audio from stdin, decodes it using FAAD2 (libfaad),
 * and pipes s16 interleaved PCM to pcmconv on stdout.
 *
 * Handles raw ADTS streams as used by internet radio
 * (Shoutcast, Icecast, etc).
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#define _PLAN9_SOURCE
#include <utf.h>
#include <lib9.h>
#include <neaacdec.h>

enum {
	Bufsz = 32*1024,
	Maxfill = 3*Bufsz,
};

static int ifd = -1;
static int debug;

static void
flushout(void)
{
	int status;

	if(ifd >= 0){
		close(ifd);
		wait(&status);
		ifd = -1;
	}
}

/*
 * Start (or restart) pcmconv as a child process.
 * Feeds it data in the format specified by rate and channels.
 * FAAD2 outputs 16-bit signed samples by default.
 */
static void
startconv(int rate, int chans)
{
	int pfd[2];
	int pid;
	char fmt[32];

	flushout();
	sprintf(fmt, "s16r%dc%d", rate, chans);
	if(debug)
		fprintf(stderr, "aacdec: format %s\n", fmt);
	if(pipe(pfd) < 0){
		fprintf(stderr, "aacdec: pipe: %s\n", strerror(errno));
		exit(1);
	}
	pid = fork();
	if(pid < 0){
		fprintf(stderr, "aacdec: fork: %s\n", strerror(errno));
		exit(1);
	}
	if(pid == 0){
		dup2(pfd[1], 0);
		close(pfd[1]);
		close(pfd[0]);
		execl("/bin/audio/pcmconv", "pcmconv", "-i", fmt, NULL);
		fprintf(stderr, "aacdec: exec pcmconv: %s\n", strerror(errno));
		exit(1);
	}
	close(pfd[1]);
	ifd = pfd[0];
}

/*
 * Search for an ADTS sync word (0xFFF) in the buffer.
 * Returns the offset to the sync, or -1 if not found.
 *
 * ADTS header (7 or 9 bytes):
 *   12 bits  sync word (0xFFF)
 *    1 bit   MPEG version (0 = MPEG-4, 1 = MPEG-2)
 *    2 bits  layer (always 0)
 *    1 bit   protection absent
 *    ... etc
 */
static int
adtssync(unsigned char *buf, int len)
{
	int i;

	for(i = 0; i < len - 1; i++){
		if(buf[i] == 0xff && (buf[i+1] & 0xf0) == 0xf0)
			return i;
	}
	return -1;
}

/*
 * Parse an ADTS header to get the frame length.
 * Returns the frame length, or 0 if the header looks invalid.
 * Needs at least 7 bytes.
 */
static int
adtsframelen(unsigned char *buf)
{
	int len;

	/* frame length is 13 bits starting at bit 30 */
	len = ((int)(buf[3] & 0x03) << 11)
	    | ((int)buf[4] << 3)
	    | ((int)(buf[5] >> 5));
	if(len < 7)
		return 0;
	return len;
}

static void
usage(void)
{
	fprintf(stderr, "usage: aacdec [-d]\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	NeAACDecHandle dec;
	NeAACDecConfigurationPtr conf;
	NeAACDecFrameInfo info;
	unsigned char *inbuf;
	void *pcm;
	unsigned long samplerate;
	unsigned char channels;
	int rate, chans;
	long bread, fill, consumed, off;
	int init;

	ARGBEGIN{
	case 'd':
		debug++;
		break;
	default:
		usage();
	}ARGEND

	inbuf = malloc(Maxfill);
	if(inbuf == NULL){
		fprintf(stderr, "aacdec: malloc: %s\n", strerror(errno));
		return 1;
	}

	dec = NeAACDecOpen();
	if(dec == NULL){
		fprintf(stderr, "aacdec: NeAACDecOpen failed\n");
		return 1;
	}

	/* configure: request 16-bit output */
	conf = NeAACDecGetCurrentConfiguration(dec);
	conf->outputFormat = FAAD_FMT_16BIT;
	conf->downMatrix = 1;		/* downmix 5.1 to stereo */
	NeAACDecSetConfiguration(dec, conf);

	rate = 0;
	chans = 0;
	fill = 0;
	init = 0;

	/* main decode loop */
	for(;;){
		/* fill input buffer */
		if(fill < Bufsz){
			bread = fread(inbuf + fill, 1, Bufsz - fill, stdin);
			if(bread <= 0){
				if(feof(stdin))
					break;
				fprintf(stderr, "aacdec: read error\n");
				break;
			}
			fill += bread;
		}

		/* find ADTS sync */
		off = adtssync(inbuf, fill);
		if(off < 0){
			/* no sync found, discard most of buffer keeping last byte */
			inbuf[0] = inbuf[fill - 1];
			fill = 1;
			continue;
		}
		if(off > 0){
			/* skip garbage before sync */
			if(debug)
				fprintf(stderr, "aacdec: skipped %ld bytes to sync\n", off);
			memmove(inbuf, inbuf + off, fill - off);
			fill -= off;
			continue;
		}

		/* initialize decoder on first valid ADTS frame */
		if(!init){
			consumed = NeAACDecInit(dec, inbuf, fill,
				&samplerate, &channels);
			if(consumed < 0){
				fprintf(stderr, "aacdec: NeAACDecInit failed\n");
				/* try to find next sync */
				memmove(inbuf, inbuf + 1, fill - 1);
				fill--;
				continue;
			}
			if(consumed > 0){
				memmove(inbuf, inbuf + consumed, fill - consumed);
				fill -= consumed;
			}
			if(debug)
				fprintf(stderr, "aacdec: init: %luHz %uch, consumed %ld\n",
					samplerate, channels, consumed);
			init = 1;
			continue;
		}

		/* need at least an ADTS header to check frame length */
		if(fill < 7){
			bread = fread(inbuf + fill, 1, Bufsz, stdin);
			if(bread <= 0)
				break;
			fill += bread;
			continue;
		}

		/* verify we have a complete frame before decoding */
		consumed = adtsframelen(inbuf);
		if(consumed == 0){
			/* bad header, skip this sync and look for next */
			memmove(inbuf, inbuf + 1, fill - 1);
			fill--;
			continue;
		}
		if(consumed > fill){
			/* need more data for complete frame */
			if(fill >= Maxfill){
				/* frame claims to be huge, probably corrupt */
				if(debug)
					fprintf(stderr, "aacdec: frame too large (%ld), resync\n",
						consumed);
				memmove(inbuf, inbuf + 1, fill - 1);
				fill--;
				continue;
			}
			bread = fread(inbuf + fill, 1, consumed - fill, stdin);
			if(bread <= 0)
				break;
			fill += bread;
			if(fill < consumed)
				continue;
		}

		/* decode one frame */
		pcm = NeAACDecDecode(dec, &info, inbuf, fill);

		if(info.error > 0){
			if(debug)
				fprintf(stderr, "aacdec: %s\n",
					NeAACDecGetErrorMessage(info.error));
			/*
			 * on error, skip past this frame or at least
			 * past the sync word to find the next frame
			 */
			if(info.bytesconsumed > 0){
				memmove(inbuf, inbuf + info.bytesconsumed,
					fill - info.bytesconsumed);
				fill -= info.bytesconsumed;
			} else {
				memmove(inbuf, inbuf + 1, fill - 1);
				fill--;
			}
			continue;
		}

		/* advance past consumed bytes */
		if(info.bytesconsumed > 0){
			memmove(inbuf, inbuf + info.bytesconsumed,
				fill - info.bytesconsumed);
			fill -= info.bytesconsumed;
		}

		/* output decoded samples */
		if(pcm != NULL && info.samples > 0){
			/* restart pcmconv if format changed */
			if((int)info.samplerate != rate || (int)info.channels != chans){
				rate = info.samplerate;
				chans = info.channels;
				startconv(rate, chans);
			}
			/* info.samples is total samples across all channels */
			if(write(ifd, pcm, info.samples * 2) != info.samples * 2){
				if(debug)
					fprintf(stderr, "aacdec: write error\n");
				break;
			}
		}
	}

	NeAACDecClose(dec);
	flushout();
	free(inbuf);

	return 0;
}
