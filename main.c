#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/mman.h>


// default Program Counter start position
#define PC_START 0x3000


uint16_t memory[UINT16_MAX];


// CPU Registers
enum {
  R0 = 0,
  R1,
  R2,
  R3,
  R4,
  R5,
  R6,
  R7,
  R_PC,
  R_COND,
  R_COUNT
};

uint16_t registers[R_COUNT];


// Condition (R_COND) Flags
enum {
  FLG_POS = 1 << 0,
  FLG_ZRO = 1 << 1,
  FLG_NEG = 1 << 2,
};


// Opcodes
enum {
  OP_BR = 0, // branch
  OP_ADD,    // add
  OP_LD,     // load
  OP_ST,     // store
  OP_JSR,    // jump register
  OP_AND,    // bitwise and
  OP_LDR,    // load register
  OP_STR,    // store register
  OP_RTI,    // unused
  OP_NOT,    // bitwise not
  OP_LDI,    // load indirect
  OP_STI,    // store indirect
  OP_JMP,    // jump
  OP_RES,    // reserved (unused)
  OP_LEA,    // load effective address
  OP_TRAP    // execute trap
};


// Trap Routines
enum
{
    TRAP_GETC  = 0x20,  // get character from keyboard, not echoed onto the terminal
    TRAP_OUT   = 0x21,   // output a character
    TRAP_PUTS  = 0x22,  // output a word string
    TRAP_IN    = 0x23,    // get character from keyboard, echoed onto the terminal
    TRAP_PUTSP = 0x24, // output a byte string
    TRAP_HALT  = 0x25   // halt the program
};


enum {
    MR_KBSR = 0xFE00, // keyboard status
    MR_KBDR = 0xFE02  // keyboard data
};


struct termios original_tio;


void disable_input_buffering() {
    tcgetattr(STDIN_FILENO, &original_tio);
    struct termios new_tio = original_tio;

    new_tio.c_lflag &= ~ICANON & ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}


void restore_input_buffering() {
    tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);
}


void handle_interrupt(int signal) {
    restore_input_buffering();
    printf("\n");
    exit(-2);
}


uint16_t check_key() {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    return select(1, &readfds, NULL, NULL, &timeout) != 0;
}


uint16_t swap16(uint16_t x) {
    return (x << 8) | (x >> 8);
}


void read_image_file(FILE* file) {
    uint16_t origin;
    fread(&origin, sizeof(origin), 1, file);
    origin = swap16(origin);

    uint16_t max_read = UINT16_MAX - origin;
    uint16_t *p = memory + origin;
    size_t read = fread(p, sizeof(uint16_t), max_read, file);

    while (read-- > 0) {
        *p = swap16(*p);
        ++p;
    }
}


int read_image(const char* image_path) {
    FILE* file = fopen(image_path, "rb");

    if (!file) {
        return 0;
    }

    read_image_file(file);
    fclose(file);

    return 1;
}

/*
 * Memory access procedures
*/
void mem_write(uint16_t address, uint16_t value) {
    memory[address] = value;
}


uint16_t mem_read(uint16_t address) {
    if (address == MR_KBSR) {
        if (check_key()) {
            memory[MR_KBSR] = (1 << 15);
            memory[MR_KBDR] = getchar();
        }
        else {
            memory[MR_KBSR] = 0;
        }
    }

    return memory[address];
}


/*
 * CPU Procedures
*/

uint16_t sign_extend(uint16_t x, int num_bits) {
    if ((x >> (num_bits - 1)) & 1) {
        x |= (0xFFFF << num_bits);
    }
    return x;
}


void update_flags(uint16_t register_) {
    if (registers[register_] == 0) {
        registers[R_COND] = FLG_ZRO;
    }
    else if (registers[register_] >> 15) {
        registers[R_COND] = FLG_NEG;
    }
    else {
        registers[R_COND] = FLG_POS;
    }
}


void add(uint16_t *instruction) {
    uint16_t register0      = (*instruction >> 9) & 0x7; // DR
    uint16_t register1      = (*instruction >> 6) & 0x7; // SR1
    uint16_t immediate_flag = (*instruction >> 5) & 0x1;

    if (immediate_flag) {
        uint16_t imm5 = sign_extend(*instruction & 0x1F, 5);
        registers[register0] = registers[register1] + imm5;
    } else {
        uint16_t register2 = *instruction & 0x7;
        registers[register0] = registers[register1] + registers[register2];
    }

    update_flags(register0);
}


void ldi(uint16_t *instruction) {
    uint16_t register0 = (*instruction >> 9) & 0x7;            // DR
    uint16_t pc_offset = sign_extend(*instruction & 0x1FF, 9); // PC offset 9

    registers[register0] = mem_read(mem_read(registers[R_PC] + pc_offset));
    update_flags(register0);
}


void and(uint16_t *instruction) {
    uint16_t register0      = (*instruction >> 9) & 0x7;
    uint16_t register1      = (*instruction >> 6) & 0x7;
    uint16_t immediate_flag = (*instruction >> 5) & 0x1;

    if (immediate_flag) {
        uint16_t imm5 = sign_extend(*instruction & 0x1F, 5);
        registers[register0] = registers[register1] & imm5;
    }
    else {
        uint16_t register2 = *instruction & 0x7;
        registers[register0] = registers[register1] & registers[register2];
    }

    update_flags(register0);
}


void not(uint16_t *instruction) {
    uint16_t register0 = (*instruction >> 9) & 0x7;
    uint16_t register1 = (*instruction >> 6) & 0x7;

    registers[register0] = ~registers[register1];
    update_flags(register0);
}


void br(uint16_t *instruction) {
    uint16_t pc_offset = sign_extend(*instruction & 0x1FF, 9);
    uint16_t cond_flag = (*instruction >> 9) & 0x7;

    if (cond_flag & registers[R_COND]) {
        registers[R_PC] += pc_offset;
    }
}


void jmp(uint16_t *instruction) {
    uint16_t register1 = (*instruction >> 6) & 0x7;
    registers[R_PC]    = registers[register1];
}


void jsr(uint16_t *instruction) {
    uint16_t long_flag = (*instruction >> 11) & 1;
    registers[R7] = registers[R_PC];

    if (long_flag) {
        uint16_t long_pc_offset = sign_extend(*instruction & 0x7FF, 11);
        registers[R_PC] += long_pc_offset; // JSR
    }
    else {
        uint16_t register1 = (*instruction >> 6) & 0x7;
        registers[R_PC] = registers[register1]; // JSRR
    }
}


void ld(uint16_t *instruction) {
    uint16_t register0 = (*instruction >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(*instruction & 0x1FF, 9);

    registers[register0] = mem_read(registers[R_PC] + pc_offset);
    update_flags(register0);
}


void ldr(uint16_t *instruction) {
    uint16_t register0 = (*instruction >> 9) & 0x7;
    uint16_t register1 = (*instruction >> 6) & 0x7;
    uint16_t offset = sign_extend(*instruction & 0x3F, 6);

    registers[register0] = mem_read(registers[register1] + offset);
    update_flags(register0);
}


void lea(uint16_t *instruction) {
    uint16_t register0 = (*instruction >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(*instruction & 0x1FF, 9);

    registers[register0] = registers[R_PC] + pc_offset;
    update_flags(register0);
}


void st(uint16_t *instruction) {
    uint16_t register0 = (*instruction >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(*instruction & 0x1FF, 9);

    mem_write(registers[R_PC] + pc_offset, registers[register0]);
}


void sti(uint16_t *instruction) {
    uint16_t register0 = (*instruction >> 9) & 0x7;
    uint16_t pc_offset = sign_extend(*instruction & 0x1FF, 9);

    mem_write(mem_read(registers[R_PC] + pc_offset), registers[register0]);
}


void str(uint16_t *instruction) {
    uint16_t register0 = (*instruction >> 9) & 0x7;
    uint16_t register1 = (*instruction >> 6) & 0x7;
    uint16_t offset = sign_extend(*instruction & 0x3F, 6);

    mem_write(registers[register1] + offset, registers[register0]);
}

/*
 * TRAP codes procedures implementation
*/

void trap_puts() {
    uint16_t *char_ = memory + registers[R0];

    while (*char_) {
        putc((char) *char_, stdout);
        ++char_;
    }

    fflush(stdout);
}


void trap_getc() {
    registers[R0] = (uint16_t) getchar();
}


void trap_out() {
    putc((char) registers[R0], stdout);
    fflush(stdout);
}


void trap_in() {
    printf("Digite um caractere: ");
    char char_ = getchar();
    putc(char_, stdout);
    registers[R0] = (uint16_t) char_;
}


void trap_putsp() {
    uint16_t *char_ = memory + registers[R0];

    while (*char_) {
        char char1 = (*char_) & 0xFF;
        putc(char1, stdout);

        char char2 = (*char_) >> 8;
        if (char2) putc(char2, stdout);

        ++char_;
    }

    fflush(stdout);
}


void halt(bool **running) {
    puts("HALT");
    fflush(stdout);
    *running = false;
}


void trap(uint16_t *instruction, bool *running) {
    switch (*instruction & 0xFF) {
        case TRAP_GETC:
            trap_getc();
            break;
        case TRAP_OUT:
            trap_out();
            break;
        case TRAP_PUTS:
            trap_puts();
            break;
        case TRAP_IN:
            trap_in();
            break;
        case TRAP_PUTSP:
            trap_putsp();
            break;
        case TRAP_HALT:
            halt(&running);
            break;
    }
}


int main(int argc, const char *argv[]) {
    if (argc < 2) {
        printf("Forma de usar:\n ./lc3 /path/to/image.obj \n");
        exit(2);
    }

    for (int i = 1; i < argc; ++i) {
        if (!read_image(argv[i])) {
            printf("falha ao carregar a imagem: %s\n", argv[i]);
            exit(1);
        }
    }

    signal(SIGINT, handle_interrupt);
    disable_input_buffering();

    registers[R_PC] = PC_START;

    bool running = true;

    while (running) {
        uint16_t instruction = mem_read(registers[R_PC]++);
        uint16_t opcode = instruction >> 12;

        switch (opcode) {
            case OP_ADD:
                add(&instruction);
                break;
            case OP_AND:
                and(&instruction);
                break;
            case OP_NOT:
                not(&instruction);
                break;
            case OP_BR:
                br(&instruction);
                break;
            case OP_JMP:
                jmp(&instruction);
                break;
            case OP_JSR:
                jsr(&instruction);
                break;
            case OP_LD:
                ld(&instruction);
                break;
            case OP_LDI:
                ldi(&instruction);
                break;
            case OP_LDR:
                ldr(&instruction);
                break;
            case OP_LEA:
                lea(&instruction);
                break;
            case OP_ST:
                st(&instruction);
                break;
            case OP_STI:
                sti(&instruction);
                break;
            case OP_STR:
                str(&instruction);
                break;
            case OP_TRAP:
                trap(&instruction, &running);
                break;
            case OP_RES:
                abort();
                break;
            case OP_RTI:
                abort();
                break;
            default:
                abort();
                break;
      }

    }
    return 0;
}
