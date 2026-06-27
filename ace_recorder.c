#include <stdint.h>
#define DR_WAV_IMPLEMENTATION
#define nchannels 64
#include "../dr_libs/dr_wav.h"
#include <linux/if_packet.h>
#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <netinet/ether.h>

struct sockaddr saddr;

struct audio_packet
{
	uint8_t source_mac[6];
  	uint8_t dest_mac[6];
  	uint8_t ether_type[2];
  	int8_t samples[65][3]; // 64 Channels+Sync
};
const uint8_t source[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t destination[] = {0x00, 0x04, 0xC4};

int kbhit()
{
	struct timeval tv;
	fd_set fds;
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds); // STDIN_FILENO is 0
	select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
	return FD_ISSET(STDIN_FILENO, &fds);
}

int main(int argc, char **argv)
{
	drwav_data_format format;
	drwav wav;
	int8_t tempFrames[4096][nchannels][3];
	if (argc < 2)
	{
		printf("No output file specified.\n");
		return -1;
	}

	format.container = drwav_container_riff;
	format.format = DR_WAVE_FORMAT_PCM;
	format.channels = nchannels;
	format.sampleRate = 48000;
	format.bitsPerSample = 24;
	if (!drwav_init_file_write(&wav, argv[1], &format, NULL))
	{
		printf("Failed to open file.\n");
		return -1;
	}
	int sock_r, saddr_len, buflen;

	unsigned char *buffer = (unsigned char *)malloc(65536);
	memset(buffer, 0, 65536);


	struct ifreq ifr;
	struct sockaddr_ll sll;
	const char *interface = "enx0050b6225caa";

	// 1. Raw Packet Socket erstellen
	sock_r = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (sock_r < 0)
	{
		perror("Raw-Socket-Erstellung fehlgeschlagen");
		return 1;
	}

	// 2. Interface-Index anhand des Namens (eth0) ermitteln
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, interface, IFNAMSIZ - 1);
	if (ioctl(sock_r, SIOCGIFINDEX, &ifr) < 0)
	{
		perror("Interface-Index konnte nicht ermittelt werden");
		close(sock_r);
		return 1;
	}

	// 3. sockaddr_ll Struktur vorbereiten
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_ifindex = ifr.ifr_ifindex; // Hier ist der Index von eth0
	sll.sll_protocol = htons(ETH_P_ALL);

	// 4. Socket an das Interface binden
	if (bind(sock_r, (struct sockaddr *)&sll, sizeof(sll)) < 0)
	{
		perror("Bind an AF_PACKET fehlgeschlagen");
		close(sock_r);
		return 1;
	}

	printf("Raw-Socket erfolgreich an %s (Index: %d) gebunden!\n", interface, ifr.ifr_ifindex);


	int i, wave_channel, ace_channel = 0;
	uint8_t lastSync;
	printf("Recording, press ENTER to stop .... \n");
	while (!kbhit())
	{
		// recvfrom reads one entire packet
		buflen = recvfrom(sock_r, buffer, 65536, 0, &saddr, (socklen_t *)&saddr_len);
		
		// the frame size of an ACE Packet is always 235 when not using VLAN
		if (buflen == 235)
		{
			struct audio_packet *audio = (struct audio_packet *)buffer;
			// memcmp returns 0 if equal
			// if (!memcmp(audio->source_mac, source, 6) && !memcmp(audio->dest_mac, destination, 3))
			// {
				uint8_t SyncByte=(audio->samples[0][2] & 0xf0) >> 4 | (audio->samples[0][2] & 0x0f) << 4;
				if (!(SyncByte == lastSync + 4))
				{
					printf("lost Packet detected\n");
				}
				lastSync = SyncByte== 0x7c ? 0x3c : SyncByte;
				for (int c = 0; c < nchannels; c++)
				{
					// on ACE the bits are weirdly shifted so we shift the back
					tempFrames[i][c][0] = (audio->samples[1 + c][0] & 0xf0) >> 4 |
											(audio->samples[1 + c][0] & 0x0f) << 4;
					tempFrames[i][c][1] = (audio->samples[1 + c][1] & 0xf0) >> 4 |
											(audio->samples[1 + c][1] & 0x0f) << 4;
					tempFrames[i][c][2] = (audio->samples[1 + c][2] & 0xf0) >> 4 |
											(audio->samples[1 + c][2] & 0x0f) << 4;
				}
				i++;
				if (i == 4096)
				{
					printf(".");
					drwav_write_pcm_frames_be(&wav, 4096, tempFrames);
					i = 0;
				}
			//}
		}
		else
		{
			printf("Received Packet of wrong length: %u\n", buflen);
		}
	}

	close(sock_r); // use signals to close socket
	drwav_uninit(&wav);

	return 0;
}
