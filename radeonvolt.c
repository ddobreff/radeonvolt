/**
 * Radeonvolt - (c) 2011 Joerie de Gram <j.de.gram@gmail.com>
 *
 * Radeonvolt is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Radeonvolt is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Radeonvolt.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pci/pci.h>
#include <stdbool.h>

#include "i2c.h"
#include "vt1165.h"

#define RADEON_IO_REGION_SIZE	131072
#define MAX_VOLTAGE		2.00f
#define MAX_CARDS		32

struct mem_ctx {
	int fd;
	void *base;
};

struct card {
	pciaddr_t base;
	int bus;
	char model[256];
	char oem[256];
	u32 subvendor;
	u32 subdevice;
};

struct card *cards;
bool opt_debug = false;

/**
 * Maps PCI device memory to userspace
 * @param mem memory context
 * @param base PCI base address
 * @return zero on success
 */
int map(struct mem_ctx *mem, pciaddr_t base)
{
	mem->fd = open("/dev/mem", O_RDWR);
	if(mem->fd == -1) {
		perror("Failed to open /dev/mem");
		return 1;
	}

	mem->base = mmap(0, RADEON_IO_REGION_SIZE, PROT_READ | PROT_WRITE,
			MAP_SHARED, mem->fd, base & ~(RADEON_IO_REGION_SIZE-1));

	if(mem->base == MAP_FAILED) {
		perror("Failed to map memory");
		return 1;
	}

	return 0;
}

/**
 * Unmaps PCI device memory
 * @param mem memory context
 */
void unmap(struct mem_ctx *mem)
{
	munmap(mem->base, RADEON_IO_REGION_SIZE);
	mem->base = 0;

	close(mem->fd);
	mem->fd = 0;
}

/**
 * Enumerates Radeon cards on the PCI bus
 * @return number of cards found
 */
unsigned int enum_cards()
{
	struct pci_access *pci;
	struct pci_dev *dev;
	struct card *card = NULL;
	u32 subvendor, subdevice;
	char namebuf[255];
	char oembuf[255];
	int i, num_cards = 0;

	pci = pci_alloc();
	pci_init(pci);

	pci_scan_bus(pci);	

	for(dev = pci->devices; dev && num_cards < MAX_CARDS; dev = dev->next) {
		if(dev->device_class == PCI_CLASS_DISPLAY_VGA &&
			dev->vendor_id == 0x1002) {
			card = NULL;

			if (opt_debug) {
				char tmpbuf[255];

				pci_lookup_name(pci, tmpbuf, sizeof(tmpbuf), 
					PCI_LOOKUP_VENDOR, dev->vendor_id);
				printf("Vendor: %s (%x-%x)\n", tmpbuf, dev->vendor_id, dev->device_id);
			}

			subvendor = pci_read_word(dev, PCI_SUBSYSTEM_VENDOR_ID);
			subdevice = pci_read_word(dev, PCI_SUBSYSTEM_ID);

			for(i = 0; i < 6 && !card; i++) {
				if(dev->size[i] == RADEON_IO_REGION_SIZE) {
					pci_lookup_name(pci, namebuf, sizeof(namebuf), 
						PCI_LOOKUP_DEVICE, dev->vendor_id, dev->device_id);
					pci_lookup_name(pci, oembuf, sizeof(oembuf),
						PCI_LOOKUP_SUBSYSTEM | PCI_LOOKUP_VENDOR, subvendor, subdevice);

					card = &cards[num_cards++];
					card->base = dev->base_addr[i];
					card->bus = dev->bus;

					strncpy(card->model, namebuf, sizeof(namebuf));
					card->model[sizeof(namebuf)-1] = '\0';
					strncpy(card->oem, oembuf, sizeof(oembuf));
					card->oem[sizeof(oembuf)-1] = '\0';

					card->subvendor = subvendor;
					card->subdevice = subdevice;
				}
			}
		}
	}

	pci_cleanup(pci);

	return num_cards;
}

/**
 * Initializes Radeon i2c controller
 * @param card card
 * @param mem memory context
 * @return zero if initialization succeeded
 */
int radeon_init(struct card *card, struct mem_ctx *mem, struct rv8xx_i2c *i2c)
{
	if(map(mem, card->base)) {
		return 1;
	}

	i2c->base = mem->base;
	i2c->address = VT1165_ADDRESS;

	i2c_setup(i2c->base);
	i2c_select_bus(i2c->base, 0);

	return 0;
}

/**
 * Unmaps Radeon i2c controller
 * @param mem memory context
 */
void radeon_release(struct mem_ctx *mem)
{
	unmap(mem);
}

/**
 * Prints status info for a given card
 * @param card card
 * @param i2c i2c controller
 */
void show_info(struct card *card, struct rv8xx_i2c *i2c)
{
	unsigned char data;

	printf("Device [%02d]: %s\n", card->bus, card->model);
	if (!opt_debug)
		printf("             %s\n\n", /*card->bus < 10 ? 12 : 13,*/ card->oem);
	else
		printf("             %s (%x-%x)\n\n", /*card->bus < 10 ? 12 : 13, */card->oem, card->subvendor, card->subdevice);

	data = vt1165_device_id(i2c);

	if(data != VT1165_DEVICE_ID) {
		fprintf(stderr, "Unsupported i2c device (%02x)\n", data);
		return;
	} else if (opt_debug)
		fprintf(stderr, "Supported i2c device (%02x)\n", data);
		return;


	if(vt1165_vid_mode(i2c) == 3) {
		float voltage = vt1165_get_voltage(i2c, 2);

		float current = vt1165_avg_current(i2c);
		printf("\tCurrent core voltage: %.4f V\n", voltage);
		printf("\tPresets: %.4f / %.4f / %.4f / %.4f V\n",
			vt1165_get_voltage(i2c, 0),
			vt1165_get_voltage(i2c, 1),
			vt1165_get_voltage(i2c, 2),
			vt1165_get_voltage(i2c, 3));

		printf("\tCore power draw: %.2f A (%.02f W)\n",
			current, (voltage * current));

		printf("\tVRM temperatures: %d / %d / %d C\n",
			vt1165_temp(i2c, 0),
			vt1165_temp(i2c, 1),
			vt1165_temp(i2c, 2));
	} else {
		fprintf(stderr, "Unsupported VID mode\n");
	}
}

void show_usage()
{
	printf("Usage: radeonvolt [options]\n\n");
	printf("Optional arguments:\n");
	printf("  --device  device to query/modify\n");
	printf("  --vcore	 set core voltage (in V)\n");
	printf("  --debug	 show debug messages\n");
	printf("\nExample: radeonvolt --device 0 --vcore 1.0875\n");
}

int main(int argc, char *argv[])
{
	unsigned int num_cards;
	struct mem_ctx mem;
	struct rv8xx_i2c i2c;
	float vddc = -1;
	int device = -1;
	int i;

	for(i = 1; i <= (argc-1); i++) {
		if(!strcmp(argv[i], "--device")) {
			device = atoi(argv[++i]);
		} else if(!strcmp(argv[i], "--vcore")) {
			vddc = atof(argv[++i]);
		} else if(!strcmp(argv[i], "--debug")) {
			opt_debug = true;
		} else {
			show_usage();
			return 1;
		}
	}

	if ((device == -1) ^ (vddc == -1)) {
		printf("Error: please specify device and core voltage\n");
		return 1;
	}

	cards = malloc(sizeof(struct card) * MAX_CARDS);
	num_cards = enum_cards();

	for(i = 0; i < num_cards; i++) {
		if(device == cards[i].bus || device == -1) {
			if(radeon_init(&cards[i], &mem, &i2c)) {
				free(cards);
				return 1;
			}

			if(device == cards[i].bus || num_cards == 1) {
				/* Set vddc if required */
				if(vddc > 0) {
					if(vddc < MAX_VOLTAGE) {
						u8 value = (u8)((vddc - 0.450f) / 0.0125f);
						printf("Setting vddc of device %d to %.4f V (%#02x)\n", cards[i].bus, vddc, value);
						vt1165_set_voltage(&i2c, 2, value);
					} else {
						fprintf(stderr, "Specified vddc (%.4f V) doesn't seem sane. Not setting..\n", vddc);
					}
				}
			}

			printf("\n");

			show_info(&cards[i], &i2c);

			printf("\n");

			radeon_release(&mem);
		}
	}

	free(cards);

	return 0;
}
