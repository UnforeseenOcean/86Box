/* Copyright holders: Sarah Walker, Tenshi, SA1988
   see COPYING for more details
*/
//#define RPCEMU_IDE

#define CDROM_ISO 200
#define IDE_TIME (5 * 100 * (1 << TIMER_SHIFT))

#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <sys/types.h>

#ifdef RPCEMU_IDE
        #include "rpcemu.h"
        #include "iomd.h"
        #include "arm.h"
#else
		#include "86box.h"
		#include "ibm.h"
        #include "io.h"
        #include "pic.h"
        #include "timer.h"
#endif
#include "cdrom.h"
#include "scsi.h"
#include "ide.h"

/* Bits of 'atastat' */
#define ERR_STAT		0x01
#define DRQ_STAT		0x08 /* Data request */
#define DSC_STAT                0x10
#define SERVICE_STAT            0x10
#define READY_STAT		0x40
#define BUSY_STAT		0x80

/* Bits of 'error' */
#define ABRT_ERR		0x04 /* Command aborted */
#define MCR_ERR			0x08 /* Media change request */

/* ATA Commands */
#define WIN_NOP				0x00
#define WIN_SRST			0x08 /* ATAPI Device Reset */
#define WIN_RECAL			0x10
#define WIN_RESTORE			WIN_RECAL
#define WIN_READ			0x20 /* 28-Bit Read */
#define WIN_READ_NORETRY                0x21 /* 28-Bit Read - no retry*/
#define WIN_WRITE			0x30 /* 28-Bit Write */
#define WIN_WRITE_NORETRY		0x31 /* 28-Bit Write */
#define WIN_VERIFY			0x40 /* 28-Bit Verify */
#define WIN_VERIFY_ONCE		0x41 /* Added by OBattler - deprected older ATA command, according to the specification I found, it is identical to 0x40 */
#define WIN_FORMAT			0x50
#define WIN_SEEK			0x70
#define WIN_DRIVE_DIAGNOSTICS           0x90 /* Execute Drive Diagnostics */
#define WIN_SPECIFY			0x91 /* Initialize Drive Parameters */
#define WIN_PACKETCMD			0xA0 /* Send a packet command. */
#define WIN_PIDENTIFY			0xA1 /* Identify ATAPI device */
#define WIN_READ_MULTIPLE               0xC4
#define WIN_WRITE_MULTIPLE              0xC5
#define WIN_SET_MULTIPLE_MODE           0xC6
#define WIN_READ_DMA                    0xC8
#define WIN_WRITE_DMA                   0xCA
#define WIN_STANDBYNOW1			0xE0
#define WIN_SETIDLE1			0xE3
#define WIN_CHECKPOWERMODE1		0xE5
#define WIN_IDENTIFY			0xEC /* Ask drive to identify itself */
#define WIN_SET_FEATURES		0xEF
#define WIN_READ_NATIVE_MAX		0xF8

/** Evaluate to non-zero if the currently selected drive is an ATAPI device */
#define IDE_DRIVE_IS_CDROM(ide)  (ide->type == IDE_CDROM)

#define ATAPI_STATUS_IDLE            0
#define ATAPI_STATUS_COMMAND         1
#define ATAPI_STATUS_COMPLETE        2
#define ATAPI_STATUS_DATA            3
#define ATAPI_STATUS_PACKET_REQ      4
#define ATAPI_STATUS_PACKET_RECEIVED 5
#define ATAPI_STATUS_READCD          6
#define ATAPI_STATUS_REQ_SENSE       7
#define ATAPI_STATUS_ERROR           0x80
#define ATAPI_STATUS_ERROR_2         0x81

enum
{
        IDE_NONE = 0,
        IDE_HDD,
        IDE_CDROM
};

uint64_t hdt[128][3] = {	{  306,  4, 17 }, {  615,  2, 17 }, {  306,  4, 26 }, { 1024,  2, 17 }, {  697,  3, 17 }, {  306,  8, 17 }, {  614,  4, 17 }, {  615,  4, 17 },		/* 000-007 */
			{  670,  4, 17 }, {  697,  4, 17 }, {  987,  3, 17 }, {  820,  4, 17 }, {  670,  5, 17 }, {  697,  5, 17 }, {  733,  5, 17 }, {  615,  6, 17 },		/* 008-015 */
			{  462,  8, 17 }, {  306,  8, 26 }, {  615,  4, 26 }, { 1024,  4, 17 }, {  855,  5, 17 }, {  925,  5, 17 }, {  932,  5, 17 }, { 1024,  2, 40 },		/* 016-023 */
			{  809,  6, 17 }, {  976,  5, 17 }, {  977,  5, 17 }, {  698,  7, 17 }, {  699,  7, 17 }, {  981,  5, 17 }, {  615,  8, 17 }, {  989,  5, 17 },		/* 024-031 */
			{  820,  4, 26 }, { 1024,  5, 17 }, {  733,  7, 17 }, {  754,  7, 17 }, {  733,  5, 26 }, {  940,  6, 17 }, {  615,  6, 26 }, {  462,  8, 26 },		/* 032-039 */
			{  830,  7, 17 }, {  855,  7, 17 }, {  751,  8, 17 }, { 1024,  4, 26 }, {  918,  7, 17 }, {  925,  7, 17 }, {  855,  5, 26 }, {  977,  7, 17 },		/* 040-047 */
			{  987,  7, 17 }, { 1024,  7, 17 }, {  823,  4, 38 }, {  925,  8, 17 }, {  809,  6, 26 }, {  976,  5, 26 }, {  977,  5, 26 }, {  698,  7, 26 },		/* 048-055 */
			{  699,  7, 26 }, {  940,  8, 17 }, {  615,  8, 26 }, { 1024,  5, 26 }, {  733,  7, 26 }, { 1024,  8, 17 }, {  823, 10, 17 }, {  754, 11, 17 },		/* 056-063 */
			{  830, 10, 17 }, {  925,  9, 17 }, { 1224,  7, 17 }, {  940,  6, 26 }, {  855,  7, 26 }, {  751,  8, 26 }, { 1024,  9, 17 }, {  965, 10, 17 },		/* 064-071 */
			{  969,  5, 34 }, {  980, 10, 17 }, {  960,  5, 35 }, {  918, 11, 17 }, { 1024, 10, 17 }, {  977,  7, 26 }, { 1024,  7, 26 }, { 1024, 11, 17 },		/* 072-079 */
			{  940,  8, 26 }, {  776,  8, 33 }, {  755, 16, 17 }, { 1024, 12, 17 }, { 1024,  8, 26 }, {  823, 10, 26 }, {  830, 10, 26 }, {  925,  9, 26 },		/* 080-087 */
			{  960,  9, 26 }, { 1024, 13, 17 }, { 1224, 11, 17 }, {  900, 15, 17 }, {  969,  7, 34 }, {  917, 15, 17 }, {  918, 15, 17 }, { 1524,  4, 39 },		/* 088-095 */
			{ 1024,  9, 26 }, { 1024, 14, 17 }, {  965, 10, 26 }, {  980, 10, 26 }, { 1020, 15, 17 }, { 1023, 15, 17 }, { 1024, 15, 17 }, { 1024, 16, 17 },		/* 096-103 */
			{ 1224, 15, 17 }, {  755, 16, 26 }, {  903,  8, 46 }, {  984, 10, 34 }, {  900, 15, 26 }, {  917, 15, 26 }, { 1023, 15, 26 }, {  684, 16, 38 },		/* 104-111 */
			{ 1930,  4, 62 }, {  967, 16, 31 }, { 1013, 10, 63 }, { 1218, 15, 36 }, {  654, 16, 63 }, {  659, 16, 63 }, {  702, 16, 63 }, { 1002, 13, 63 },		/* 112-119 */
			{  854, 16, 63 }, {  987, 16, 63 }, {  995, 16, 63 }, { 1024, 16, 63 }, { 1036, 16, 63 }, { 1120, 16, 59 }, { 1054, 16, 63 }, {    0,  0,  0 }	};	/* 119-127 */
			
typedef struct IDE
{
        int type;
        int board;
        uint8_t atastat;
        uint8_t error;
        int secount,sector,cylinder,head,drive,cylprecomp;
        uint8_t command;
        uint8_t fdisk;
        int pos;
        int packlen;
        int spt,hpc;
        int tracks;
        int packetstatus;
        int cdpos,cdlen;
        uint8_t asc;
        int reset;
        FILE *hdfile;
        uint16_t buffer[65536];
        int irqstat;
        int service;
        int lba;
        uint32_t lba_addr;
        int skip512;
        int blocksize, blockcount;
		uint16_t dma_identify_data[3];
		int hdi,base;
		int hdc_num;
} IDE;

IDE ide_drives[6];

IDE *ext_ide;

char ide_fn[4][512];

int (*ide_bus_master_read_sector)(int channel, uint8_t *data);
int (*ide_bus_master_write_sector)(int channel, uint8_t *data);
void (*ide_bus_master_set_irq)(int channel);

static void callnonreadcd(IDE *ide);
static void callreadcd(IDE *ide);
static void atapicommand(int ide_board);

int idecallback[3] = {0, 0, 0};

int cur_ide[3];

int atapi_command = 0;

int atapi_cdrom_channel = 2;

int ide_ter_enabled = 0;

uint8_t getstat(IDE *ide) { return ide->atastat; }

#define MSFtoLBA(m,s,f)  ((((m*60)+s)*75)+f)

typedef struct __attribute__((packed))
{
	uint8_t user_data[2048];
	uint8_t ecc[288];
} m1_data_t;

typedef struct __attribute__((packed))
{
	uint8_t sub_header[8];
	uint8_t user_data[2328];
} m2_data_t;

typedef union __attribute__((packed))
{
	m1_data_t m1_data;
	m2_data_t m2_data;
	uint8_t raw_data[2352];
} sector_data_t;

typedef struct __attribute__((packed))
{
	uint8_t sync[12];
	uint8_t header[4];
	sector_data_t data;
	uint8_t c2[296];
	uint8_t subchannel_raw[96];
	uint8_t subchannel_q[16];
	uint8_t subchannel_rw[96];
} cdrom_sector_t;

typedef union __attribute__((packed))
{
	cdrom_sector_t cdrom_sector;
	uint8_t buffer[2856];
} sector_buffer_t;

sector_buffer_t cdrom_sector_buffer;

int cdrom_sector_type, cdrom_sector_flags;
int cdrom_sector_size, cdrom_sector_ismsf;

int image_is_hdi(const char *s)
{
	int i, len;
	char ext[5] = { 0, 0, 0, 0, 0 };
	len = strlen(s);
	if ((len < 4) || (s[0] == '.'))
	{
		return 0;
	}
	memcpy(ext, s + len - 4, 4);
	for (i = 0; i < 4; i++)
	{
		ext[i] = toupper(ext[i]);
	}
	if (strcmp(ext, ".HDI") == 0)
	{
		return 1;
	}
	else
	{
		return 0;
	}
}

static inline void ide_irq_raise(IDE *ide)
{
//        pclog("IDE_IRQ_RAISE\n");
	if (!(ide->fdisk&2)) {
#ifdef RPCEMU_IDE
		iomd.irqb.status |= IOMD_IRQB_IDE;
		updateirqs();
#else
//                if (ide->board && !ide->irqstat) pclog("IDE_IRQ_RAISE\n");
#ifdef MAINLINE
                picint((ide->board)?(1<<15):(1<<14));
#else
				switch(ide->board)
				{
					case 0:
						picint(1 << 14);
						break;
					case 1:
						picint(1 << 15);
						break;
					case 2:
						picint(1 << 10);
						break;
				}
#endif
				if (ide->board < 2)
				{
					if (ide_bus_master_set_irq)
							ide_bus_master_set_irq(ide->board);
				}
#endif
	}
	ide->irqstat=1;
        ide->service=1;
	// pclog("raising interrupt %i\n", 14 + ide->board);
}

static inline void ide_irq_lower(IDE *ide)
{
//        pclog("IDE_IRQ_LOWER\n");
//	if (ide.board == 0) {
#ifdef RPCEMU_IDE
		iomd.irqb.status &= ~IOMD_IRQB_IDE;
		updateirqs();
#else
#ifdef MAINLINE
                picintc((ide->board)?(1<<15):(1<<14));
#else
				switch(ide->board)
				{
					case 0:
						picintc(1 << 14);
						break;
					case 1:
						picintc(1 << 15);
						break;
					case 2:
						picintc(1 << 10);
						break;
				}
#endif
#endif
//	}
	ide->irqstat=0;
}

int get_irq(uint8_t board)
{
	if (board == 0)  return 1 << 14;
	else if (board == 1)  return 1 << 15;
	else if (board == 2)  return 1 << 10;
}

void ide_irq_update(IDE *ide)
{
#ifdef RPCEMU_IDE
	if (ide->irqstat && !(iomd.irqb.status & IOMD_IRQB_IDE) && !(ide->fdisk & 2)) {
		iomd.irqb.status |= IOMD_IRQB_IDE;
		updateirqs();
        }
        else if (iomd.irqb.status & IOMD_IRQB_IDE)
        {
		iomd.irqb.status &= ~IOMD_IRQB_IDE;
		updateirqs();
        }
#else
#ifdef MAINLINE
	if (ide->irqstat && !((pic2.pend|pic2.ins)&0x40) && !(ide->fdisk & 2))
            picint((ide->board)?(1<<15):(1<<14));
        else if ((pic2.pend|pic2.ins)&0x40)
            picintc((ide->board)?(1<<15):(1<<14));
#else
	if (ide->irqstat && !((pic2.pend|pic2.ins)&0x40) && !(ide->fdisk & 2))
            picint(get_irq(ide->board));
        else if ((pic2.pend|pic2.ins)&0x40)
            picintc(get_irq(ide->board));
#endif
#endif
}
/**
 * Copy a string into a buffer, padding with spaces, and placing characters as
 * if they were packed into 16-bit values, stored little-endian.
 *
 * @param str Destination buffer
 * @param src Source string
 * @param len Length of destination buffer to fill in. Strings shorter than
 *            this length will be padded with spaces.
 */
static void
ide_padstr(char *str, const char *src, int len)
{
	int i, v;

	for (i = 0; i < len; i++) {
		if (*src != '\0') {
			v = *src++;
		} else {
			v = ' ';
		}
		str[i ^ 1] = v;
	}
}

/**
 * Copy a string into a buffer, padding with spaces. Does not add string
 * terminator.
 *
 * @param buf      Destination buffer
 * @param buf_size Size of destination buffer to fill in. Strings shorter than
 *                 this length will be padded with spaces.
 * @param src      Source string
 */
static void
ide_padstr8(uint8_t *buf, int buf_size, const char *src)
{
	int i;

	for (i = 0; i < buf_size; i++) {
		if (*src != '\0') {
			buf[i] = *src++;
		} else {
			buf[i] = ' ';
		}
	}
}

/**
 * Fill in ide->buffer with the output of the "IDENTIFY DEVICE" command
 */
static void ide_identify(IDE *ide)
{
	memset(ide->buffer, 0, 512);

	//ide->buffer[1] = 101; /* Cylinders */

	ide->buffer[1] = hdc[cur_ide[ide->board]].tracks; /* Cylinders */
	ide->buffer[3] = hdc[cur_ide[ide->board]].hpc;  /* Heads */
	ide->buffer[6] = hdc[cur_ide[ide->board]].spt;  /* Sectors */
	ide_padstr((char *) (ide->buffer + 10), "", 20); /* Serial Number */
	ide_padstr((char *) (ide->buffer + 23), emulator_version, 8); /* Firmware */
	ide_padstr((char *) (ide->buffer + 27), "86BoxHD", 40); /* Model */
	ide->buffer[20] = 3;   /*Buffer type*/
	ide->buffer[21] = 512; /*Buffer size*/
	ide->buffer[47] = 16;  /*Max sectors on multiple transfer command*/
	ide->buffer[48] = 1;   /*Dword transfers supported*/
	ide->buffer[49] = (1 << 9) | (1 << 8); /* LBA and DMA supported */
	ide->buffer[50] = 0x4000; /* Capabilities */
	ide->buffer[51] = 2 << 8; /*PIO timing mode*/
	ide->buffer[52] = 2 << 8; /*DMA timing mode*/
	ide->buffer[59] = ide->blocksize ? (ide->blocksize | 0x100) : 0;
	ide->buffer[60] = (hdc[cur_ide[ide->board]].tracks * hdc[cur_ide[ide->board]].hpc * hdc[cur_ide[ide->board]].spt) & 0xFFFF; /* Total addressable sectors (LBA) */
	ide->buffer[61] = (hdc[cur_ide[ide->board]].tracks * hdc[cur_ide[ide->board]].hpc * hdc[cur_ide[ide->board]].spt) >> 16;
	// ide->buffer[63] = 7; /*Multiword DMA*/
	ide->buffer[62] = ide->dma_identify_data[0];
	ide->buffer[63] = ide->dma_identify_data[1];
	ide->buffer[80] = 0xe; /*ATA-1 to ATA-3 supported*/
	ide->buffer[88] = ide->dma_identify_data[2];
}

/**
 * Fill in ide->buffer with the output of the "IDENTIFY PACKET DEVICE" command
 */
static void ide_atapi_identify(IDE *ide)
{
	memset(ide->buffer, 0, 512);

	ide->buffer[0] = 0x8000 | (5<<8) | 0x80 | (2<<5); /* ATAPI device, CD-ROM drive, removable media, accelerated DRQ */
	ide_padstr((char *) (ide->buffer + 10), "", 20); /* Serial Number */
	ide_padstr((char *) (ide->buffer + 23), emulator_version, 8); /* Firmware */
	ide_padstr((char *) (ide->buffer + 27), "86BoxCD", 40); /* Model */
	ide->buffer[49] = 0x200; /* LBA supported */
	ide->buffer[51] = 2 << 8; /*PIO timing mode*/
}

/*
 * Return the sector offset for the current register values
 */
static off64_t ide_get_sector(IDE *ide)
{
        if (ide->lba)
        {
                return (off64_t)ide->lba_addr + ide->skip512;
        }
        else
        {
        	int heads = ide->hpc;
        	int sectors = ide->spt;

        	return ((((off64_t) ide->cylinder * heads) + ide->head) *
        	          sectors) + (ide->sector - 1) + ide->skip512;
        }
}

/**
 * Move to the next sector using CHS addressing
 */
static void ide_next_sector(IDE *ide)
{
        if (ide->lba)
        {
                ide->lba_addr++;
        }
        else
        {
        	ide->sector++;
        	if (ide->sector == (ide->spt + 1)) {
        		ide->sector = 1;
        		ide->head++;
        		if (ide->head == ide->hpc) {
        			ide->head = 0;
        			ide->cylinder++;
                        }
		}
	}
}

static void loadhd(IDE *ide, int d, const char *fn)
{
	uint32_t sector_size = 512;
	uint32_t zero = 0;
	uint32_t full_size = 0;
	int c;
	ide->base = 0;
	ide->hdi = 0;
	if (ide->hdfile == NULL) {
		/* Try to open existing hard disk image */
		if (fn[0] == '.')
		{
			ide->type = IDE_NONE;
			return;
		}
		ide->hdfile = fopen64(fn, "rb+");
		if (ide->hdfile == NULL) {
			/* Failed to open existing hard disk image */
			if (errno == ENOENT) {
				/* Failed because it does not exist,
				   so try to create new file */
				ide->hdfile = fopen64(fn, "wb+");
				if (ide->hdfile == NULL) {
                                        ide->type = IDE_NONE;
/*					fatal("Cannot create file '%s': %s",
					      fn, strerror(errno));*/
					return;
				}
				else
				{
					if (image_is_hdi(fn))
					{
						full_size = hdc[d].spt * hdc[d].hpc * hdc[d].tracks * 512;
						ide->base = 0x1000;
						ide->hdi = 1;
						fwrite(&zero, 1, 4, ide->hdfile);
						fwrite(&zero, 1, 4, ide->hdfile);
						fwrite(&(ide->base), 1, 4, ide->hdfile);
						fwrite(&full_size, 1, 4, ide->hdfile);
						fwrite(&sector_size, 1, 4, ide->hdfile);
						fwrite(&(hdc[d].spt), 1, 4, ide->hdfile);
						fwrite(&(hdc[d].hpc), 1, 4, ide->hdfile);
						fwrite(&(hdc[d].tracks), 1, 4, ide->hdfile);
						for (c = 0; c < 0x3f8; c++)
						{
							fwrite(&zero, 1, 4, ide->hdfile);
						}
					}
					ide->hdc_num = d;
				}
			} else {
				/* Failed for another reason */
                                ide->type = IDE_NONE;
/*				fatal("Cannot open file '%s': %s",
				      fn, strerror(errno));*/
				return;
			}
		}
		else
		{
			if (image_is_hdi(fn))
			{
				fseek(ide->hdfile, 0x8, SEEK_SET);
				fread(&(ide->base), 1, 4, ide->hdfile);
				fseek(ide->hdfile, 0x10, SEEK_SET);
				fread(&sector_size, 1, 4, ide->hdfile);
				if (sector_size != 512)
				{
					/* Sector size is not 512 */
					fclose(ide->hdfile);
					ide->type = IDE_NONE;
					return;
				}
				fread(&(hdc[d].spt), 1, 4, ide->hdfile);
				fread(&(hdc[d].hpc), 1, 4, ide->hdfile);
				fread(&(hdc[d].tracks), 1, 4, ide->hdfile);
				ide->hdi = 1;
			}
		}
	}
	
    ide->spt = hdc[d].spt;
    ide->hpc = hdc[d].hpc;
    ide->tracks = hdc[d].tracks;
    ide->type = IDE_HDD;
	ide->hdc_num = d;
}

void ide_set_signature(IDE *ide)
{
	ide->secount=1;
	ide->sector=1;
	ide->head=0;
	ide->cylinder=(IDE_DRIVE_IS_CDROM(ide) ? 0xEB14 : ((ide->type == IDE_HDD) ? 0 : 0xFFFF));
	if (ide->type == IDE_HDD)  ide->drive = 0;
}

int ide_set_features(IDE *ide)
{
	uint8_t val = ide->secount & 7;

	if (ide->type == IDE_NONE)  return 0;
	
	switch(ide->cylprecomp)
	{
		case 0x02:
		case 0x82:
			return 0;
		case 0xcc:
		case 0x66:
		case 0xaa:
		case 0x55:
		case 0x05:
		case 0x85:
		case 0x69:
		case 0x67:
		case 0x96:
		case 0x9a:
		case 0x42:
		case 0xc2:
			return 1;
		case 0x03:
			if (ide->type == IDE_CDROM)
			{
				return 0;
			}
			switch(ide->secount >> 3)
			{
				case 0:
				case 1:
					ide->dma_identify_data[0] = ide->dma_identify_data[1] = 7;
					ide->dma_identify_data[2] = 0x3f;
					break;
				case 2:
					// if (ide->type == IDE_CDROM)  return 0;
					ide->dma_identify_data[0] = 7 | (1 << (val + 8));
					ide->dma_identify_data[1] = 7;
					ide->dma_identify_data[2] = 0x3f;
					break;
				case 4:
					// if (ide->type == IDE_CDROM)  return 0;
					ide->dma_identify_data[0] = 7;
					ide->dma_identify_data[1] = 7 | (1 << (val + 8));
					ide->dma_identify_data[2] = 0x3f;
					break;
				default:
					return 0;
			}
	}
	return 1;
}

void ide_set_sector(IDE *ide, int64_t sector_num)
{
	unsigned int cyl, r;
	if (ide->lba)
	{
		ide->head = (sector_num >> 24);
		ide->cylinder = (sector_num >> 8);
		ide->sector = (sector_num);
	}
	else
	{
		cyl = sector_num / (hdc[cur_ide[ide->board]].hpc * hdc[cur_ide[ide->board]].spt);
		r = sector_num % (hdc[cur_ide[ide->board]].hpc * hdc[cur_ide[ide->board]].spt);
		ide->cylinder = cyl;
		ide->head = ((r / hdc[cur_ide[ide->board]].spt) & 0x0f);
		ide->sector = (r % hdc[cur_ide[ide->board]].spt) + 1;
	}
}

void resetide(void)
{
        int d;

        /* Close hard disk image files (if previously open) */
        for (d = 0; d < 4; d++) {
                ide_drives[d].type = IDE_NONE;
                if (ide_drives[d].hdfile != NULL) {
                        fclose(ide_drives[d].hdfile);
                        ide_drives[d].hdfile = NULL;
                }
                ide_drives[d].atastat = READY_STAT | DSC_STAT;
                ide_drives[d].service = 0;
                ide_drives[d].board = (d & 2) ? 1 : 0;
        }
		
		for (d = 4; d < 6; d++)
		{
                ide_drives[d].type = IDE_NONE;
                ide_drives[d].atastat = READY_STAT | DSC_STAT;
                ide_drives[d].service = 0;
                ide_drives[d].board = 2;
		}
		
	page_flags[GPMODE_CDROM_AUDIO_PAGE] &= 0xFD;		/* Clear changed flag for CDROM AUDIO mode page. */
	memset(mode_pages_in[GPMODE_CDROM_AUDIO_PAGE], 0, 256);	/* Clear the page itself. */

	idecallback[0]=idecallback[1]=0;

	for (d = 0; d < 4; d++)
	{
		ide_drives[d].packetstatus = 0xFF;

		if ((atapi_cdrom_channel == d) && cdrom_enabled && !scsi_cdrom_enabled)
		{
			ide_drives[d].type = IDE_CDROM;
		}
		else
		{
			loadhd(&ide_drives[d], d, ide_fn[d]);
		}
			
		ide_set_signature(&ide_drives[d]);

		if (ide_drives[d].type != IDE_NONE)
		{
			ide_drives[d].dma_identify_data[0] = 7;
			ide_drives[d].dma_identify_data[1] = 7 | (1 << 15);
			ide_drives[d].dma_identify_data[2] = 0x3f;
		}
	}
		
		/* REMOVE WHEN SUBMITTING TO MAINLINE - START */
		for (d = 4; d < 6; d++)
		{
			ide_drives[d].packetstatus = 0xFF;

			if ((atapi_cdrom_channel == d) && cdrom_enabled && !scsi_cdrom_enabled)
			{
				ide_drives[d].type = IDE_CDROM;
			}
			else
			{
				ide_drives[d].type = IDE_NONE;
			}
			
			ide_set_signature(&ide_drives[d]);

			ide_drives[d].dma_identify_data[0] = 7;
			ide_drives[d].dma_identify_data[1] = 7 | (1 << 15);
			ide_drives[d].dma_identify_data[2] = 0x3f;
		}
		/* REMOVE WHEN SUBMITTING TO MAINLINE - END */
		
        cur_ide[0] = 0;
        cur_ide[1] = 2;

        cur_ide[2] = 4;
        
//        ide_drives[1].type = IDE_CDROM;

		page_flags[GPMODE_CDROM_AUDIO_PAGE] &= ~PAGE_CHANGED;

		SCSISense.UnitAttention = 0;
}

int idetimes=0;
void writeidew(int ide_board, uint16_t val)
{
        IDE *ide = &ide_drives[cur_ide[ide_board]];
        
        /*Some software issue excess writes after the 12 bytes required by the command, this will have all of them ignored*/
	if (ide->packetstatus && (ide->packetstatus != ATAPI_STATUS_PACKET_REQ))
                return;

//        pclog("Write IDEw %04X\n",val);
        ide->buffer[ide->pos >> 1] = val;
        ide->pos+=2;

        if (ide->packetstatus == ATAPI_STATUS_PACKET_REQ)
        {
			if ((ide->pos>=prefix_len+4) && (page_flags[page_current] & PAGE_CHANGEABLE))
			{
				mode_pages_in[page_current][ide->pos - prefix_len - 4] = ((uint8_t *) ide->buffer)[ide->pos - 2];
				mode_pages_in[page_current][ide->pos - prefix_len - 3] = ((uint8_t *) ide->buffer)[ide->pos - 1];
			}
                if (ide->pos>=(ide->packlen+2))
                {
                        ide->packetstatus = ATAPI_STATUS_PACKET_RECEIVED;
                        timer_process();
                        idecallback[ide_board]=6*IDE_TIME;
                        timer_update_outstanding();
//                        pclog("Packet over!\n");
                        ide_irq_lower(ide);
                }
                return;
        }
        else if (ide->packetstatus == ATAPI_STATUS_PACKET_RECEIVED)
                return;
        else if (ide->command == WIN_PACKETCMD && ide->pos>=0xC)
        {
                ide->pos=0;
                ide->atastat = BUSY_STAT;
                ide->packetstatus = ATAPI_STATUS_COMMAND;
/*                idecallback[ide_board]=6*IDE_TIME;*/
		timer_process();
                callbackide(ide_board);
                timer_update_outstanding();
//                idecallback[ide_board]=60*IDE_TIME;
//                if ((ide->buffer[0]&0xFF)==0x43) idecallback[ide_board]=1*IDE_TIME;
//                pclog("Packet now waiting!\n");
/*                if (ide->buffer[0]==0x243)
                {
                        idetimes++;
                        output=3;
                }*/
        }
        else if (ide->pos>=512)
        {
                ide->pos=0;
                ide->atastat = BUSY_STAT;
                timer_process();
                if (ide->command == WIN_WRITE_MULTIPLE)
                   callbackide(ide_board);
                else
              	   idecallback[ide_board]=6*IDE_TIME;
                timer_update_outstanding();
        }
}

void writeidel(int ide_board, uint32_t val)
{
//        pclog("WriteIDEl %08X\n", val);
        writeidew(ide_board, val);
        writeidew(ide_board, val >> 16);
}

void writeide(int ide_board, uint16_t addr, uint8_t val)
{
        IDE *ide = &ide_drives[cur_ide[ide_board]];
        IDE *ide_other = &ide_drives[cur_ide[ide_board] ^ 1];

//        if ((cr0&1) && !(eflags&VM_FLAG))
//         pclog("WriteIDE %04X %02X from %04X(%08X):%08X %i\n", addr, val, CS, cs, pc, ins);
//        return;
        addr|=0x80;
		/* ONLY FOR EXPERIMENTAL */
		addr|=0x10;			/* 1F0 | 10 = 1F0, 1E8 | 10 = 1F8 */
		addr&=0xFFF7;		/* 1F0 & FFF7 = 1F0, 1F8 | FFF7 = 1F0 */
//        if (ide_board) pclog("Write IDEb %04X %02X %04X(%08X):%04X %i  %02X %02X\n",addr,val,CS,cs,pc,ins,ide->atastat,ide_drives[0].atastat);
        /*if (idedebug) */
//        pclog("Write IDE %08X %02X %04X:%08X\n",addr,val,CS,pc);
//        int c;
//      rpclog("Write IDE %08X %02X %08X %08X\n",addr,val,PC,armregs[12]);

        if (ide->type == IDE_NONE && (addr == 0x1f0 || addr == 0x1f7)) return;
        
        switch (addr)
        {
        case 0x1F0: /* Data */
                writeidew(ide_board, val | (val << 8));
                return;

        case 0x1F1: /* Features */
                ide->cylprecomp = val;
                ide_other->cylprecomp = val;
                return;

        case 0x1F2: /* Sector count */
                ide->secount = val;
                ide_other->secount = val;
                return;

        case 0x1F3: /* Sector */
                ide->sector = val;
                ide->lba_addr = (ide->lba_addr & 0xFFFFF00) | val;
                ide_other->sector = val;
                ide_other->lba_addr = (ide_other->lba_addr & 0xFFFFF00) | val;
                return;

        case 0x1F4: /* Cylinder low */
                ide->cylinder = (ide->cylinder & 0xFF00) | val;
                ide->lba_addr = (ide->lba_addr & 0xFFF00FF) | (val << 8);
                ide_other->cylinder = (ide_other->cylinder&0xFF00) | val;
                ide_other->lba_addr = (ide_other->lba_addr&0xFFF00FF) | (val << 8);
//                pclog("Write cylinder low %02X\n",val);
                return;

        case 0x1F5: /* Cylinder high */
                ide->cylinder = (ide->cylinder & 0xFF) | (val << 8);
                ide->lba_addr = (ide->lba_addr & 0xF00FFFF) | (val << 16);
                ide_other->cylinder = (ide_other->cylinder & 0xFF) | (val << 8);
                ide_other->lba_addr = (ide_other->lba_addr & 0xF00FFFF) | (val << 16);
                return;

        case 0x1F6: /* Drive/Head */
/*        if (val==0xB0)
        {
                dumpregs();
                exit(-1);
        }*/

                if (cur_ide[ide_board] != ((val>>4)&1)+(ide_board<<1))
                {
                        cur_ide[ide_board]=((val>>4)&1)+(ide_board<<1);

                        if (ide->reset || ide_other->reset)
                        {
                                ide->atastat = ide_other->atastat = READY_STAT | DSC_STAT;
                                ide->error = ide_other->error = 1;
                                ide->secount = ide_other->secount = 1;
                                ide->sector = ide_other->sector = 1;
                                ide->head = ide_other->head = 0;
                                ide->cylinder = ide_other->cylinder = 0;
                                ide->reset = ide_other->reset = 0;
                                // ide->blocksize = ide_other->blocksize = 0;
                                if (IDE_DRIVE_IS_CDROM(ide))
                                        ide->cylinder=0xEB14;
                                if (IDE_DRIVE_IS_CDROM(ide_other))
                                        ide_other->cylinder=0xEB14;

                                idecallback[ide_board] = 0;
                                timer_update_outstanding();
                                return;
                        }

                        ide = &ide_drives[cur_ide[ide_board]];
                }
                                
                ide->head = val & 0xF;
                ide->lba = val & 0x40;
                ide_other->head = val & 0xF;
                ide_other->lba = val & 0x40;
                
                ide->lba_addr = (ide->lba_addr & 0x0FFFFFF) | ((val & 0xF) << 24);
                ide_other->lba_addr = (ide_other->lba_addr & 0x0FFFFFF)|((val & 0xF) << 24);

                ide_irq_update(ide);
                return;

        case 0x1F7: /* Command register */
        if (ide->type == IDE_NONE) return;
//                pclog("IDE command %02X drive %i\n",val,ide.drive);
        ide_irq_lower(ide);
                ide->command=val;
                
                // pclog("New IDE command - %02X %i %i\n",ide->command,cur_ide[ide_board],ide_board);
                ide->error=0;
                switch (val)
                {
                case WIN_SRST: /* ATAPI Device Reset */
                        if (IDE_DRIVE_IS_CDROM(ide)) ide->atastat = BUSY_STAT;
                        else                         ide->atastat = READY_STAT;
                        timer_process();
                        idecallback[ide_board]=100*IDE_TIME;
                        timer_update_outstanding();
                        return;

                case WIN_RESTORE:
                case WIN_SEEK:
//                        pclog("WIN_RESTORE start\n");
                        ide->atastat = READY_STAT;
                        timer_process();
                        idecallback[ide_board]=100*IDE_TIME;
                        timer_update_outstanding();
                        return;

                case WIN_READ_MULTIPLE:
                /* Fatal removed in accordance with the official ATAPI reference:

				   If the Read Multiple command is attempted before the Set Multiple Mode
                   command  has  been  executed  or  when  Read  Multiple  commands  are
                   disabled, the Read Multiple operation is rejected with an Aborted Com-
                   mand error. */
			/* if (!ide->blocksize && (ide->type != IDE_CDROM))
                           fatal("READ_MULTIPLE - blocksize = 0\n"); */
#if 0
                        if (ide->lba) pclog("Read Multiple %i sectors from LBA addr %07X\n",ide->secount,ide->lba_addr);
                        else          pclog("Read Multiple %i sectors from sector %i cylinder %i head %i  %i\n",ide->secount,ide->sector,ide->cylinder,ide->head,ins);
#endif
                        ide->blockcount = 0;
                        
                case WIN_READ:
                case WIN_READ_NORETRY:
                case WIN_READ_DMA:
/*                        if (ide.secount>1)
                        {
                                fatal("Read %i sectors from sector %i cylinder %i head %i\n",ide.secount,ide.sector,ide.cylinder,ide.head);
                        }*/
#if 0
                        if (ide->lba) pclog("Read %i sectors from LBA addr %07X\n",ide->secount,ide->lba_addr);
                        else          pclog("Read %i sectors from sector %i cylinder %i head %i  %i\n",ide->secount,ide->sector,ide->cylinder,ide->head,ins);
#endif
                        ide->atastat = BUSY_STAT;
                        timer_process();
                        idecallback[ide_board]=200*IDE_TIME;
                        timer_update_outstanding();
                        return;
                        
                case WIN_WRITE_MULTIPLE:
                        if (!ide->blocksize && (ide->type != IDE_CDROM))
                           fatal("Write_MULTIPLE - blocksize = 0\n");
#if 0
                        if (ide->lba) pclog("Write Multiple %i sectors from LBA addr %07X\n",ide->secount,ide->lba_addr);
                        else          pclog("Write Multiple %i sectors to sector %i cylinder %i head %i\n",ide->secount,ide->sector,ide->cylinder,ide->head);
#endif
                        ide->blockcount = 0;
                        
                case WIN_WRITE:
                case WIN_WRITE_NORETRY:
                /*                        if (ide.secount>1)
                        {
                                fatal("Write %i sectors to sector %i cylinder %i head %i\n",ide.secount,ide.sector,ide.cylinder,ide.head);
                        }*/
#if 0
                        if (ide->lba) pclog("Write %i sectors from LBA addr %07X\n",ide->secount,ide->lba_addr);
                        else          pclog("Write %i sectors to sector %i cylinder %i head %i\n",ide->secount,ide->sector,ide->cylinder,ide->head);
#endif
                        ide->atastat = DRQ_STAT | DSC_STAT | READY_STAT;
                        ide->pos=0;
                        return;

                case WIN_WRITE_DMA:
#if 0
                        if (ide->lba) pclog("Write %i sectors from LBA addr %07X\n",ide->secount,ide->lba_addr);
                        else          pclog("Write %i sectors to sector %i cylinder %i head %i\n",ide->secount,ide->sector,ide->cylinder,ide->head);
#endif
                        ide->atastat = BUSY_STAT;
                        timer_process();
                        idecallback[ide_board]=200*IDE_TIME;
                        timer_update_outstanding();
                        return;

                case WIN_VERIFY:
				case WIN_VERIFY_ONCE:
#if 0
                        if (ide->lba) pclog("Read verify %i sectors from LBA addr %07X\n",ide->secount,ide->lba_addr);
                        else          pclog("Read verify %i sectors from sector %i cylinder %i head %i\n",ide->secount,ide->sector,ide->cylinder,ide->head);
#endif
                        ide->atastat = BUSY_STAT;
                        timer_process();
                        idecallback[ide_board]=200*IDE_TIME;
                        timer_update_outstanding();
                        return;

                case WIN_FORMAT:
//                        pclog("Format track %i head %i\n",ide.cylinder,ide.head);
                        ide->atastat = DRQ_STAT;
//                        idecallback[ide_board]=200;
                        ide->pos=0;
                        return;

                case WIN_SPECIFY: /* Initialize Drive Parameters */
                        ide->atastat = BUSY_STAT;
                        timer_process();
                        idecallback[ide_board]=30*IDE_TIME;
                        timer_update_outstanding();
//                        pclog("SPECIFY\n");
//                        output=1;
                        return;

                case WIN_DRIVE_DIAGNOSTICS: /* Execute Drive Diagnostics */
                case WIN_PIDENTIFY: /* Identify Packet Device */
                case WIN_SET_MULTIPLE_MODE: /*Set Multiple Mode*/
//                output=1;
				case WIN_NOP:
				case WIN_STANDBYNOW1:
                case WIN_SETIDLE1: /* Idle */
				case WIN_CHECKPOWERMODE1:
                        ide->atastat = BUSY_STAT;
                        timer_process();
						idecallback[ide_board]=30*IDE_TIME;
                        // callbackide(ide_board);
//                        idecallback[ide_board]=200*IDE_TIME;
                        timer_update_outstanding();
                        return;

                case WIN_IDENTIFY: /* Identify Device */
                case WIN_SET_FEATURES:
				case WIN_READ_NATIVE_MAX:
//                        output=3;
//                        timetolive=500;
                        ide->atastat = BUSY_STAT;
                        timer_process();
                        idecallback[ide_board]=200*IDE_TIME;
                        timer_update_outstanding();
                        return;

                case WIN_PACKETCMD: /* ATAPI Packet */
                        ide->packetstatus = ATAPI_STATUS_IDLE;
                        ide->atastat = BUSY_STAT;
                        timer_process();
                        idecallback[ide_board]=1;//30*IDE_TIME;
                        timer_update_outstanding();
                        ide->pos=0;
                        return;
                        
                case 0xF0:
                        default:
                	ide->atastat = READY_STAT | ERR_STAT | DSC_STAT;
                	ide->error = ABRT_ERR;
                        ide_irq_raise(ide);
/*                        fatal("Bad IDE command %02X\n", val);*/
                        // pclog("Bad IDE command %02X\n", val);
                        return;
                }
                
                return;

        case 0x3F6: /* Device control */
                if ((ide->fdisk&4) && !(val&4) && (ide->type != IDE_NONE || ide_other->type != IDE_NONE))
                {
			timer_process();
                        idecallback[ide_board]=500*IDE_TIME;
                        timer_update_outstanding();
                        ide->reset = ide_other->reset = 1;
                        ide->atastat = ide_other->atastat = BUSY_STAT;
//                        pclog("IDE Reset %i\n", ide_board);
                }
                ide->fdisk = ide_other->fdisk = val;
                ide_irq_update(ide);
                return;
        }
//        fatal("Bad IDE write %04X %02X\n", addr, val);
}

uint8_t readide(int ide_board, uint16_t addr)
{
        IDE *ide = &ide_drives[cur_ide[ide_board]];
        uint8_t temp;
        uint16_t tempw;

        addr|=0x80;
		/* ONLY FOR EXPERIMENTAL */
		addr|=0x10;			/* 1F0 | 10 = 1F0, 1E8 | 10 = 1F8 */
		addr&=0xFFF7;		/* 1F0 & FFF7 = 1F0, 1F8 | FFF7 = 1F0 */

//        if ((cr0&1) && !(eflags&VM_FLAG))
//         pclog("ReadIDE %04X  from %04X(%08X):%08X\n", addr, CS, cs, pc);
//        return 0xFF;

        if (ide->type == IDE_NONE && (addr == 0x1f0 || addr == 0x1f7)) return 0;
//        /*if (addr!=0x1F7 && addr!=0x3F6) */pclog("Read IDEb %04X %02X %02X %i %04X:%04X %i  %04X\n",addr,ide->atastat,(ide->atastat & ~DSC_STAT) | (ide->service ? SERVICE_STAT : 0),cur_ide[ide_board],CS,pc,ide_board, BX);
//rpclog("Read IDE %08X %08X %02X\n",addr,PC,iomd.irqb.mask);
        switch (addr)
        {
        case 0x1F0: /* Data */
                tempw = readidew(ide_board);
//                pclog("Read IDEW %04X\n", tempw);                
                temp = tempw & 0xff;
                break;
                
        case 0x1F1: /* Error */
//        pclog("Read error %02X\n",ide.error);
                temp = ide->error;
                break;

        case 0x1F2: /* Sector count */
//        pclog("Read sector count %02X\n",ide->secount);
                temp = (uint8_t)ide->secount;
                break;

        case 0x1F3: /* Sector */
                temp = (uint8_t)ide->sector;
                break;

        case 0x1F4: /* Cylinder low */
//        pclog("Read cyl low %02X\n",ide.cylinder&0xFF);
                temp = (uint8_t)(ide->cylinder&0xFF);
                break;

        case 0x1F5: /* Cylinder high */
//        pclog("Read cyl low %02X\n",ide.cylinder>>8);
                temp = (uint8_t)(ide->cylinder>>8);
                break;

        case 0x1F6: /* Drive/Head */
                temp = (uint8_t)(ide->head | ((cur_ide[ide_board] & 1) ? 0x10 : 0) | (ide->lba ? 0x40 : 0) | 0xa0);
                break;

        case 0x1F7: /* Status */
                if (ide->type == IDE_NONE)
                {
//                        pclog("Return status 00\n");
                        temp = 0;
                        break;
                }
                ide_irq_lower(ide);
                if (ide->type == IDE_CDROM)
                {
//                        pclog("Read CDROM status %02X\n",(ide->atastat & ~DSC_STAT) | (ide->service ? SERVICE_STAT : 0));
                        temp = (ide->atastat & ~DSC_STAT) | (ide->service ? SERVICE_STAT : 0);
                }
                else
                {
//                 && ide->service) return ide.atastat[ide.board]|SERVICE_STAT;
//                pclog("Return status %02X %04X:%04X %02X %02X\n",ide->atastat, CS ,pc, AH, BH);
                        temp = ide->atastat;
                }
                break;

        case 0x3F6: /* Alternate Status */
//        pclog("3F6 read %02X\n",ide.atastat[ide.board]);
//        if (output) output=0;
                if (ide->type == IDE_NONE)
                {
//                        pclog("Return status 00\n");
                        temp = 0;
                        break;
                }
                if (ide->type == IDE_CDROM)
                {
//                        pclog("Read CDROM status %02X\n",(ide->atastat & ~DSC_STAT) | (ide->service ? SERVICE_STAT : 0));
                        temp = (ide->atastat & ~DSC_STAT) | (ide->service ? SERVICE_STAT : 0);
                }
                else
                {
//                 && ide->service) return ide.atastat[ide.board]|SERVICE_STAT;
//                pclog("Return status %02X\n",ide->atastat);
                        temp = ide->atastat;
                }
                break;
        }
//        if (ide_board) pclog("Read IDEb %04X %02X   %02X %02X %i %04X:%04X %i\n", addr, temp, ide->atastat,(ide->atastat & ~DSC_STAT) | (ide->service ? SERVICE_STAT : 0),cur_ide[ide_board],CS,pc,ide_board);
        return temp;
//        fatal("Bad IDE read %04X\n", addr);
}

uint16_t readidew(int ide_board)
{
        IDE *ide = &ide_drives[cur_ide[ide_board]];
        uint16_t temp;

//        return 0xFFFF;
//        pclog("Read IDEw %04X %04X:%04X %02X %i %i\n",ide->buffer[ide->pos >> 1],CS,pc,opcode,ins, ide->pos);
        
//if (idedebug) pclog("Read IDEW %08X\n",PC);

        temp = ide->buffer[ide->pos >> 1];

        ide->pos+=2;
	if ((ide->command == WIN_PACKETCMD) && ((ide->packetstatus == ATAPI_STATUS_REQ_SENSE) || (ide->packetstatus==8)))
	{
		callnonreadcd(ide);
		return temp;
	}
        if ((ide->pos>=512 && ide->command != WIN_PACKETCMD) || (ide->command == WIN_PACKETCMD && ide->pos>=ide->packlen))
        {
//                pclog("Over! packlen %i %i\n",ide->packlen,ide->pos);
                ide->pos=0;
                if (ide->command == WIN_PACKETCMD)// && ide.packetstatus==6)
                {
//                        pclog("Call readCD\n");
                        callreadcd(ide);
                }
                else
                {
                        ide->atastat = READY_STAT | DSC_STAT;
                        ide->packetstatus = ATAPI_STATUS_IDLE;
                        if (ide->command == WIN_READ || ide->command == WIN_READ_NORETRY || ide->command == WIN_READ_MULTIPLE)
                        {
                                ide->secount = (ide->secount - 1) & 0xff;
                                if (ide->secount)
                                {
                                        ide_next_sector(ide);
                                        ide->atastat = BUSY_STAT;
                                        timer_process();
                                        if (ide->command == WIN_READ_MULTIPLE)
                                           callbackide(ide_board);
                                        else
                                           idecallback[ide_board]=6*IDE_TIME;
                                        timer_update_outstanding();
//                                        pclog("set idecallback\n");
//                                        callbackide(ide_board);
                                }
//                                else
//                                   pclog("readidew done %02X\n", ide->atastat);
                        }
                }
        }
//        pclog("Read IDEw %04X\n",temp);
        return temp;
}

uint32_t readidel(int ide_board)
{
        uint16_t temp;
//        pclog("Read IDEl %i\n", ide_board);
        temp = readidew(ide_board);
        return temp | (readidew(ide_board) << 16);
}

int times30=0;
void callbackide(int ide_board)
{
        IDE *ide = &ide_drives[cur_ide[ide_board]];
        IDE *ide_other = &ide_drives[cur_ide[ide_board] ^ 1];
        off64_t addr;
		uint64_t faddr;
        int c;
        ext_ide = ide;
		int64_t snum;
//        return;
        if (ide->command==0x30) times30++;
//        if (times30==2240) output=1;
        //if (times30==2471 && ide->command==0xA0) output=1;
///*if (ide_board) */pclog("CALLBACK %02X %i %i  %i\n",ide->command,times30,ide->reset,cur_ide[ide_board]);
//        if (times30==1294)
//                output=1;
        if (ide->reset)
        {
                ide->atastat = ide_other->atastat = READY_STAT | DSC_STAT;
                ide->error = ide_other->error = 1;
                ide->secount = ide_other->secount = 1;
                ide->sector = ide_other->sector = 1;
                ide->head = ide_other->head = 0;
                ide->cylinder = ide_other->cylinder = 0;
                ide->reset = ide_other->reset = 0;
                if (IDE_DRIVE_IS_CDROM(ide))
                {
                        ide->cylinder=0xEB14;
                        cdrom->stop();
                }
                if (ide->type == IDE_NONE)
                {
                        ide->cylinder=0xFFFF;
                        cdrom->stop();
                }
                if (IDE_DRIVE_IS_CDROM(ide_other))
                {
                        ide_other->cylinder=0xEB14;
                        cdrom->stop();
                }
                if (ide_other->type == IDE_NONE)
                {
                        ide_other->cylinder=0xFFFF;
                        cdrom->stop();
                }
//                pclog("Reset callback\n");
                return;
        }
        switch (ide->command)
        {
                //Initialize the Task File Registers as follows: Status = 00h, Error = 01h, Sector Count = 01h, Sector Number = 01h, Cylinder Low = 14h, Cylinder High =EBh and Drive/Head = 00h.
        case WIN_SRST: /*ATAPI Device Reset */
                ide->atastat = READY_STAT | DSC_STAT;
                ide->error=1; /*Device passed*/
                ide->secount = ide->sector = 1;
		ide_set_signature(ide);
		if (IDE_DRIVE_IS_CDROM(ide))
			ide->atastat = 0;
                ide_irq_raise(ide);
                if (IDE_DRIVE_IS_CDROM(ide))
                   ide->service = 0;
                return;

        case WIN_RESTORE:
        case WIN_SEEK:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        pclog("WIN_RESTORE callback on CD-ROM\n");
                        goto abort_cmd;
                }
		case WIN_NOP:
		case WIN_STANDBYNOW1:
		case WIN_SETIDLE1:
//                pclog("WIN_RESTORE callback\n");
                ide->atastat = READY_STAT | DSC_STAT;
                ide_irq_raise(ide);
                return;

		case WIN_CHECKPOWERMODE1:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
				ide->secount = 0xFF;
                ide->atastat = READY_STAT | DSC_STAT;
                ide_irq_raise(ide);
                return;

        case WIN_READ:
        case WIN_READ_NORETRY:
                if (IDE_DRIVE_IS_CDROM(ide)) {
			ide_set_signature(ide);
                        goto abort_cmd;
                }
                addr = ide_get_sector(ide) * 512;
//                pclog("Read %i %i %i %08X\n",ide.cylinder,ide.head,ide.sector,addr);
                /*                if (ide.cylinder || ide.head)
                {
                        fatal("Read from other cylinder/head");
                }*/
                fseeko64(ide->hdfile, ide->base + addr, SEEK_SET);
                fread(ide->buffer, 512, 1, ide->hdfile);
                ide->pos=0;
                ide->atastat = DRQ_STAT | READY_STAT | DSC_STAT;
//                pclog("Read sector callback %i %i %i offset %08X %i left %i %02X\n",ide.sector,ide.cylinder,ide.head,addr,ide.secount,ide.spt,ide.atastat[ide.board]);
//                if (addr) output=3;
                ide_irq_raise(ide);
#ifndef RPCEMU_IDE
                readflash=1;
#endif
                return;

        case WIN_READ_DMA:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                addr = ide_get_sector(ide) * 512;
                fseeko64(ide->hdfile, addr, SEEK_SET);
                fread(ide->buffer, 512, 1, ide->hdfile);
                ide->pos=0;
                
                if (ide_bus_master_read_sector)
                {
                        if (ide_bus_master_read_sector(ide_board, (uint8_t *)ide->buffer))
                           idecallback[ide_board]=6*IDE_TIME;           /*DMA not performed, try again later*/
                        else
                        {
                                /*DMA successful*/
                                ide->atastat = DRQ_STAT | READY_STAT | DSC_STAT;

                                ide->secount = (ide->secount - 1) & 0xff;
                                if (ide->secount)
                                {
                                        ide_next_sector(ide);
                                        ide->atastat = BUSY_STAT;
                                        idecallback[ide_board]=6*IDE_TIME;
                                }
                                else
                                {
                                        ide_irq_raise(ide);
                                }
                        }
                }
#ifndef RPCEMU_IDE
                readflash=1;
#endif
                return;

        case WIN_READ_MULTIPLE:
                /* According to the official ATA reference:

				   If the Read Multiple command is attempted before the Set Multiple Mode
                   command  has  been  executed  or  when  Read  Multiple  commands  are
                   disabled, the Read Multiple operation is rejected with an Aborted Com-
                   mand error. */
				if (IDE_DRIVE_IS_CDROM(ide) || !ide->blocksize) {
                        goto abort_cmd;
                }

                addr = ide_get_sector(ide) * 512;
//                pclog("Read multiple from %08X %i (%i) %i\n", addr, ide->blockcount, ide->blocksize, ide->secount);
                fseeko64(ide->hdfile, ide->base + addr, SEEK_SET);
                fread(ide->buffer, 512, 1, ide->hdfile);
                ide->pos=0;
                ide->atastat = DRQ_STAT | READY_STAT | DSC_STAT;
                if (!ide->blockcount)// || ide->secount == 1)
                {
//                        pclog("Read multiple int\n");
                        ide_irq_raise(ide);
                }                        
                ide->blockcount++;
                if (ide->blockcount >= ide->blocksize)
                   ide->blockcount = 0;
#ifndef RPCEMU_IDE
                readflash=1;
#endif
                return;

        case WIN_WRITE:
        case WIN_WRITE_NORETRY:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                addr = ide_get_sector(ide) * 512;
//                pclog("Write sector callback %i %i %i offset %08X %i left %i\n",ide.sector,ide.cylinder,ide.head,addr,ide.secount,ide.spt);
                fseeko64(ide->hdfile, ide->base + addr, SEEK_SET);
                fwrite(ide->buffer, 512, 1, ide->hdfile);
                ide_irq_raise(ide);
                ide->secount = (ide->secount - 1) & 0xff;
                if (ide->secount)
                {
                        ide->atastat = DRQ_STAT | READY_STAT | DSC_STAT;
                        ide->pos=0;
                        ide_next_sector(ide);
                }
                else
                   ide->atastat = READY_STAT | DSC_STAT;
#ifndef RPCEMU_IDE
                readflash=1;
#endif
                return;
                
        case WIN_WRITE_DMA:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }

                if (ide_bus_master_write_sector)
                {
                        if (ide_bus_master_write_sector(ide_board, (uint8_t *)ide->buffer))
                           idecallback[ide_board]=6*IDE_TIME;           /*DMA not performed, try again later*/
                        else
                        {
                                /*DMA successful*/
                                /*if(IDE_DRIVE_IS_CDROM(ide))
                                {
                                }
                                else
                                {*/
                                        addr = ide_get_sector(ide) * 512;
                                        fseeko64(ide->hdfile, ide->base + addr, SEEK_SET);
                                        fwrite(ide->buffer, 512, 1, ide->hdfile);
                                //}
                                
                                ide->atastat = DRQ_STAT | READY_STAT | DSC_STAT;

                                ide->secount = (ide->secount - 1) & 0xff;
                                if (ide->secount)
                                {
                                        ide_next_sector(ide);
                                        ide->atastat = BUSY_STAT;
                                        idecallback[ide_board]=6*IDE_TIME;
                                }
                                else
                                {
                                        ide_irq_raise(ide);
                                }
                        }
                }
#ifndef RPCEMU_IDE
                readflash=1;
#endif
                return;

        case WIN_WRITE_MULTIPLE:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                addr = ide_get_sector(ide) * 512;
//                pclog("Write sector callback %i %i %i offset %08X %i left %i\n",ide.sector,ide.cylinder,ide.head,addr,ide.secount,ide.spt);
                fseeko64(ide->hdfile, ide->base + addr, SEEK_SET);
                fwrite(ide->buffer, 512, 1, ide->hdfile);
                ide->blockcount++;
                if (ide->blockcount >= ide->blocksize || ide->secount == 1)
                {
                        ide->blockcount = 0;
                        ide_irq_raise(ide);
                }
                ide->secount = (ide->secount - 1) & 0xff;
                if (ide->secount)
                {
                        ide->atastat = DRQ_STAT | READY_STAT | DSC_STAT;
                        ide->pos=0;
                        ide_next_sector(ide);
                }
                else
                   ide->atastat = READY_STAT | DSC_STAT;
#ifndef RPCEMU_IDE
                readflash=1;
#endif
                return;

		case WIN_VERIFY:
		case WIN_VERIFY_ONCE:
				if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                ide->pos=0;
                ide->atastat = READY_STAT | DSC_STAT;
//                pclog("Read verify callback %i %i %i offset %08X %i left\n",ide.sector,ide.cylinder,ide.head,addr,ide.secount);
                ide_irq_raise(ide);
#ifndef RPCEMU_IDE
                readflash=1;
#endif
                return;

        case WIN_FORMAT:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                addr = ide_get_sector(ide) * 512;
//                pclog("Format cyl %i head %i offset %08X %08X %08X secount %i\n",ide.cylinder,ide.head,addr,addr>>32,addr,ide.secount);
                fseeko64(ide->hdfile, ide->base + addr, SEEK_SET);
                memset(ide->buffer, 0, 512);
                for (c=0;c<ide->secount;c++)
                {
                        fwrite(ide->buffer, 512, 1, ide->hdfile);
                }
                ide->atastat = READY_STAT | DSC_STAT;
                ide_irq_raise(ide);
#ifndef RPCEMU_IDE
                readflash=1;
#endif
                return;

        case WIN_DRIVE_DIAGNOSTICS:
		ide_set_signature(ide);
                ide->error=1; /*No error detected*/
		if (IDE_DRIVE_IS_CDROM(ide))
		{
			ide->atastat = 0;
		}
		else
		{
			ide->atastat = READY_STAT | DSC_STAT;
			ide_irq_raise(ide);
		}
                return;

        case WIN_SPECIFY: /* Initialize Drive Parameters */
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                ide->spt=ide->secount;
                ide->hpc=ide->head+1;
#if 0
				if (ide->hdi)
				{
					faddr = ftello64(ide->hdfile);
					if (hdc[ide->hdc_num].spt != ide->spt)
					{
						fseeko64(ide->hdfile, 0x34, SEEK_SET);
						fwrite(&(ide->spt), 1, 4, ide->hdfile);
					}
					if (hdc[ide->hdc_num].hpc != ide->hpc)
					{
						fseeko64(ide->hdfile, 0x38, SEEK_SET);
						fwrite(&(ide->hpc), 1, 4, ide->hdfile);
					}
					fseeko64(ide->hdfile, faddr, SEEK_SET);					
				}
#endif
#if 0
				/* Make sure other parts of the emulator are aware the sectors and heads have changed. */
				hdc[ide->hdc_num].spt = ide->spt;
				hdc[ide->hdc_num].hpc = ide->hpc;
#endif
                ide->atastat = READY_STAT | DSC_STAT;
#ifndef RPCEMU_IDE
//                pclog("SPECIFY - %i sectors per track, %i heads per cylinder  %i %i\n",ide->spt,ide->hpc,cur_ide[ide_board],ide_board);
#endif
                ide_irq_raise(ide);
                return;

        case WIN_PIDENTIFY: /* Identify Packet Device */
                if (IDE_DRIVE_IS_CDROM(ide)) {
//                        pclog("ATAPI identify\n");
                        ide_atapi_identify(ide);
                        ide->pos=0;
                        ide->error=0;
                        ide->atastat = DRQ_STAT | READY_STAT | DSC_STAT;
                        ide_irq_raise(ide);
                        return;
                }
//                pclog("Not ATAPI\n");
                goto abort_cmd;

        case WIN_SET_MULTIPLE_MODE:
                if (IDE_DRIVE_IS_CDROM(ide)) {
                        goto abort_cmd;
                }
                ide->blocksize = ide->secount;
                ide->atastat = READY_STAT | DSC_STAT;
#ifndef RPCEMU_IDE
                pclog("Set multiple mode - %i\n", ide->blocksize);
#endif
                ide_irq_raise(ide);
                return;
                

		// case WIN_SETIDLE1: /* Idle */
                // goto abort_cmd;

		case WIN_SET_FEATURES:
				if (!(ide_set_features(ide)))  goto abort_cmd;
                ide->atastat = READY_STAT | DSC_STAT;
                ide_irq_raise(ide);
				return;
				
		case WIN_READ_NATIVE_MAX:
				if (ide->type != IDE_HDD)  goto abort_cmd;
				snum = hdc[cur_ide[ide->board]].spt;
				snum *= hdc[cur_ide[ide->board]].hpc;
				snum *= hdc[cur_ide[ide->board]].tracks;
				ide_set_sector(ide, snum - 1);
                ide->atastat = READY_STAT | DSC_STAT;
                ide_irq_raise(ide);
				return;
				
        case WIN_IDENTIFY: /* Identify Device */
		if (ide->type == IDE_NONE)
		{
			goto abort_cmd;
		}
                if (IDE_DRIVE_IS_CDROM(ide))
		{
			ide_set_signature(ide);
			goto abort_cmd;
		}
		else
		{
                        ide_identify(ide);
                        ide->pos=0;
                        ide->atastat = DRQ_STAT | READY_STAT | DSC_STAT;
//                pclog("ID callback\n");
                        ide_irq_raise(ide);
                }
                return;

        case WIN_PACKETCMD: /* ATAPI Packet */
                if (!IDE_DRIVE_IS_CDROM(ide)) goto abort_cmd;
//                pclog("Packet callback! %i %08X\n",ide->packetstatus,ide);

                if (ide->packetstatus == ATAPI_STATUS_IDLE)
                {
			readcdmode=0;
                        ide->pos=0;
                        ide->secount = (uint8_t)((ide->secount&0xF8)|1);
                        ide->atastat = READY_STAT | DRQ_STAT |(ide->atastat&ERR_STAT);
                        //ide_irq_raise(ide);
//                        pclog("1 Preparing to recieve packet max DRQ count %04X\n",ide->cylinder);
                }
                else if (ide->packetstatus == ATAPI_STATUS_COMMAND)
                {
                        ide->atastat = BUSY_STAT|(ide->atastat&ERR_STAT);
//                        pclog("Running ATAPI command 2\n");
                        atapicommand(ide_board);
//                        exit(-1);
                }
                else if (ide->packetstatus == ATAPI_STATUS_COMPLETE)
                {
//                        pclog("packetstatus==2\n");
                        ide->atastat = READY_STAT;
                        ide->secount=3;
                        ide_irq_raise(ide);
//                        if (iomd.irqb.mask&2) output=1;
                }
                else if (ide->packetstatus == ATAPI_STATUS_DATA)
                {
                        ide->atastat = READY_STAT|DRQ_STAT|(ide->atastat&ERR_STAT);
//                        rpclog("Recieve data packet 3! %02X\n",ide->atastat);
                        ide_irq_raise(ide);
                        ide->packetstatus=0xFF;
                }
                else if (ide->packetstatus == ATAPI_STATUS_PACKET_REQ)
                {
                        ide->atastat = 0x58 | (ide->atastat & ERR_STAT);
//                        pclog("Send data packet 4!\n");
                        ide_irq_raise(ide);
//                        ide.packetstatus=5;
                        ide->pos=2;
                }
                else if (ide->packetstatus == ATAPI_STATUS_PACKET_RECEIVED)
                {
//                        pclog("Packetstatus 5 !\n");
                        atapicommand(ide_board);
                }
                else if (ide->packetstatus == ATAPI_STATUS_READCD) /*READ CD callback*/
                {
                        ide->atastat = DRQ_STAT|(ide->atastat&ERR_STAT);
//                        pclog("Recieve data packet 6!\n");
                        ide_irq_raise(ide);
//                        ide.packetstatus=0xFF;
                }
		else if (ide->packetstatus == ATAPI_STATUS_REQ_SENSE)	/*REQUEST SENSE callback #1*/
		{
			// pclog("REQUEST SENSE callback #1: setting status to 0x5A\n");
			ide->atastat = 0x58 | (ide->atastat & ERR_STAT);
			ide_irq_raise(ide);
		}
                else if (ide->packetstatus == ATAPI_STATUS_ERROR) /*Error callback*/
                {
//                        pclog("Packet error\n");
                        ide->atastat = READY_STAT | ERR_STAT;
                        ide_irq_raise(ide);
                }
                else if (ide->packetstatus == ATAPI_STATUS_ERROR_2) /*Error callback with atastat already set - needed for the disc change stuff.*/
                {
                        //pclog("Packet check status\n");
                        ide->atastat = ERR_STAT;
                        ide_irq_raise(ide);
                }
                return;
        }

abort_cmd:
	ide->command = 0;
	ide->atastat = READY_STAT | ERR_STAT | DSC_STAT;
	ide->error = ABRT_ERR;
	ide->pos = 0;
	ide_irq_raise(ide);
}

void ide_callback_pri()
{
	idecallback[0] = 0;
	callbackide(0);
}

void ide_callback_sec()
{
	idecallback[1] = 0;
	callbackide(1);
}

void ide_callback_ter()
{
	idecallback[2] = 0;
	callbackide(2);
}

/*ATAPI CD-ROM emulation*/
void atapi_cmd_error(IDE *ide, uint8_t sensekey, uint8_t asc, uint8_t ascq)
{
	ide->error = (sensekey << 4);
	ide->atastat = READY_STAT | ERR_STAT;
	ide->secount = (ide->secount & ~7) | 3;
	ide->packetstatus = 0x80;
	idecallback[ide->board]=50*IDE_TIME;
}

uint8_t atapi_prev;
int toctimes=0;

void atapi_command_send_init(IDE *ide, uint8_t command, int req_length, int alloc_length)
{
	if (ide->cylinder == 0xffff)
		ide->cylinder = 0xfffe;

	if ((ide->cylinder & 1) && !(alloc_length <= ide->cylinder))
	{
		pclog("Odd byte count (0x%04x) to ATAPI command 0x%02x, using 0x%04x\n", ide->cylinder, command, ide->cylinder - 1);
		ide->cylinder--;
	}
	
	if (alloc_length < 0)
		fatal("Allocation length < 0\n");
	if (alloc_length == 0)
		alloc_length = ide->cylinder;
		
	// Status: 0x80 (busy), 0x08 (drq), 0x01 (err)
	// Interrupt: 0x02 (i_o), 0x01 (c_d)
	/* No atastat setting: PCem actually emulates the callback cycle. */
	ide->secount = 2;
	
	// no bytes transferred yet
	ide->pos = 0;

	if ((ide->cylinder > req_length) || (ide->cylinder == 0))
		ide->cylinder = req_length;
	if (ide->cylinder > alloc_length)
		ide->cylinder = alloc_length;

	//pclog("atapi_command_send_init(ide, %02X, %04X, %04X)\n", command, req_length, alloc_length);
	//pclog("IDE settings: Pos=%08X, Secount=%08X, Cylinder=%08X\n", ide->pos, ide->secount, ide->cylinder);
}

static void atapi_command_ready(int ide_board, int packlen)
{
	IDE *ide = &ide_drives[cur_ide[ide_board]];
	ide->packetstatus = ATAPI_STATUS_REQ_SENSE;
	idecallback[ide_board]=60*IDE_TIME;
	ide->packlen=packlen;
}

static void atapi_sense_clear(int command, int ignore_ua)
{
	if ((SCSISense.SenseKey == SENSE_UNIT_ATTENTION) || ignore_ua)
	{
		atapi_prev=command;
		SCSISense.SenseKey=0;
		SCSISense.Asc=0;
		SCSISense.Ascq=0;
	}
}

static int cdrom_add_error_and_subchannel(uint8_t *b, int real_sector_type)
{
	if ((cdrom_sector_flags & 0x06) == 0x02)
	{
		/* Add error flags. */
		memcpy(b + cdrom_sector_size, cdrom_sector_buffer.cdrom_sector.c2, 294);
		cdrom_sector_size += 294;
	}
	else if ((cdrom_sector_flags & 0x06) == 0x04)
	{
		/* Add error flags. */
		memcpy(b + cdrom_sector_size, cdrom_sector_buffer.cdrom_sector.c2, 296);
		cdrom_sector_size += 296;
	}
	else if ((cdrom_sector_flags & 0x06) == 0x06)
	{
		pclog("Invalid error flags\n");
		return 0;
	}
	
	/* if (real_sector_type == 1)
	{ */
		if ((cdrom_sector_flags & 0x700) == 0x100)
		{
			memcpy(b + cdrom_sector_size, cdrom_sector_buffer.cdrom_sector.subchannel_raw, 96);
			cdrom_sector_size += 96;
		}
		else if ((cdrom_sector_flags & 0x700) == 0x200)
		{
			memcpy(b + cdrom_sector_size, cdrom_sector_buffer.cdrom_sector.subchannel_q, 16);
			cdrom_sector_size += 16;
		}
		else if ((cdrom_sector_flags & 0x700) == 0x400)
		{
			memcpy(b + cdrom_sector_size, cdrom_sector_buffer.cdrom_sector.subchannel_rw, 96);
			cdrom_sector_size += 96;
		}
		else if (((cdrom_sector_flags & 0x700) == 0x300) || ((cdrom_sector_flags & 0x700) > 0x400))
		{
			pclog("Invalid subchannel data flags\n");
			return 0;
		}
	// }
	
	// pclog("CD-ROM sector size after processing: %i (%i, %i) [%04X]\n", cdrom_sector_size, cdrom_sector_type, real_sector_type, cdrom_sector_flags);

	return cdrom_sector_size;
}

static int cdrom_LBAtoMSF_accurate(IDE *ide)
{
	int temp_pos;
	int m, s, f;
	
	temp_pos = ide->cdpos;
	f = temp_pos % 75;
	temp_pos -= f;
	temp_pos /= 75;
	s = temp_pos % 60;
	temp_pos -= s;
	temp_pos /= 60;
	m = temp_pos;
	
	return ((m << 16) | (s << 8) | f);
}

static int cdrom_read_data(IDE *ide, uint8_t *buffer)
{
	int real_sector_type;
	uint8_t *b;
	uint8_t *temp_b;
	int is_audio;
	int real_pos;
	
	b = temp_b = buffer;
	
	if  (cdrom_sector_ismsf)
	{
		real_pos = cdrom_LBAtoMSF_accurate(ide);
	}
	else
	{
		real_pos = ide->cdpos;
	}

	memset(cdrom_sector_buffer.buffer, 0, 2856);

	cdrom->readsector_raw(cdrom_sector_buffer.buffer, real_pos, cdrom_sector_ismsf);
	is_audio = cdrom->is_track_audio(real_pos, cdrom_sector_ismsf);

	if (!(cdrom_sector_flags & 0xf0))		/* 0x00 and 0x08 are illegal modes */
	{
		return 0;
	}
	
	if (cdrom->is_track_audio(ide->cdpos, 0))
	{
		real_sector_type = 1;
		
		if (cdrom_sector_type > 1)
		{
			return 0;
		}
		else
		{
			memcpy(b, cdrom_sector_buffer.buffer, 2352);
			cdrom_sector_size = 2352;
		}
	}
	else
	{
		if ((cdrom_sector_flags & 0x18) == 0x08)		/* EDC/ECC without user data is an illegal mode */
		{
			return 0;
		}

		cdrom_sector_size = 0;

		if (cdrom_sector_flags & 0x80)		/* Sync */
		{
			memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.sync, 12);
			cdrom_sector_size += 12;
			temp_b += 12;
		}
		if (cdrom_sector_flags & 0x20)		/* Header */
		{
			memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.header, 4);
			cdrom_sector_size += 4;
			temp_b += 4;
		}
		
		switch(cdrom_sector_buffer.cdrom_sector.header[3])
		{
			case 1:
				real_sector_type = 2;	/* Mode 1 */
				break;
			case 2:
				real_sector_type = 3;	/* Mode 2 */
				break;
			default:
				return 0;
		}

		if (real_sector_type == 2)
		{
			if ((cdrom_sector_type == 0) || (cdrom_sector_type == 2))
			{
				/* Mode 1 sector, expected type is 1 type. */
				if (cdrom_sector_flags & 0x40)		/* Sub-header */
				{
					if (!(cdrom_sector_flags & 0x10))		/* No user data */
					{
						memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.data.m1_data.user_data, 8);
						cdrom_sector_size += 8;
						temp_b += 8;
					}
				}
				if (cdrom_sector_flags & 0x10)		/* User data */
				{
					memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.data.m1_data.user_data, 2048);
					cdrom_sector_size += 2048;
					temp_b += 2048;
				}
				if (cdrom_sector_flags & 0x08)		/* EDC/ECC */
				{
					memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.data.m1_data.ecc, 288);
					cdrom_sector_size += 288;
					temp_b += 288;
				}
			}
			else
			{
				return 0;
			}
		}
		else if (real_sector_type == 3)
		{
			if ((cdrom_sector_type == 0) || (cdrom_sector_type == 3))
			{
				/* Mode 2 sector, non-XA mode. */
				if (cdrom_sector_flags & 0x40)		/* Sub-header */
				{
					if (!(cdrom_sector_flags & 0x10))		/* No user data */
					{
						memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.data.m2_data.sub_header, 8);
						cdrom_sector_size += 8;
						temp_b += 8;
					}
				}
				if (cdrom_sector_flags & 0x10)		/* User data */
				{
					memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.data.m2_data.sub_header, 2328);
					cdrom_sector_size += 8;
					temp_b += 8;
					memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.data.m2_data.user_data, 2328);
					cdrom_sector_size += 2328;
					temp_b += 2328;
				}
			}
			else if (cdrom_sector_type == 4)
			{
				/* Mode 2 sector, XA Form 1 mode */
				if ((cdrom_sector_flags & 0xf0) == 0x30)
				{
					return 0;
				}
				if (((cdrom_sector_flags & 0xf8) >= 0xa8) || ((cdrom_sector_flags & 0xf8) <= 0xd8))
				{
					return 0;
				}
				if (cdrom_sector_flags & 0x40)		/* Sub-header */
				{
					memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.data.m2_data.sub_header, 8);
					cdrom_sector_size += 8;
					temp_b += 8;
				}
				if (cdrom_sector_flags & 0x10)		/* User data */
				{
					if ((cdrom_sector_flags & 0xf0) == 0x10)
					{
						/* The data is alone, include sub-header. */
						memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.data.m2_data.sub_header, 8);
						cdrom_sector_size += 8;
						temp_b += 8;
					}
					memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.data.m2_data.user_data, 2040);
					cdrom_sector_size += 2040;
					temp_b += 2040;
				}
				if (cdrom_sector_flags & 0x08)		/* EDC/ECC */
				{
					memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.data.m2_data.user_data + 2040, 288);
					cdrom_sector_size += 288;
					temp_b += 288;
				}
			}
			else if (cdrom_sector_type == 5)
			{
				/* Mode 2 sector, XA Form 2 mode */
				if ((cdrom_sector_flags & 0xf0) == 0x30)
				{
					return 0;
				}
				if (((cdrom_sector_flags & 0xf8) >= 0xa8) || ((cdrom_sector_flags & 0xf8) <= 0xd8))
				{
					return 0;
				}
				/* Mode 2 sector, XA Form 1 mode */
				if (cdrom_sector_flags & 0x40)		/* Sub-header */
				{
					memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.data.m2_data.sub_header, 8);
					cdrom_sector_size += 8;
					temp_b += 8;
				}
				if (cdrom_sector_flags & 0x10)		/* User data */
				{
					memcpy(temp_b, cdrom_sector_buffer.cdrom_sector.data.m2_data.user_data, 2328);
					cdrom_sector_size += 2328;
					temp_b += 2328;
				}
			}
			else
			{
				return 0;
			}
		}
		else
		{
			return 0;
		}
	}

	// pclog("CD-ROM sector size: %i (%i, %i) [%04X]\n", cdrom_sector_size, cdrom_sector_type, real_sector_type, cdrom_sector_flags);
	return cdrom_add_error_and_subchannel(b, real_sector_type);
}

static void atapicommand(int ide_board)
{
	IDE *ide = &ide_drives[cur_ide[ide_board]];
	uint8_t *idebufferb = (uint8_t *) ide->buffer;
	uint8_t rcdmode = 0;
	int c;
	int len;
	int msf;
	int pos=0;
	unsigned char temp;
	uint32_t size;
	uint8_t page_code;
	int max_len;
	unsigned idx = 0;
	unsigned size_idx;
	unsigned preamble_len;
	int toc_format;
	int temp_command;
	int alloc_length;
	int completed;
    uint8_t index = 0;
	int media;
	int format;
	int ret;

#if 0
	pclog("ATAPI command 0x%02X, Sense Key %02X, Asc %02X, Ascq %02X, %i, Unit attention: %i\n",idebufferb[0],SCSISense.SenseKey,SCSISense.Asc,SCSISense.Ascq,ins,SCSISense.UnitAttention);
		
	int CdbLength;
	for (CdbLength = 1; CdbLength < 12; CdbLength++)
		pclog("ATAPI CDB[%d] = 0x%02X\n", CdbLength, idebufferb[CdbLength]);
#endif
	
	msf=idebufferb[1]&2;
	ide->cdlen=0;

	if (cdrom->medium_changed())
	{
		SCSICDROM_Insert();
	}

	if (!cdrom->ready() && SCSISense.UnitAttention)
	{
		/* If the drive is not ready, there is no reason to keep the
			UNIT ATTENTION condition present, as we only use it to mark
			disc changes. */
		SCSISense.UnitAttention = 0;
	}
	
	/* If the UNIT ATTENTION condition is set and the command does not allow
		execution under it, error out and report the condition. */
	if (!(SCSICommandTable[idebufferb[0]] & ALLOW_UA) && SCSISense.UnitAttention)
	{
		// pclog("UNIT ATTENTION: Command not allowed to pass through\n");
		atapi_cmd_error(ide, SENSE_UNIT_ATTENTION, ASC_MEDIUM_MAY_HAVE_CHANGED, 0);
		return;
	}

	if (idebufferb[0] == GPCMD_READ_TOC_PMA_ATIP)
	{
		SCSISense.UnitAttention = 0;
	}

	/* Unless the command is REQUEST SENSE, clear the sense. This will *NOT*
		the UNIT ATTENTION condition if it's set. */
	if (idebufferb[0]!=GPCMD_REQUEST_SENSE)
	{
		atapi_sense_clear(idebufferb[0], 1);
	}

	/* Next it's time for NOT READY. */
	if ((SCSICommandTable[idebufferb[0]] & CHECK_READY) && !cdrom->ready())
	{
		// pclog("Not ready\n");
		atapi_cmd_error(ide, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
		SCSISense.SenseKey = SENSE_NOT_READY;
		SCSISense.Asc = ASC_MEDIUM_NOT_PRESENT;
		SCSISense.Ascq = 0;
		return;
	}

	// pclog("Continuing with command\n");
		
	prev_status = cd_status;
	cd_status = cdrom->status();
	if (((prev_status == CD_STATUS_PLAYING) || (prev_status == CD_STATUS_PAUSED)) && ((cd_status != CD_STATUS_PLAYING) && (cd_status != CD_STATUS_PAUSED)))
	{
		completed = 1;
	}
	else
	{
		completed = 0;
	}

	switch (idebufferb[0])
	{
                case GPCMD_TEST_UNIT_READY:
		ide->packetstatus = ATAPI_STATUS_COMPLETE;
		idecallback[ide_board]=50*IDE_TIME;
                break;

                case GPCMD_REQUEST_SENSE: /* Used by ROS 4+ */
		alloc_length = idebufferb[4];
		temp_command = idebufferb[0];
		atapi_command_send_init(ide, temp_command, 18, alloc_length);
				
                /*Will return 18 bytes of 0*/
                memset(idebufferb,0,512);

		idebufferb[0]=0x80|0x70;

		if ((SCSISense.SenseKey > 0) || (cd_status < CD_STATUS_PLAYING))
		{
			if (completed)
			{
				idebufferb[2]=SENSE_ILLEGAL_REQUEST;
				idebufferb[12]=ASC_AUDIO_PLAY_OPERATION;
				idebufferb[13]=ASCQ_AUDIO_PLAY_OPERATION_COMPLETED;
			}
			else
			{
				idebufferb[2]=SCSISense.SenseKey;
				idebufferb[12]=SCSISense.Asc;
				idebufferb[13]=SCSISense.Ascq;
			}
		}
		else if ((SCSISense.SenseKey == 0) && (cd_status >= CD_STATUS_PLAYING))
		{
			idebufferb[2]=SENSE_ILLEGAL_REQUEST;
			idebufferb[12]=ASC_AUDIO_PLAY_OPERATION;
			idebufferb[13]=(cd_status == CD_STATUS_PLAYING) ? ASCQ_AUDIO_PLAY_OPERATION_IN_PROGRESS : ASCQ_AUDIO_PLAY_OPERATION_PAUSED;
		}
		else
		{
			if (SCSISense.UnitAttention)
			{
				idebufferb[2]=SENSE_UNIT_ATTENTION;
				idebufferb[12]=ASC_MEDIUM_MAY_HAVE_CHANGED;
				idebufferb[13]=0;
			}
		}

		idebufferb[7]=10;

		// pclog("REQUEST SENSE start\n");
		atapi_command_ready(ide_board, 18);

		if (idebufferb[2] == SENSE_UNIT_ATTENTION)
		{
			/* If the last remaining sense is unit attention, clear
				that condition. */
			SCSISense.UnitAttention = 0;
		}
		
		/* Clear the sense stuff as per the spec. */
		atapi_sense_clear(temp_command, 0);
                break;

                case GPCMD_SET_SPEED:
                ide->packetstatus = ATAPI_STATUS_COMPLETE;
                idecallback[ide_board]=50*IDE_TIME;
                break;

		case GPCMD_MECHANISM_STATUS:
		len=(idebufferb[7]<<16)|(idebufferb[8]<<8)|idebufferb[9];

		if (len == 0)
			fatal("Zero allocation length to MECHANISM STATUS not impl.\n");

		atapi_command_send_init(ide, idebufferb[0], 8, alloc_length);
		
		idebufferb[0] = 0;
		idebufferb[1] = 0;
		idebufferb[2] = 0;
		idebufferb[3] = 0;
		idebufferb[4] = 0;
		idebufferb[5] = 1;
		idebufferb[6] = 0;
		idebufferb[7] = 0;
		// len = 8;
			
		atapi_command_ready(ide_board, 8);
		break;	
		
                case GPCMD_READ_TOC_PMA_ATIP:
//                pclog("Read TOC ready? %08X\n",ide);
                toctimes++;
//                if (toctimes==2) output=3;
//                pclog("Read TOC %02X\n",idebufferb[9]);
		toc_format = idebufferb[2] & 0xf;
		if (toc_format == 0)
			toc_format = (idebufferb[9]>>6) & 3;
                switch (toc_format)
                {
                        case 0: /*Normal*/
			// pclog("ATAPI: READ TOC type requested: Normal\n");
                        len=idebufferb[8]+(idebufferb[7]<<8);
                        len=cdrom->readtoc(idebufferb,idebufferb[6],msf,len,0);
                        break;
                        case 1: /*Multi session*/
			// pclog("ATAPI: READ TOC type requested: Multi-session\n");
                        len=idebufferb[8]+(idebufferb[7]<<8);
                        len=cdrom->readtoc_session(idebufferb,msf,len);
                        idebufferb[0]=0; idebufferb[1]=0xA;
                        break;
			case 2: /*Raw*/
			// pclog("ATAPI: READ TOC type requested: Raw TOC\n");
			len=idebufferb[8]+(idebufferb[7]<<8);
			len=cdrom->readtoc_raw(idebufferb,len);
			break;
                        default:
			// pclog("ATAPI: Unknown READ TOC type requested: %i\n", (idebufferb[9]>>6));
                        ide->atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
                        ide->error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
                        if (SCSISense.SenseKey == SENSE_UNIT_ATTENTION)
                                ide->error |= MCR_ERR;
                        ide->packetstatus = ATAPI_STATUS_ERROR;
                        idecallback[ide_board]=50*IDE_TIME;
                        return;
/*                        pclog("Bad read TOC format\n");
                        pclog("Packet data :\n");
                        for (c=0;c<12;c++)
                            pclog("%02X ",idebufferb[c]);
                        pclog("\n");
                        exit(-1);*/
                }
//                pclog("ATAPI buffer len %i\n",len);
                ide->packetstatus = ATAPI_STATUS_DATA;
                ide->cylinder=len;
                ide->secount=2;
                ide->pos=0;
                idecallback[ide_board]=60*IDE_TIME;
                ide->packlen=len;
                return;
                
        case GPCMD_READ_CD:
		case GPCMD_READ_CD_MSF:
//          pclog("Read CD : start LBA %02X%02X%02X%02X Length %02X%02X%02X Flags %02X\n",idebufferb[2],idebufferb[3],idebufferb[4],idebufferb[5],idebufferb[6],idebufferb[7],idebufferb[8],idebufferb[9]);
			if (idebufferb[0] == GPCMD_READ_CD_MSF)
			{
				ide->cdpos=MSFtoLBA(idebufferb[3],idebufferb[4],idebufferb[5]);
				ide->cdlen=MSFtoLBA(idebufferb[6],idebufferb[7],idebufferb[8]);

				ide->cdlen -= ide->cdpos;
				ide->cdlen++;
				
				cdrom_sector_ismsf = 1;
			}
			else
			{
				ide->cdlen=(idebufferb[6]<<16)|(idebufferb[7]<<8)|idebufferb[8];
				ide->cdpos=(idebufferb[2]<<24)|(idebufferb[3]<<16)|(idebufferb[4]<<8)|idebufferb[5];

				cdrom_sector_ismsf = 0;
			}

			cdrom_sector_type = (idebufferb[1] >> 2) & 7;
			cdrom_sector_flags = idebufferb[9] || (((uint32_t) idebufferb[10]) << 8);

			ret = cdrom_read_data(ide, idebufferb);
				
            if (!ret)
            {
                ide->atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
                ide->error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
                if (SCSISense.SenseKey == SENSE_UNIT_ATTENTION)
				{
                    ide->error |= MCR_ERR;
				}
                SCSISense.Asc = ASC_ILLEGAL_OPCODE;
                ide->packetstatus = ATAPI_STATUS_ERROR;
                idecallback[ide_board]=50*IDE_TIME;
                break;
//              pclog("Bad flags bits %02X\n",idebufferb[9]);
//              exit(-1);
            }

//			pclog("Read at %08X %08X\n",ide.cdpos,ide.cdpos*2048);

#ifndef RPCEMU_IDE
            readflash=1;
#endif

            ide->cdpos++;
            ide->cdlen--;
            if (ide->cdlen >= 0)
                ide->packetstatus = ATAPI_STATUS_READCD;
            else
                ide->packetstatus = ATAPI_STATUS_DATA;
            ide->cylinder=cdrom_sector_size;
            ide->secount=2;
            ide->pos=0;
            idecallback[ide_board]=60*IDE_TIME;
            ide->packlen=cdrom_sector_size;
            return;

		case GPCMD_READ_6:
		case GPCMD_READ_10:
		case GPCMD_READ_12:
//                pclog("Read 10 : start LBA %02X%02X%02X%02X Length %02X%02X%02X Flags %02X\n",idebufferb[2],idebufferb[3],idebufferb[4],idebufferb[5],idebufferb[6],idebufferb[7],idebufferb[8],idebufferb[9]);
		cdrom_sector_ismsf = 0;

		if (idebufferb[0] == GPCMD_READ_6)
		{
			ide->cdlen=idebufferb[4];
			ide->cdpos=((((uint32_t) idebufferb[1]) & 0x1f)<<16)|(((uint32_t) idebufferb[2])<<8)|((uint32_t) idebufferb[3]);
		}
		else if (idebufferb[0] == GPCMD_READ_10)
		{
			ide->cdlen=(idebufferb[7]<<8)|idebufferb[8];
			ide->cdpos=(idebufferb[2]<<24)|(idebufferb[3]<<16)|(idebufferb[4]<<8)|idebufferb[5];
		}
		else
		{
			ide->cdlen=(((uint32_t) idebufferb[6])<<24)|(((uint32_t) idebufferb[7])<<16)|(((uint32_t) idebufferb[8])<<8)|((uint32_t) idebufferb[9]);
			ide->cdpos=(((uint32_t) idebufferb[2])<<24)|(((uint32_t) idebufferb[3])<<16)|(((uint32_t) idebufferb[4])<<8)|((uint32_t) idebufferb[5]);
		}

				if (!ide->cdlen)
                {
//                        pclog("All done - callback set\n");
                        ide->packetstatus = ATAPI_STATUS_COMPLETE;
                        idecallback[ide_board]=20*IDE_TIME;
                        break;
                }

				cdrom_sector_type = 0;
				cdrom_sector_flags = 0x10;

				ret = cdrom_read_data(ide, idebufferb);

#ifndef RPCEMU_IDE
                readflash=1;
#endif
                ide->cdpos++;
                ide->cdlen--;
                if (ide->cdlen >= 0)
                        ide->packetstatus = ATAPI_STATUS_READCD;
                else
                        ide->packetstatus = ATAPI_STATUS_DATA;
                ide->cylinder=2048;
                ide->secount=2;
                ide->pos=0;
                idecallback[ide_board]=60*IDE_TIME;
                ide->packlen=2048;
                return;

        case GPCMD_READ_HEADER:
                if (msf)
                {
                        ide->atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
                        ide->error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
                        if (SCSISense.SenseKey == SENSE_UNIT_ATTENTION)
                                ide->error |= MCR_ERR;
                        SCSISense.SenseKey = ASC_ILLEGAL_OPCODE;
                        ide->packetstatus = ATAPI_STATUS_ERROR;
                        idecallback[ide_board]=50*IDE_TIME;
                        break;
//                        pclog("Read Header MSF!\n");
//                        exit(-1);
                }
                for (c=0;c<4;c++) idebufferb[c+4]=idebufferb[c+2];
                idebufferb[0]=1; /*2048 bytes user data*/
                idebufferb[1]=idebufferb[2]=idebufferb[3]=0;
                
                ide->packetstatus = ATAPI_STATUS_DATA;
                ide->cylinder=8;
                ide->secount=2;
                ide->pos=0;
                idecallback[ide_board]=60*IDE_TIME;
                ide->packlen=8;
                return;
                
		case GPCMD_MODE_SENSE_6:
		case GPCMD_MODE_SENSE_10:
		temp_command = idebufferb[0];
		
		if (temp_command == GPCMD_MODE_SENSE_6)
			len=idebufferb[4];
		else
			len=(idebufferb[8]|(idebufferb[7]<<8));

                temp=idebufferb[2] & 0x3F;

		memset(idebufferb, 0, len);
		alloc_length = len;
		// for (c=0;c<len;c++) idebufferb[c]=0;
		if (!(mode_sense_pages[temp] & IMPLEMENTED))
		{
                        // ide->atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
                        ide->error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
                        idecallback[ide_board]=50*IDE_TIME;
			atapi_cmd_error(ide, SCSISense.SenseKey, SCSISense.Asc, SCSISense.Ascq);
			ide->atastat = 0x53;
                        ide->packetstatus = ATAPI_STATUS_ERROR;
			SCSISense.SenseKey = SENSE_ILLEGAL_REQUEST;
                        SCSISense.Asc = ASC_INV_FIELD_IN_CMD_PACKET;
			SCSISense.Ascq = 0;
                        return;
		}
			
		if (temp_command == GPCMD_MODE_SENSE_6)
		{
			len = SCSICDROMModeSense(idebufferb,4,temp);
			idebufferb[0] = len - 1;
			idebufferb[1]=3; /*120mm data CD-ROM*/
		}
        	else
		{
			len = SCSICDROMModeSense(idebufferb,8,temp);
			idebufferb[0]=(len - 2)>>8;
			idebufferb[1]=(len - 2)&255;
			idebufferb[2]=3; /*120mm data CD-ROM*/
		}				

		atapi_command_send_init(ide, temp_command, len, alloc_length);

		atapi_command_ready(ide_board, len);
		return;

		case GPCMD_MODE_SELECT_6:
		case GPCMD_MODE_SELECT_10:
                if (ide->packetstatus == ATAPI_STATUS_PACKET_RECEIVED)
                {
                        ide->atastat = READY_STAT;
                        ide->secount=3;
//                        pclog("Recieve data packet!\n");
                        ide_irq_raise(ide);
                        ide->packetstatus=0xFF;
                        ide->pos=0;
  //                      pclog("Length - %02X%02X\n",idebufferb[0],idebufferb[1]);
//                        pclog("Page %02X length %02X\n",idebufferb[8],idebufferb[9]);
                }
                else
                {
			if (idebufferb[0] == GPCMD_MODE_SELECT_6)
			{
				len=idebufferb[4];
				prefix_len = 6;
			}
			else
			{
				len=(idebufferb[7]<<8)|idebufferb[8];
				prefix_len = 10;
			}
			page_current = idebufferb[2];
			if (page_flags[page_current] & PAGE_CHANGEABLE)
                                page_flags[GPMODE_CDROM_AUDIO_PAGE] |= PAGE_CHANGED;
                        ide->packetstatus = ATAPI_STATUS_PACKET_REQ;
                        ide->cylinder=len;
                        ide->secount=0;
                        ide->pos=0;
                        idecallback[ide_board]=60*IDE_TIME;
                        ide->packlen=len;
/*                        pclog("Waiting for ARM to send packet %i\n",len);
                pclog("Packet data :\n");
                for (c=0;c<12;c++)
                    pclog("%02X ",idebufferb[c]);
                    pclog("\n");*/
                }
                return;

		case GPCMD_GET_CONFIGURATION:
        {
            temp_command = idebufferb[0];
            /* XXX: could result in alignment problems in some architectures */
            len = (idebufferb[7]<<8)|idebufferb[8];
            alloc_length = len;
       
            index = 0;
 
            /* only feature 0 is supported */
            if (idebufferb[2] != 0 || idebufferb[3] != 0)
            {
                ide->atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
                ide->error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
				if (SCSISense.SenseKey == SENSE_UNIT_ATTENTION)
					ide->error |= MCR_ERR;
                SCSISense.Asc = ASC_INV_FIELD_IN_CMD_PACKET;
                ide->packetstatus = ATAPI_STATUS_ERROR;
                idecallback[ide_board]=50*IDE_TIME;
                return;
            }
 
            /*
             * XXX: avoid overflow for io_buffer if len is bigger than
             *      the size of that buffer (dimensioned to max number of
             *      sectors to transfer at once)
             *
             *      Only a problem if the feature/profiles grow.
             */
            if (alloc_length > 512) /* XXX: assume 1 sector */
                alloc_length = 512;
 
            memset(idebufferb, 0, alloc_length);
            /*
             * the number of sectors from the media tells us which profile
             * to use as current.  0 means there is no media
             */
            if (len > CD_MAX_SECTORS )
            {
                idebufferb[6] = (MMC_PROFILE_DVD_ROM >> 8) & 0xff;
                idebufferb[7] = MMC_PROFILE_DVD_ROM & 0xff;
            }
            else if (len <= CD_MAX_SECTORS)
            {
                idebufferb[6] = (MMC_PROFILE_CD_ROM >> 8) & 0xff;
                idebufferb[7] = MMC_PROFILE_CD_ROM & 0xff;
            }
            idebufferb[10] = 0x02 | 0x01; /* persistent and current */
            alloc_length = 12; /* headers: 8 + 4 */
            alloc_length += SCSICDROMSetProfile(idebufferb, &index, MMC_PROFILE_DVD_ROM);
            alloc_length += SCSICDROMSetProfile(idebufferb, &index, MMC_PROFILE_CD_ROM);
            idebufferb[0] = ((alloc_length-4) >> 24) & 0xff;
            idebufferb[1] = ((alloc_length-4) >> 16) & 0xff;
            idebufferb[2] = ((alloc_length-4) >> 8) & 0xff;
            idebufferb[3] = (alloc_length-4) & 0xff;           
 
            atapi_command_send_init(ide, temp_command, len, alloc_length);     
           
            atapi_command_ready(ide_board, len);
        }
        break;
		
		case GPCMD_GET_EVENT_STATUS_NOTIFICATION:
		temp_command = idebufferb[0];
		alloc_length = len;        
                {
                        struct
                        {
                                uint8_t opcode;
                                uint8_t polled;
                                uint8_t reserved2[2];
                                uint8_t class;
                                uint8_t reserved3[2];
                                uint16_t len;
                                uint8_t control;
                        } *gesn_cdb;
            
                        struct
                        {
                                uint16_t len;
                                uint8_t notification_class;
                                uint8_t supported_events;
                        } *gesn_event_header;
                        unsigned int used_len;
            
                        gesn_cdb = (void *)idebufferb;
                        gesn_event_header = (void *)idebufferb;
            
                        /* It is fine by the MMC spec to not support async mode operations */
                        if (!(gesn_cdb->polled & 0x01))
                        {   /* asynchronous mode */
                                /* Only pollign is supported, asynchronous mode is not. */
                                ide->error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
                                idecallback[ide_board]=50*IDE_TIME;
                                atapi_cmd_error(ide, SCSISense.SenseKey, SCSISense.Asc, SCSISense.Ascq);
                                ide->atastat = 0x53;
                                ide->packetstatus=0x80;
                                SCSISense.SenseKey = SENSE_ILLEGAL_REQUEST;
                                SCSISense.Asc = ASC_INV_FIELD_IN_CMD_PACKET;
                                SCSISense.Ascq = 0;
                                return;
                        }
            
                        /* polling mode operation */
            
                        /*
                         * These are the supported events.
                         *
                         * We currently only support requests of the 'media' type.
                         * Notification class requests and supported event classes are bitmasks,
                         * but they are built from the same values as the "notification class"
                         * field.
                         */
                        gesn_event_header->supported_events = 1 << GESN_MEDIA;
            
                        /*
                         * We use |= below to set the class field; other bits in this byte
                         * are reserved now but this is useful to do if we have to use the
                         * reserved fields later.
                         */
                        gesn_event_header->notification_class = 0;
            
                        /*
                         * Responses to requests are to be based on request priority.  The
                         * notification_class_request_type enum above specifies the
                         * priority: upper elements are higher prio than lower ones.
                         */
                        if (gesn_cdb->class & (1 << GESN_MEDIA))
                        {
                                gesn_event_header->notification_class |= GESN_MEDIA;
                                used_len = SCSICDROMEventStatus(idebufferb);
                        }
                        else
                        {
                                gesn_event_header->notification_class = 0x80; /* No event available */
                                used_len = sizeof(*gesn_event_header);
                        }
                        gesn_event_header->len = used_len - sizeof(*gesn_event_header);
                }

                atapi_command_send_init(ide, temp_command, len, alloc_length);

                atapi_command_ready(ide_board, len);
                break;
                
		case GPCMD_READ_DISC_INFORMATION:
		idebufferb[1] = 32;
		idebufferb[2] = 0xe; /* last session complete, disc finalized */
		idebufferb[3] = 1; /* first track on disc */
		idebufferb[4] = 1; /* # of sessions */
		idebufferb[5] = 1; /* first track of last session */
		idebufferb[6] = 1; /* last track of last session */
		idebufferb[7] = 0x20; /* unrestricted use */
		idebufferb[8] = 0x00; /* CD-ROM */
			
		len=34;
		ide->packetstatus = ATAPI_STATUS_DATA;
		ide->cylinder=len;
		ide->secount=2;
		ide->pos=0;
		idecallback[ide_board]=60*IDE_TIME;
		ide->packlen=len;			
		break;

                case GPCMD_PLAY_AUDIO_10:
                case GPCMD_PLAY_AUDIO_12:
                case GPCMD_PLAY_AUDIO_MSF:
		/*This is apparently deprecated in the ATAPI spec, and apparently
                  has been since 1995 (!). Hence I'm having to guess most of it*/
		if (idebufferb[0] == GPCMD_PLAY_AUDIO_10)
		{
			pos=(idebufferb[2]<<24)|(idebufferb[3]<<16)|(idebufferb[4]<<8)|idebufferb[5];
			len=(idebufferb[7]<<8)|idebufferb[8];
		}
		else if (idebufferb[0] == GPCMD_PLAY_AUDIO_MSF)
		{
			pos=(idebufferb[3]<<16)|(idebufferb[4]<<8)|idebufferb[5];
			len=(idebufferb[6]<<16)|(idebufferb[7]<<8)|idebufferb[8];
		}
		else
		{
			pos=(idebufferb[3]<<16)|(idebufferb[4]<<8)|idebufferb[5];
			len=(idebufferb[7]<<16)|(idebufferb[8]<<8)|idebufferb[9];
		}


		if ((cdrom_drive < 1) || (cdrom_drive == CDROM_ISO) || (cd_status <= CD_STATUS_DATA_ONLY) ||
                    !cdrom->is_track_audio(pos, (idebufferb[0] == GPCMD_PLAY_AUDIO_MSF) ? 1 : 0))
                {
                        ide->atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
                        ide->error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
						SCSISense.SenseKey = SENSE_ILLEGAL_REQUEST;
                        SCSISense.Asc = ASC_ILLEGAL_MODE_FOR_THIS_TRACK;
						SCSISense.Ascq = 0;
                        ide->packetstatus = ATAPI_STATUS_ERROR;
                        idecallback[ide_board]=50*IDE_TIME;
						atapi_cmd_error(ide, SCSISense.SenseKey, SCSISense.Asc, SCSISense.Ascq);
                        break;
                }
				
                cdrom->playaudio(pos, len, (idebufferb[0] == GPCMD_PLAY_AUDIO_MSF) ? 1 : 0);
                ide->packetstatus = ATAPI_STATUS_COMPLETE;
                idecallback[ide_board]=50*IDE_TIME;
                break;

                case GPCMD_READ_SUBCHANNEL:
                temp=idebufferb[2]&0x40;
                if (idebufferb[3]!=1)
                {
//                        pclog("Read subchannel check condition %02X\n",idebufferb[3]);
                        ide->atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
                        ide->error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
                        if (SCSISense.SenseKey == SENSE_UNIT_ATTENTION)
                                ide->error |= MCR_ERR;
                        SCSISense.Asc = ASC_ILLEGAL_OPCODE;
                        ide->packetstatus = ATAPI_STATUS_ERROR;
                        idecallback[ide_board]=50*IDE_TIME;
                        break;
/*                        pclog("Bad read subchannel!\n");
                        pclog("Packet data :\n");
                        for (c=0;c<12;c++)
                            pclog("%02X\n",idebufferb[c]);
                        dumpregs();
                        exit(-1);*/
                }
                pos=0;
                idebufferb[pos++]=0;
                idebufferb[pos++]=0; /*Audio status*/
                idebufferb[pos++]=0; idebufferb[pos++]=0; /*Subchannel length*/
                idebufferb[pos++]=1; /*Format code*/
                idebufferb[1]=cdrom->getcurrentsubchannel(&idebufferb[5],msf);
//                pclog("Read subchannel complete - audio status %02X\n",idebufferb[1]);
                len=16;
                if (!temp) len=4;
                ide->packetstatus = ATAPI_STATUS_DATA;
                ide->cylinder=len;
                ide->secount=2;
                ide->pos=0;
                idecallback[ide_board]=1000*IDE_TIME;
                ide->packlen=len;
                break;

				case GPCMD_READ_DVD_STRUCTURE:
				temp_command = idebufferb[0];
				media = idebufferb[1];
				format = idebufferb[7];
 
				len = (((uint32_t) idebufferb[6])<<24)|(((uint32_t) idebufferb[7])<<16)|(((uint32_t) idebufferb[8])<<8)|((uint32_t) idebufferb[9]);
				alloc_length = len;
           
				if (format < 0xff) {
					if (len <= CD_MAX_SECTORS) {
						ide->atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
						ide->error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
						SCSISense.Asc = ASC_INCOMPATIBLE_FORMAT;
						ide->packetstatus = ATAPI_STATUS_ERROR;
						idecallback[ide_board]=50*IDE_TIME;
						break;
					} else {
						ide->atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
						ide->error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
						if (SCSISense.SenseKey == SENSE_UNIT_ATTENTION)
							ide->error |= MCR_ERR;
						SCSISense.Asc = ASC_INV_FIELD_IN_CMD_PACKET;
						ide->packetstatus = ATAPI_STATUS_ERROR;
						idecallback[ide_board]=50*IDE_TIME;
						return;
					}
				}
 
				memset(idebufferb, 0, alloc_length > 256 * 512 + 4 ?
					256 * 512 + 4 : alloc_length);
 
				switch (format) {
					case 0x00 ... 0x7f:
					case 0xff:
						if (media == 0) {
							ret = SCSICDROMReadDVDStructure(format, idebufferb, idebufferb);
 
							if (ret < 0)
								atapi_cmd_error(ide, SENSE_ILLEGAL_REQUEST, -ret, 0);
							else
							{
								atapi_command_send_init(ide, temp_command, len, alloc_length);
								atapi_command_ready(ide_board, len);
							}
							break;
						}
						/* TODO: BD support, fall through for now */
 
					/* Generic disk structures */
					case 0x80: /* TODO: AACS volume identifier */
					case 0x81: /* TODO: AACS media serial number */
					case 0x82: /* TODO: AACS media identifier */
					case 0x83: /* TODO: AACS media key block */
					case 0x90: /* TODO: List of recognized format layers */
					case 0xc0: /* TODO: Write protection status */
					default:
					ide->atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
					ide->error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
					if (SCSISense.SenseKey == SENSE_UNIT_ATTENTION)
						ide->error |= MCR_ERR;
					SCSISense.Asc = ASC_INV_FIELD_IN_CMD_PACKET;
					ide->packetstatus = ATAPI_STATUS_ERROR;
					idecallback[ide_board]=50*IDE_TIME;
					return;
				}
				break;

			case GPCMD_START_STOP_UNIT:
                if (idebufferb[4]!=2 && idebufferb[4]!=3 && idebufferb[4])
                {
                        ide->atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
                        ide->error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
                        if (SCSISense.SenseKey == SENSE_UNIT_ATTENTION)
                                ide->error |= MCR_ERR;
                        SCSISense.Asc = ASC_ILLEGAL_OPCODE;
                        ide->packetstatus = ATAPI_STATUS_ERROR;
                        idecallback[ide_board]=50*IDE_TIME;
                        break;
/*                        pclog("Bad start/stop unit command\n");
                        pclog("Packet data :\n");
                        for (c=0;c<12;c++)
                            pclog("%02X\n",idebufferb[c]);
                        exit(-1);*/
                }
                if (!idebufferb[4])        cdrom->stop();
                else if (idebufferb[4]==2) cdrom->eject();
                else                       cdrom->load();
                ide->packetstatus = ATAPI_STATUS_COMPLETE;
                idecallback[ide_board]=50*IDE_TIME;
                break;
                
                case GPCMD_INQUIRY:
		page_code = idebufferb[2];
		max_len = idebufferb[4];
		alloc_length = max_len;
		temp_command = idebufferb[0];
				
		if (idebufferb[1] & 1)
		{
			preamble_len = 4;
			size_idx = 3;
					
			idebufferb[idx++] = 05;
			idebufferb[idx++] = page_code;
			idebufferb[idx++] = 0;
					
			idx++;
			
			switch (page_code)
			{
				case 0x00:
				idebufferb[idx++] = 0x00;
				idebufferb[idx++] = 0x83;
				break;
        			case 0x83:
				if (idx + 24 > max_len)
				{
					atapi_cmd_error(ide, SENSE_ILLEGAL_REQUEST, ASC_DATA_PHASE_ERROR, 0);
					SCSISense.SenseKey = SENSE_ILLEGAL_REQUEST;
					SCSISense.Asc = ASC_DATA_PHASE_ERROR;
					return;
				}
				idebufferb[idx++] = 0x02;
				idebufferb[idx++] = 0x00;
				idebufferb[idx++] = 0x00;
				idebufferb[idx++] = 20;
				ide_padstr8(idebufferb + idx, 20, "53R141");	/* Serial */
				idx += 20;

				if (idx + 72 > max_len)
				{
					goto atapi_out;
				}
				idebufferb[idx++] = 0x02;
				idebufferb[idx++] = 0x01;
				idebufferb[idx++] = 0x00;
				idebufferb[idx++] = 68;
				ide_padstr8(idebufferb + idx, 8, "86Box"); /* Vendor */
				idx += 8;
				ide_padstr8(idebufferb + idx, 40, "86BoxCD v1.0"); /* Product */
				idx += 40;
				ide_padstr8(idebufferb + idx, 20, "53R141"); /* Product */
				idx += 20;
				
				break;
				default:
				atapi_cmd_error(ide, SENSE_ILLEGAL_REQUEST, ASC_INV_FIELD_IN_CMD_PACKET, 0);
				SCSISense.SenseKey = SENSE_ILLEGAL_REQUEST;
				SCSISense.Asc = ASC_INV_FIELD_IN_CMD_PACKET;
				return;
        		}
		}
		else
		{
			preamble_len = 5;
			size_idx = 4;

			idebufferb[0] = 5; /*CD-ROM*/
			idebufferb[1] = 0x80; /*Removable*/
			idebufferb[2] = 0;
			idebufferb[3] = 0x21;
			idebufferb[4] = 31;
			idebufferb[5] = 0;
			idebufferb[6] = 0;
			idebufferb[7] = 0;
                        ide_padstr8(idebufferb + 8, 8, "86Box"); /* Vendor */
                        ide_padstr8(idebufferb + 16, 16, "86BoxCD"); /* Product */
                        ide_padstr8(idebufferb + 32, 4, emulator_version); /* Revision */
					
                        idx = 36;
		}

atapi_out:
		idebufferb[size_idx] = idx - preamble_len;
                len=idx;
				
		atapi_command_send_init(ide, temp_command, len, alloc_length);

		atapi_command_ready(ide_board, len);				
                break;

                case GPCMD_PREVENT_REMOVAL:
                ide->packetstatus = ATAPI_STATUS_COMPLETE;
                idecallback[ide_board]=50*IDE_TIME;
                break;

                case GPCMD_PAUSE_RESUME:
                if (idebufferb[8]&1) cdrom->resume();
                else                 cdrom->pause();
                ide->packetstatus = ATAPI_STATUS_COMPLETE;
                idecallback[ide_board]=50*IDE_TIME;
                break;

                case GPCMD_SEEK:
                pos=(idebufferb[3]<<16)|(idebufferb[4]<<8)|idebufferb[5];
                cdrom->seek(pos);
                ide->packetstatus = ATAPI_STATUS_COMPLETE;
                idecallback[ide_board]=50*IDE_TIME;
                break;

                case GPCMD_READ_CDROM_CAPACITY:
		atapi_command_send_init(ide, temp_command, 8, 8);
                size = cdrom->size();
                idebufferb[0] = (size >> 24) & 0xff;
                idebufferb[1] = (size >> 16) & 0xff;
                idebufferb[2] = (size >> 8) & 0xff;
                idebufferb[3] = size & 0xff;
                idebufferb[4] = (2048 >> 24) & 0xff;
                idebufferb[5] = (2048 >> 16) & 0xff;
                idebufferb[6] = (2048 >> 8) & 0xff;
                idebufferb[7] = 2048 & 0xff;
                len=8;
		atapi_command_ready(ide_board, len);
                break;
                
                case GPCMD_SEND_DVD_STRUCTURE:
                default:
		ide->atastat = READY_STAT | ERR_STAT;    /*CHECK CONDITION*/
                ide->error = (SENSE_ILLEGAL_REQUEST << 4) | ABRT_ERR;
                if (SCSISense.SenseKey == SENSE_UNIT_ATTENTION)
                        ide->error |= MCR_ERR;
		SCSISense.SenseKey = SENSE_ILLEGAL_REQUEST;
                SCSISense.Asc = ASC_ILLEGAL_OPCODE;
                ide->packetstatus = ATAPI_STATUS_ERROR;
                idecallback[ide_board]=50*IDE_TIME;
                break;

                case GPCMD_STOP_PLAY_SCAN:
                cdrom->stop();
                ide->packetstatus = ATAPI_STATUS_COMPLETE;
                idecallback[ide_board]=50*IDE_TIME;
                break;
/*                default:
                pclog("Bad ATAPI command %02X\n",idebufferb[0]);
                pclog("Packet data :\n");
                for (c=0;c<12;c++)
                    pclog("%02X\n",idebufferb[c]);
                exit(-1);*/
        }
}

static void callnonreadcd(IDE *ide)		/* Callabck for non-Read CD commands */
{
	ide_irq_lower(ide);
	if (ide->pos >= ide->packlen)
	{
		// pclog("Command finished, setting callback\n");
		ide->packetstatus = ATAPI_STATUS_COMPLETE;
		idecallback[ide->board]=20*IDE_TIME;
	}
	else
	{
		// pclog("Command not finished, keep sending data\n");
		ide->atastat = BUSY_STAT;
		ide->packetstatus = ATAPI_STATUS_REQ_SENSE;
		ide->cylinder=2;
		ide->secount=2;
		idecallback[ide->board]=60*IDE_TIME;
	}
}

static void callreadcd(IDE *ide)
{
		int ret;

        ide_irq_lower(ide);
        if (ide->cdlen<=0)
        {
//                pclog("All done - callback set\n");
                ide->packetstatus = ATAPI_STATUS_COMPLETE;
                idecallback[ide->board]=20*IDE_TIME;
                return;
        }
//        pclog("Continue readcd! %i blocks left\n",ide->cdlen);
        ide->atastat = BUSY_STAT;

		ret = cdrom_read_data(ide, (uint8_t *) ide->buffer);

#ifndef RPCEMU_IDE
        readflash=1;
#endif

        ide->cdpos++;
        ide->cdlen--;
        ide->packetstatus = ATAPI_STATUS_READCD;
        ide->cylinder=cdrom_sector_size;
        ide->secount=2;
        ide->pos=0;
        idecallback[ide->board]=60*IDE_TIME;
        ide->packlen=cdrom_sector_size;
}



void ide_write_pri(uint16_t addr, uint8_t val, void *priv)
{
        writeide(0, addr, val);
}
void ide_write_pri_w(uint16_t addr, uint16_t val, void *priv)
{
        writeidew(0, val);
}
void ide_write_pri_l(uint16_t addr, uint32_t val, void *priv)
{
        writeidel(0, val);
}
uint8_t ide_read_pri(uint16_t addr, void *priv)
{
        return readide(0, addr);
}
uint16_t ide_read_pri_w(uint16_t addr, void *priv)
{
        return readidew(0);
}
uint32_t ide_read_pri_l(uint16_t addr, void *priv)
{
        return readidel(0);
}

void ide_write_sec(uint16_t addr, uint8_t val, void *priv)
{
        writeide(1, addr, val);
}
void ide_write_sec_w(uint16_t addr, uint16_t val, void *priv)
{
        writeidew(1, val);
}
void ide_write_sec_l(uint16_t addr, uint32_t val, void *priv)
{
        writeidel(1, val);
}
uint8_t ide_read_sec(uint16_t addr, void *priv)
{
        return readide(1, addr);
}
uint16_t ide_read_sec_w(uint16_t addr, void *priv)
{
        return readidew(1);
}
uint32_t ide_read_sec_l(uint16_t addr, void *priv)
{
        return readidel(1);
}

/* *** REMOVE FROM CODE SUBMITTED TO MAINLINE - START *** */
void ide_write_ter(uint16_t addr, uint8_t val, void *priv)
{
        writeide(2, addr, val);
}
void ide_write_ter_w(uint16_t addr, uint16_t val, void *priv)
{
        writeidew(2, val);
}
void ide_write_ter_l(uint16_t addr, uint32_t val, void *priv)
{
        writeidel(2, val);
}
uint8_t ide_read_ter(uint16_t addr, void *priv)
{
        return readide(2, addr);
}
uint16_t ide_read_ter_w(uint16_t addr, void *priv)
{
        return readidew(2);
}
uint32_t ide_read_ter_l(uint16_t addr, void *priv)
{
        return readidel(2);
}
/* *** REMOVE FROM CODE SUBMITTED TO MAINLINE - END *** */

void ide_pri_enable()
{
        io_sethandler(0x01f0, 0x0008, ide_read_pri, ide_read_pri_w, ide_read_pri_l, ide_write_pri, ide_write_pri_w, ide_write_pri_l, NULL);
        io_sethandler(0x03f6, 0x0001, ide_read_pri, NULL,           NULL,           ide_write_pri, NULL,            NULL           , NULL);
}

void ide_pri_disable()
{
        io_removehandler(0x01f0, 0x0008, ide_read_pri, ide_read_pri_w, ide_read_pri_l, ide_write_pri, ide_write_pri_w, ide_write_pri_l, NULL);
        io_removehandler(0x03f6, 0x0001, ide_read_pri, NULL,           NULL,           ide_write_pri, NULL,            NULL           , NULL);
}

void ide_sec_enable()
{
        io_sethandler(0x0170, 0x0008, ide_read_sec, ide_read_sec_w, ide_read_sec_l, ide_write_sec, ide_write_sec_w, ide_write_sec_l, NULL);
        io_sethandler(0x0376, 0x0001, ide_read_sec, NULL,           NULL,           ide_write_sec, NULL,            NULL           , NULL);
}

void ide_sec_disable()
{
        io_removehandler(0x0170, 0x0008, ide_read_sec, ide_read_sec_w, ide_read_sec_l, ide_write_sec, ide_write_sec_w, ide_write_sec_l, NULL);
        io_removehandler(0x0376, 0x0001, ide_read_sec, NULL,           NULL,           ide_write_sec, NULL,            NULL           , NULL);
}

/* *** REMOVE FROM CODE SUBMITTED TO MAINLINE - START *** */
void ide_ter_enable()
{
        io_sethandler(0x0168, 0x0008, ide_read_ter, ide_read_ter_w, ide_read_ter_l, ide_write_ter, ide_write_ter_w, ide_write_ter_l, NULL);
        io_sethandler(0x036e, 0x0001, ide_read_ter, NULL,           NULL,           ide_write_ter, NULL,            NULL           , NULL);

		ide_ter_enabled = 1;
}

void ide_ter_disable()
{
        io_removehandler(0x0168, 0x0008, ide_read_ter, ide_read_ter_w, ide_read_ter_l, ide_write_ter, ide_write_ter_w, ide_write_ter_l, NULL);
        io_removehandler(0x036e, 0x0001, ide_read_ter, NULL,           NULL,           ide_write_ter, NULL,            NULL           , NULL);

		ide_ter_enabled = 0;
}

void ide_ter_init()
{
		ide_ter_enable();

        timer_add(ide_callback_ter, &idecallback[2], &idecallback[2],  NULL);
}
/* *** REMOVE FROM CODE SUBMITTED TO MAINLINE - END *** */

void ide_init()
{
        ide_pri_enable();
        ide_sec_enable();
        ide_bus_master_read_sector = ide_bus_master_write_sector = NULL;
        
        timer_add(ide_callback_pri, &idecallback[0], &idecallback[0],  NULL);
        timer_add(ide_callback_sec, &idecallback[1], &idecallback[1],  NULL);
}

void ide_set_bus_master(int (*read_sector)(int channel, uint8_t *data), int (*write_sector)(int channel, uint8_t *data), void (*set_irq)(int channel))
{
        ide_bus_master_read_sector = read_sector;
        ide_bus_master_write_sector = write_sector;
        ide_bus_master_set_irq = set_irq;
}
