#include <stdint.h>
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_SUPPORT_WRITE
#define DR_WAV_SUPPORT_READ
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
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/ether.h>

struct sockaddr saddr;

struct audio_packet {
  uint8_t dest_mac[6];     // Ziel-MAC (Mischpult)
  uint8_t source_mac[6];   // Quell-MAC (PC)
  uint8_t ether_type[2];   // Protokoll-Typ
  int8_t samples[65][3];   // 1x Sync-Slot + 64 Kanäle (jeweils 3 Bytes/24-Bit)
} __attribute__((packed)); // Wichtig, damit der Compiler kein Padding einfügt

// Standard-ACE-Adressen (ggf. anpassen, falls dein Pult andere MACs nutzt)
const uint8_t ace_source[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t ace_dest[]   = {0x00, 0x04, 0xC4}; 

int kbhit() {
  struct timeval tv;
  fd_set fds;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
  return FD_ISSET(STDIN_FILENO, &fds);
}

// Hilfsfunktion für den ACE-spezifischen Bit-Dreher (Nibble-Swap)
static inline int8_t swap_nibbles(int8_t val) {
    return (val & 0xf0) >> 4 | (val & 0x0f) << 4;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    printf("Verwendung:\n");
    printf("  Aufnahme:  %s -r <ausgabe_datei.wav>\n", argv[0]);
    printf("  Wiedergabe: %s -p <eingabe_datei.wav>\n", argv[0]);
    return -1;
  }

  char *mode = argv[1];
  char *filename = argv[2];
  bool record_mode = (strcmp(mode, "-r") == 0);

  drwav_data_format format;
  drwav wav;

  // 4096 Frames Puffer für Datei-I/O
  int8_t tempFrames[4096][nchannels][3];

  if (record_mode) {
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = nchannels;
    format.sampleRate = 48000;
    format.bitsPerSample = 24;
    if (!drwav_init_file_write(&wav, filename, &format, NULL)) {
      printf("Fehler beim Öffnen der WAV-Datei zum Schreiben.\n");
      return -1;
    }
    printf("[REC] Aufnahme in Datei: %s\n", filename);
  } else {
    if (!drwav_init_file(&wav, filename, NULL)) {
      printf("Fehler beim Öffnen der WAV-Datei zum Lesen.\n");
      return -1;
    }
    if (wav.channels != nchannels || wav.sampleRate != 48000 || wav.bitsPerSample != 24) {
      printf("[WARNUNG] WAV-Format passt nicht exakt (Erwartet: 64 Kanäle, 48kHz, 24-Bit).\n");
    }
    printf("[PLAY] Wiedergabe aus Datei: %s\n", filename);
  }

  int sock_r, saddr_len;
  unsigned char *buffer = (unsigned char *)malloc(65536);
  memset(buffer, 0, 65536);

  struct ifreq ifr;
  struct sockaddr_ll sll;
  const char *interface = "eth0"; // Netzwerkkarte anpassen!

  sock_r = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (sock_r < 0) {
      perror("Raw-Socket Fehler");
      free(buffer);
      return 1;
  }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, interface, IFNAMSIZ - 1);
  if (ioctl(sock_r, SIOCGIFINDEX, &ifr) < 0) {
      perror("Interface-Index Fehler");
      close(sock_r);
      free(buffer);
      return 1;
  }

  memset(&sll, 0, sizeof(sll));
  sll.sll_family = AF_PACKET;
  sll.sll_ifindex = ifr.ifr_ifindex;
  sll.sll_protocol = htons(ETH_P_ALL);

  if (bind(sock_r, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
      perror("Bind Fehler");
      close(sock_r);
      free(buffer);
      return 1;
  }

  printf("Raw-Socket an %s gebunden!\n", interface);

  int i = 0; // i initialisiert!
  uint8_t lastSync = 0;
  uint8_t playSyncCounter = 0x3c; // Startwert für ACE-Sync-Wiedergabe
  
  // Paket-Struktur für die Wiedergabe vorbereiten
  struct audio_packet play_packet;
  memset(&play_packet, 0, sizeof(play_packet));
  // MAC-Adressen spiegeln: Ziel ist das Pult, Quelle ist der PC (bzw. Broadcast)
  memcpy(play_packet.dest_mac, ace_dest, 3); // Nutzt die ersten 3 Bytes zur Erkennung
  memcpy(play_packet.source_mac, ace_source, 6);
  play_packet.ether_type[0] = 0x00; // Beispielhafter Ethertype aus deinem Code extrahiert
  play_packet.ether_type[1] = 0x00;

  // Ersten Block für Wiedergabe vorladen, falls im Play-Modus
  if (!record_mode) {
    drwav_read_pcm_frames(&wav, 4096, tempFrames);
  }

  printf("Drücke ENTER zum Stoppen...\n");

  while (!kbhit()) {
    saddr_len = sizeof(saddr);
    int buflen = recvfrom(sock_r, buffer, 65536, 0, &saddr, (socklen_t *)&saddr_len);
    
    if (buflen == 235) {
      struct audio_packet *audio = (struct audio_packet *)buffer;
      
      // Prüfen, ob das Paket vom iLive-Pult stammt
      if (!memcmp(audio->source_mac, ace_source, 6) && !memcmp(audio->dest_mac, ace_dest, 3)) {
        
        // --- MODUS 1: AUFNAHME ---
        if (record_mode) {
          uint8_t SyncByte = swap_nibbles(audio->samples[0][2]);  
          if (i > 0 && !(SyncByte == (uint8_t)(lastSync + 4))) {
            printf("Paketverlust erkannt!\n");
          }
          lastSync = (SyncByte == 0x7c) ? 0x3c : SyncByte;

          for (int c = 0; c < nchannels; c++) {
            tempFrames[i][c][0] = swap_nibbles(audio->samples[1 + c][0]);
            tempFrames[i][c][1] = swap_nibbles(audio->samples[1 + c][1]);
            tempFrames[i][c][2] = swap_nibbles(audio->samples[1 + c][2]);
          }
          i++;
          if (i == 4096) {
            drwav_write_pcm_frames_be(&wav, 4096, tempFrames);
            i = 0;
          }
        }
        
        // --- MODUS 2: WIEDERGABE (Playback) ---
        else {
          // 1. Sync-Byte für ACE berechnen und "drehen"
          play_packet.samples[0][0] = 0;
          play_packet.samples[0][1] = 0;
          play_packet.samples[0][2] = swap_nibbles(playSyncCounter);
          
          // ACE-Sync zählt üblicherweise in 4er-Schritten hoch und springt bei 0x7c zurück auf 0x3c
          playSyncCounter += 4;
          if (playSyncCounter > 0x7c) {
              playSyncCounter = 0x3c;
          }

          // 2. Audiodaten aus dem Puffer in das Netzwerkpaket laden (mit Nibble-Swap)
          for (int c = 0; c < nchannels; c++) {
            play_packet.samples[1 + c][0] = swap_nibbles(tempFrames[i][c][0]);
            play_packet.samples[1 + c][1] = swap_nibbles(tempFrames[i][c][1]);
            play_packet.samples[1 + c][2] = swap_nibbles(tempFrames[i][c][2]);
          }

          // 3. Paket direkt als Antwort abschicken (Nutzt das iLive-Paket als Clock!)
          if (sendto(sock_r, &play_packet, 235, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
              perror("Senden fehlgeschlagen");
          }

          i++;
          if (i == 4096) {
            // Nächsten Block aus der WAV-Datei lesen
            size_t read = drwav_read_pcm_frames(&wav, 4096, tempFrames);
            if (read < 4096) {
              printf("Ende der WAV-Datei erreicht.\n");
              break; // Loop beenden
            }
            i = 0;
          }
        }

      }
    }
  }

  // Aufräumen
  close(sock_r);
  drwav_uninit(&wav);
  free(buffer);
  printf("Beendet.\n");
  return 0;
}