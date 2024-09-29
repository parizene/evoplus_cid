#include <stdio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h> 
#include "mmc.h"

#define CID_SIZE 16
#define SAMSUNG_VENDOR_OPCODE 62

struct cid_info {
    uint8_t  mid;      // Manufacturer ID
    uint16_t oid;      // OEM/Application ID
    char     pnm[6];   // Product name (5 chars + null terminator)
    uint8_t  prv;      // Product revision
    uint32_t psn;      // Product serial number
    uint16_t mdt;      // Manufacturing date (year and month)
};

int mmc_movi_vendor_cmd(unsigned int arg, int fd) {
	int ret = 0;
	struct mmc_ioc_cmd idata = {0};

	idata.data_timeout_ns = 0x10000000;
	idata.write_flag = 1;
	idata.opcode = SAMSUNG_VENDOR_OPCODE;
	idata.arg = arg;
	idata.flags = MMC_RSP_R1B | MMC_CMD_AC;

	ret = ioctl(fd, MMC_IOC_CMD, &idata);

	return ret;
}

int cid_backdoor(int fd) {
	int ret;

	ret = mmc_movi_vendor_cmd(0xEFAC62EC, fd);
	if (ret) {
		printf("Failed to enter vendor mode. Genuine Samsung Evo Plus?\n");
	} else {
		ret = mmc_movi_vendor_cmd(0xEF50, fd);
		if (ret) {
			printf("Unlock command failed.\n");
		} else {
			ret = mmc_movi_vendor_cmd(0x00DECCEE, fd);
			if (ret) {
				printf("Failed to exit vendor mode.\n");
			}
		}
	}

	return ret;
}

int program_cid(int fd, const unsigned char *cid) {
	int ret;
	struct mmc_ioc_cmd idata = {0};

	idata.data_timeout_ns = 0x10000000;
	idata.write_flag = 1;
	idata.opcode = MMC_PROGRAM_CID;
	idata.arg = 0;
	idata.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;
	idata.blksz = CID_SIZE;
	idata.blocks = 1;
	idata.data_ptr = (__u64)cid;

	ret = ioctl(fd, MMC_IOC_CMD, &idata);
	if (ret) {
		printf("Success! Remove and reinsert SD card to check new CID.\n");
	}

	return ret;
}

void show_cid(const unsigned char *cid) {
	int i;
	for (i = 0; i < CID_SIZE; i++){
		printf("%02x", cid[i]);
	}
	printf("\n");
}

unsigned char crc7(const unsigned char data[], int len) {

	int count;
	unsigned char crc = 0;

	for (count = 0; count <= len; count++) {
		unsigned char dat;
		unsigned char bits;
		if (count == len) {
			dat = 0;
			bits = 7;
		} else {
			dat = data[count];
			bits = 8;
		}
		for (; bits > 0; bits--) {
			crc = (crc << 1) + ( (dat & 0x80) ? 1 : 0 );
			if (crc & 0x80) crc ^= 0x09;
			dat <<= 1;
		}
	   crc &= 0x7f;
	}

	return ((crc << 1) + 1);
}

int parse_serial(const char *str) {

	long val;

	// accept decimal or hex, but not octal
	if ((strlen(str) > 2) && (str[0] == '0') &&
		(((str[1] == 'x')) || ((str[1] == 'X')))) {
		val = strtol(str, NULL, 16);
	} else {
		val = strtol(str, NULL, 10);
	}

	return (int)val;
}

int read_cid_sysfs(const char *device_path, unsigned char *cid) {
    char sysfs_path[256];
    FILE *fp;

    // Extract device name from path
    const char *dev_name = basename((char *)device_path);

    // Construct the sysfs path
    snprintf(sysfs_path, sizeof(sysfs_path), "/sys/block/%s/device/cid", dev_name);

    fp = fopen(sysfs_path, "r");
    if (!fp) {
        perror("Error opening sysfs CID file");
        return -1;
    }

    char cid_str[33]; // CID is 16 bytes, so 32 hex chars + null terminator
    if (fgets(cid_str, sizeof(cid_str), fp) == NULL) {
        perror("Error reading CID from sysfs");
        fclose(fp);
        return -1;
    }
    fclose(fp);

    // Convert hex string to bytes
    for (int i = 0; i < CID_SIZE; i++) {
        sscanf(&cid_str[i * 2], "%2hhx", &cid[i]);
    }

    return 0;
}

void decode_cid(const unsigned char *cid, struct cid_info *info) {
    // Manufacturer ID (MID)
    info->mid = cid[0];

    // OEM/Application ID (OID) - 2 bytes
    info->oid = (cid[1] << 8) | cid[2];

    // Product name (PNM) - 5 characters (40 bits)
    memcpy(info->pnm, &cid[3], 5);
    info->pnm[5] = '\0';  // Null-terminate the product name string

    // Product revision (PRV)
    info->prv = cid[8];

    // Product serial number (PSN) - 4 bytes
    info->psn = (cid[9] << 24) | (cid[10] << 16) | (cid[11] << 8) | cid[12];

    // Manufacturing date (MDT)
    info->mdt = ((cid[13] & 0x0F) << 8) | cid[14];

    // CRC and not used bits are typically ignored for decoding purposes
}

void print_cid_info(const struct cid_info *info) {
    printf("Manufacturer ID: %02X\n", info->mid);
    printf("OEM/Application ID: %c%c\n", (info->oid >> 8) & 0xFF, info->oid & 0xFF);
    printf("Product Name: %s\n", info->pnm);
    printf("Product Revision: %d.%d\n", (info->prv >> 4) & 0xF, info->prv & 0xF);
    printf("Product Serial Number: %lu\n", (unsigned long)info->psn);

    // Decode manufacturing date
    int year = 2000 + ((info->mdt >> 4) & 0xFF);  // Year is offset by 2000
    int month = info->mdt & 0x0F;  // Month is stored as the lower 4 bits
    printf("Manufacturing Date: %04d/%02d\n", year, month);
}

int main(int argc, const char **argv) {
	int fd, ret, i, len;
	unsigned char cid[CID_SIZE] = {0};
  unsigned char current_cid[CID_SIZE];

	if (argc < 2 || argc > 4) {
    printf("Usage:\n");
    printf("  ./evoplus_cid <device>\n");
    printf("  ./evoplus_cid <device> <cid> [serial]\n");
    printf("\n");
    printf("device - sd card block device e.g. /dev/block/mmcblk1\n");
    printf("cid    - new cid, must be in hex (without 0x prefix)\n");
    printf("         It can be 32 chars with checksum or 30 chars without.\n");
    printf("         It will be updated with new serial number if supplied.\n");
    printf("         The checksum is (re)calculated if not supplied or new serial applied.\n");
    printf("serial - optional, can be hex (0x prefixed) or decimal\n");
    printf("         Will be applied to the supplied cid before writing.\n");
    printf("\n");
    printf("If no cid is provided, the program will read and display the current CID.\n");
    printf("Warning: use at your own risk!\n");
    return 0;
  }

  // open device
	fd = open(argv[1], O_RDWR);
	if (fd < 0){
		printf("Unable to open device %s\n", argv[1]);
		return -1;
	}

  ret = read_cid_sysfs(argv[1], current_cid);
  if (ret) {
    printf("Failed to read current CID via sysfs.\n");
    close(fd);
    return -1;
  }

  printf("Current CID: ");
  show_cid(current_cid);

  struct cid_info info;
  decode_cid(current_cid, &info);
  print_cid_info(&info);

  // if only device is provided, exit after displaying CID
  if (argc == 2) {
      close(fd);
      return 0;
  }

	len = strlen(argv[2]);
	if (len != 30 && len != 32) {
		printf("CID should be 30 or 32 chars long!\n");
		return -1;
	}

	// parse new cid
	for (i = 0; i < (len/2); i++){
		ret = sscanf(&argv[2][i*2], "%2hhx", &cid[i]);
		if (!ret){
			printf("CID should be hex (without 0x prefix)!\n");
			return -1;
		}
	}

	// incorporate optional serial number
	if (argc == 4) {
		*((int*)&cid[9]) = htonl(parse_serial(argv[3]));
	}

	// calculate checksum if required
	if (len != 32 || argc == 4) {
		cid[15] = crc7(cid, 15);
	}

	// unlock card
	//ret = 0;
	ret = cid_backdoor(fd);
	if (!ret){
		// write new cid
		printf("Writing new CID: ");
		show_cid(cid);
		ret = program_cid(fd, cid);
		if (!ret){
			printf("Success! Remove and reinsert SD card to check new CID.\n");
		}
	}
	close(fd);

	return 0;
}

