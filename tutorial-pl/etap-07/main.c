/*
 * Stage 7 — what a cartridge is.
 *
 * A game for this console is not one lump of data. It is a file with three
 * parts, in this order:
 *
 *   a header        16 bytes that describe the rest of the file
 *   program memory  the instructions the processor runs
 *   graphics memory the patterns the picture chip draws with
 *
 * The header comes first because a machine that is about to run a cartridge
 * has to know how big the two memories are and where they belong in its
 * address space before it can read a single instruction.
 *
 * This program is a host program, not firmware: it runs on the computer and
 * reads the file from disk, because that is where a cartridge lives before it
 * reaches a console. Its whole job is to take the header apart and print what
 * it found, so that the numbers a real machine acts on become visible.
 *
 * The header format is the one the emulator itself parses when it loads a
 * cartridge. Every constant below is that format and nothing else.
 */
#include <stdint.h>
#include <stdio.h>

/* The first four bytes of every cartridge file, in order. The last of them is
 * an old end-of-file marker, kept because it catches the one mistake that
 * ruins a cartridge silently: reading the file as if it were text, which
 * rewrites bytes above 0x7F and scrambles the program. */
#define HEADER_MAGIC_0  'N'
#define HEADER_MAGIC_1  'E'
#define HEADER_MAGIC_2  'S'
#define HEADER_MAGIC_3  0x1A

#define HEADER_SIZE     16

/* One byte is eight bits, and the header packs several small facts into one
 * byte. These masks pick out the bit we want; the rest of the byte is about
 * other things. */
#define FLAG_MIRRORING   0x01       /* bit 0: 0 = side by side, 1 = stacked */
#define FLAG_BATTERY     0x02       /* bit 1: the cartridge keeps saved games */
#define FLAG_TRAINER     0x04       /* bit 2: 512 extra bytes before the program */
#define FLAG_FOUR_SCREEN 0x08       /* bit 3: extra memory on the cartridge */

#define MAPPER_HIGH_NIBBLE 0xF0     /* the four high bits of a byte */
#define MAPPER_LOW_SHIFT   4        /* move the high half down into place */

/* One bank is a slice of memory. A program bank holds 16 x 1024 bytes and a
 * graphics bank holds 8 x 1024; the header counts banks, not bytes. */
#define PRG_BANK_BYTES  (16u * 1024u)
#define CHR_BANK_BYTES  (8u * 1024u)

/* What the header says about the file. The order of the fields follows the
 * order of the bytes, because the reader is going to compare the two. */
typedef struct {
    uint8_t  prg_banks;             /* how many program banks the file carries */
    uint8_t  chr_banks;             /* how many graphics banks it carries      */
    uint8_t  flags6;
    uint8_t  flags7;
    uint32_t prg_bytes;             /* the same counts, turned into bytes      */
    uint32_t chr_bytes;
    int      mapper;                /* which chip is on the board              */
} rom_header;

/* Print one byte as two hexadecimal digits. A byte is eight bits and one hex
 * digit is four bits, so two digits carry exactly one byte and no more: this
 * is why everything in a file header is written in hex. */
static void print_hex_byte(uint8_t value)
{
    printf("%02X", value);
}

/* Print a count of bytes in a form a human reads without counting zeros.
 * uint32_t is a different type on different machines, so it is widened to
 * unsigned long, which %lu always matches. */
static void print_size(const char *label, uint32_t bytes)
{
    printf("  %-11s %6lu bytes  (%lu KB)\n",
           label, (unsigned long)bytes, (unsigned long)(bytes / 1024u));
}

/* Read the first 16 bytes and take them apart. Returns 0 on success. */
static int read_header(const char *path, rom_header *header)
{
    FILE *file = fopen(path, "rb");         /* "rb" = read, binary, no translation */
    if (file == NULL) {
        printf("cannot open %s\n", path);
        return 1;
    }

    uint8_t raw[HEADER_SIZE];
    if (fread(raw, 1, HEADER_SIZE, file) != HEADER_SIZE) {
        printf("%s is shorter than a cartridge header\n", path);
        fclose(file);
        return 1;
    }
    fclose(file);

    if (raw[0] != HEADER_MAGIC_0 || raw[1] != HEADER_MAGIC_1 ||
        raw[2] != HEADER_MAGIC_2 || raw[3] != HEADER_MAGIC_3) {
        printf("%s does not start with the four bytes every cartridge starts with\n",
               path);
        return 1;
    }

    header->prg_banks = raw[4];
    header->chr_banks = raw[5];
    header->flags6    = raw[6];
    header->flags7    = raw[7];

    header->prg_bytes = (uint32_t)header->prg_banks * PRG_BANK_BYTES;
    header->chr_bytes = (uint32_t)header->chr_banks * CHR_BANK_BYTES;

    /* The chip number is eight bits, split across two bytes: four bits here,
     * four bits there. Shifting the high half into place and adding the low
     * half puts the number back together. */
    header->mapper = (header->flags7 & MAPPER_HIGH_NIBBLE) |
                     ((header->flags6 & MAPPER_HIGH_NIBBLE) >> MAPPER_LOW_SHIFT);

    /* Say what the bytes are, so the fields printed next can be checked
     * against them by eye. */
    printf("first %d bytes: ", HEADER_SIZE);
    for (int i = 0; i < HEADER_SIZE; i++) {
        print_hex_byte(raw[i]);
        printf(i + 1 == HEADER_SIZE ? "\n" : " ");
    }

    return 0;
}

/* Two numbers in the header claim how long the file is: the bank counts and
 * the 16 bytes of the header itself. The only way to find out whether the
 * claim is true is to measure the file, and a file whose numbers do not add up
 * is a file the console would refuse to run. */
static int check_length(const char *path, const rom_header *header)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;                   /* the header pass already reported this */
    }

    fseek(file, 0, SEEK_END);       /* walk to the end of the file */
    long length = ftell(file);      /* and ask where that is */
    fclose(file);

    if (length < 0) {
        return 0;
    }

    uint32_t total = HEADER_SIZE + header->prg_bytes + header->chr_bytes;

    printf("  file is %lu bytes, header plus both memories is %lu\n",
           (unsigned long)length, (unsigned long)total);
    if ((uint32_t)length == total) {
        printf("  the two memories account for the whole file\n");
    } else if ((uint32_t)length > total) {
        printf("  %lu bytes more than the header describes (trainer or extra data)\n",
               (unsigned long)((uint32_t)length - total));
    } else {
        printf("  %lu bytes missing: the header describes more than the file holds\n",
               (unsigned long)(total - (uint32_t)length));
    }

    return 0;
}

static void describe(const rom_header *header)
{
    print_size("program:", header->prg_bytes);
    print_size("graphics:", header->chr_bytes);

    printf("  mapper:      %d\n", (int)header->mapper);

    /* Bit 3 changes the meaning of bit 0: with four screens the two memories
     * inside the console are not shared between the two halves of the picture
     * at all, so there is nothing to mirror. */
    if (header->flags6 & FLAG_FOUR_SCREEN) {
        printf("  mirroring:  four separate screens\n");
    } else if (header->flags6 & FLAG_MIRRORING) {
        printf("  mirroring:  stacked (vertical)\n");
    } else {
        printf("  mirroring:  side by side (horizontal)\n");
    }

    printf("  saves:       %s\n",
           (header->flags6 & FLAG_BATTERY) ? "yes, the cartridge keeps them" : "no");
    printf("  trainer:     %s\n",
           (header->flags6 & FLAG_TRAINER) ? "yes, 512 bytes before the program" : "no");
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: %s FILE.nes\n", argv[0]);
        return 1;
    }

    rom_header header;
    printf("%s\n", argv[1]);

    if (read_header(argv[1], &header) != 0) {
        return 1;
    }

    describe(&header);
    check_length(argv[1], &header);

    return 0;
}
